// SPDX-License-Identifier: Apache-2.0
// Bytecode fast path for hot code.
//
// An action, or a single loop statement, compiles to register bytecode in which every variable
// is a numbered slot instead of a name looked up in a hash map. Only a subset compiles: ints,
// floats, bools and strings, lists and maps, operators, if / while / do / for / for-each /
// repeat, break / continue / return, print, string interpolation, calls to script actions,
// `string.*` / `math.*` builtins with a plain result and int() / float() / string() / bool().
// Anything else (structs, pointers, closures, entities, ...) makes the compile fail and the
// tree-walker runs that code as before.
//
// Operations reuse the interpreter's helpers (apply_binary, eval_binary_slow,
// to_display_string, collection_method, the builtin dispatcher), so compiled code prints and
// fails exactly like the tree-walker. Values only the tree-walker understands (heap / weak /
// file handles, struct markers) never enter a compiled frame: call arguments and loop
// variables are checked on entry.
//
// Lists and maps may hold such values too. Code that reads collection contents ("aware" code)
// therefore scans every list and map it receives from the tree-walker, and calls only other
// compiled code that does the same, so no tree-walker code can store into a collection while
// an aware frame runs.

#include "erelang/builtin_registry.hpp"
#include "erelang/parser.hpp"
#include "erelang/runtime.hpp"
#include "erelang/runtime_helpers.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#if defined(__GNUC__)
#define ERELANG_NOINLINE __attribute__((noinline, cold))
#elif defined(_MSC_VER)
#define ERELANG_NOINLINE __declspec(noinline)
#else
#define ERELANG_NOINLINE
#endif

namespace erelang {

namespace {

// Every op in enum order; the same list builds the dispatch table in FastVm::exec.
#define ERELANG_FAST_OPS(X) \
    X(Move)         /* a = b */ \
    X(Bin)          /* a = b <sub> c; d indexes Code::bins (tree-walker path for non-scalars) */ \
    X(Unary)        /* a = <sub> b */ \
    X(Short)        /* sub 0 &&, 1 ||, 2 ??: when b decides the result, a = result, jump to c */ \
    X(Jmp)          /* jump to a */ \
    X(JmpFalse)     /* if !truthy(a) jump to b */ \
    X(JmpTrue)      /* if truthy(a) jump to b */ \
    X(CmpJmpFalse)  /* if !truthy(a <sub> b) jump to c; d as for Bin */ \
    X(CmpJmpTrue)   /* if truthy(a <sub> b) jump to c; d as for Bin */ \
    X(Print)        /* print a */ \
    X(Concat)       /* a = display(b) + ... + display(b + c - 1) */ \
    X(IncInt)       /* a = as_int(a) + b (statement x++ / x--) */ \
    X(CheckType)    /* throw unless a matches Code::lets[b] */ \
    X(ToCount)      /* a = to_int(display(b)) (repeat count) */ \
    X(Call)         /* a = Code::calls[b](c .. c + d - 1) */ \
    X(Builtin)      /* a = Code::builtins[b](c .. c + d - 1); sub 1 int(), 2 float() try a native path */ \
    X(NewList)      /* a = [b .. b + c - 1] */ \
    X(NewDict)      /* a = new map reserving b entries */ \
    X(DictPut)      /* map a: key display(b) = c */ \
    X(Range)        /* a = b .. c (sub 1: ..<) */ \
    X(Index)        /* a = b[c] */ \
    X(IndexSet)     /* b[c] = a */ \
    X(Method)       /* a = c.Code::methods[b](d ..) */ \
    X(IterInit)     /* Code iterator b starts over a */ \
    X(IterNext)     /* next of iterator b into a (and d when sub is 1); jump to c when done */ \
    X(Ret)          /* return a */ \
    X(End) \
    /* Specialized forms made from Bin and CmpJmp* by Compiler::specialize: an inline int */ \
    /* path, otherwise exactly the generic op (sub and d are kept). K: the right operand */ \
    /* is a constant register holding an int. */ \
    X(AddInt) X(SubInt) X(MulInt) \
    X(AddIntK) X(SubIntK) X(MulIntK) \
    /* Jump to c when a <op> b for ints; sub is the original op | kJumpWhenTrue. */ \
    X(JmpLt) X(JmpLe) X(JmpGt) X(JmpGe) X(JmpEq) X(JmpNe) \
    X(JmpLtK) X(JmpLeK) X(JmpGtK) X(JmpGeK) X(JmpEqK) X(JmpNeK) \
    /* A loop step fused with the JmpLt / JmpLe (or K form) right after it: IncInt or AddIntK */ \
    /* as usual, then for ints that jump is decided here; otherwise it runs next as normal. */ \
    X(IncIntLt) X(IncIntLe) X(AddIntKLt) X(AddIntKLe)

enum class Op : uint8_t {
#define ERELANG_OP_ENUM(name) name,
    ERELANG_FAST_OPS(ERELANG_OP_ENUM)
#undef ERELANG_OP_ENUM
};

#define ERELANG_OP_COUNT(name) +1
constexpr std::size_t kOpCount = 0 ERELANG_FAST_OPS(ERELANG_OP_COUNT);
#undef ERELANG_OP_COUNT

constexpr uint8_t kJumpWhenTrue = 0x80;

struct Ins {
    Op op;
    uint8_t sub{0};
    int32_t a{0}, b{0}, c{0}, d{0};
};

int64_t wrap_add(int64_t a, int64_t b) {
    return static_cast<int64_t>(static_cast<uint64_t>(a) + static_cast<uint64_t>(b));
}
int64_t wrap_sub(int64_t a, int64_t b) {
    return static_cast<int64_t>(static_cast<uint64_t>(a) - static_cast<uint64_t>(b));
}
int64_t wrap_mul(int64_t a, int64_t b) {
    return static_cast<int64_t>(static_cast<uint64_t>(a) * static_cast<uint64_t>(b));
}

ValueBinOp to_vbin(BinOp op) {
    switch (op) {
        case BinOp::Add: return ValueBinOp::Add;
        case BinOp::Sub: return ValueBinOp::Sub;
        case BinOp::Mul: return ValueBinOp::Mul;
        case BinOp::Div: return ValueBinOp::Div;
        case BinOp::Mod: return ValueBinOp::Mod;
        case BinOp::Pow: return ValueBinOp::Pow;
        case BinOp::EQ: return ValueBinOp::Eq;
        case BinOp::NE: return ValueBinOp::Ne;
        case BinOp::LT: return ValueBinOp::Lt;
        case BinOp::LE: return ValueBinOp::Le;
        case BinOp::GT: return ValueBinOp::Gt;
        case BinOp::GE: return ValueBinOp::Ge;
        case BinOp::And: return ValueBinOp::And;
        case BinOp::Or: return ValueBinOp::Or;
        case BinOp::Coalesce: return ValueBinOp::Coalesce;
        case BinOp::BitAnd: return ValueBinOp::BitAnd;
        case BinOp::BitXor: return ValueBinOp::BitXor;
        case BinOp::BitOr: return ValueBinOp::BitOr;
        case BinOp::Shl: return ValueBinOp::Shl;
        case BinOp::Shr: return ValueBinOp::Shr;
        case BinOp::StrictEQ: return ValueBinOp::StrictEq;
        case BinOp::StrictNE: return ValueBinOp::StrictNe;
    }
    return ValueBinOp::Add;
}

bool is_comparison(BinOp op) {
    switch (op) {
        case BinOp::EQ: case BinOp::NE: case BinOp::LT: case BinOp::LE:
        case BinOp::GT: case BinOp::GE: case BinOp::StrictEQ: case BinOp::StrictNE:
            return true;
        default:
            return false;
    }
}

void set_int(Value& v, int64_t x) {
    if (v.str) v.str.reset();
    v.kind = ValueKind::Int;
    v.i = x;
}

void set_bool(Value& v, bool x) {
    if (v.str) v.str.reset();
    v.kind = ValueKind::Bool;
    v.i = 0;
    v.b = x;
}

bool is_scalar(const Value& v) {
    return v.kind == ValueKind::Int || v.kind == ValueKind::Float ||
           v.kind == ValueKind::Bool || v.kind == ValueKind::Null;
}

// Both ints: the tree-walker's int_binary in apply_binary, inlined. False when op is not one.
bool int_binary(uint8_t op, int64_t a, int64_t b, Value& dst) {
    switch (static_cast<ValueBinOp>(op)) {
        case ValueBinOp::Add: set_int(dst, wrap_add(a, b)); return true;
        case ValueBinOp::Sub: set_int(dst, wrap_sub(a, b)); return true;
        case ValueBinOp::Mul: set_int(dst, wrap_mul(a, b)); return true;
        case ValueBinOp::Div:
            if (b == 0) throw std::runtime_error("Division by zero");
            set_int(dst, b == -1 ? wrap_sub(0, a) : a / b);  // INT64_MIN / -1 traps on x86
            return true;
        case ValueBinOp::Mod:
            if (b == 0) throw std::runtime_error("Modulo by zero");
            set_int(dst, b == -1 ? 0 : a % b);
            return true;
        case ValueBinOp::Eq: case ValueBinOp::StrictEq: set_bool(dst, a == b); return true;
        case ValueBinOp::Ne: case ValueBinOp::StrictNe: set_bool(dst, a != b); return true;
        case ValueBinOp::Lt: set_bool(dst, a < b); return true;
        case ValueBinOp::Le: set_bool(dst, a <= b); return true;
        case ValueBinOp::Gt: set_bool(dst, a > b); return true;
        case ValueBinOp::Ge: set_bool(dst, a >= b); return true;
        case ValueBinOp::BitAnd: set_int(dst, a & b); return true;
        case ValueBinOp::BitOr: set_int(dst, a | b); return true;
        case ValueBinOp::BitXor: set_int(dst, a ^ b); return true;
        default: return false;
    }
}

bool int_compare(uint8_t op, int64_t a, int64_t b) {
    switch (static_cast<ValueBinOp>(op)) {
        case ValueBinOp::Eq: case ValueBinOp::StrictEq: return a == b;
        case ValueBinOp::Ne: case ValueBinOp::StrictNe: return a != b;
        case ValueBinOp::Lt: return a < b;
        case ValueBinOp::Le: return a <= b;
        case ValueBinOp::Gt: return a > b;
        default: return a >= b;
    }
}

void set_float(Value& v, double x) {
    if (v.str) v.str.reset();
    v.kind = ValueKind::Float;
    v.f = x;
}

// An int and a float, or two floats: the float branches of apply_binary, inlined.
bool mixed_operands(const Value& x, const Value& y) {
    return (x.kind == ValueKind::Float && (y.kind == ValueKind::Float || y.kind == ValueKind::Int)) ||
           (x.kind == ValueKind::Int && y.kind == ValueKind::Float);
}
bool is_number(const Value& v) { return v.kind == ValueKind::Int || v.kind == ValueKind::Float; }
double as_double(const Value& v) { return v.kind == ValueKind::Float ? v.f : static_cast<double>(v.i); }

// Comparison of mixed operands; false in `holds` when op is not a comparison.
bool float_compare(uint8_t op, const Value& x, const Value& y, bool& holds) {
    const double a = as_double(x), b = as_double(y);
    switch (static_cast<ValueBinOp>(op)) {
        case ValueBinOp::Eq: holds = a == b; return true;
        case ValueBinOp::Ne: holds = a != b; return true;
        case ValueBinOp::StrictEq: holds = x.kind == y.kind && a == b; return true;
        case ValueBinOp::StrictNe: holds = !(x.kind == y.kind && a == b); return true;
        case ValueBinOp::Lt: holds = a < b; return true;
        case ValueBinOp::Le: holds = a <= b; return true;
        case ValueBinOp::Gt: holds = a > b; return true;
        case ValueBinOp::Ge: holds = a >= b; return true;
        default: return false;
    }
}

bool float_binary(uint8_t op, const Value& x, const Value& y, Value& dst) {
    const double a = as_double(x), b = as_double(y);
    switch (static_cast<ValueBinOp>(op)) {
        case ValueBinOp::Add: set_float(dst, a + b); return true;
        case ValueBinOp::Sub: set_float(dst, a - b); return true;
        case ValueBinOp::Mul: set_float(dst, a * b); return true;
        case ValueBinOp::Div:
            if (b == 0.0) throw std::runtime_error("Division by zero");
            set_float(dst, a / b);
            return true;
        case ValueBinOp::Mod:
            if (b == 0.0) throw std::runtime_error("Modulo by zero");
            set_float(dst, std::fmod(a, b));
            return true;
        case ValueBinOp::Pow: set_float(dst, std::pow(a, b)); return true;
        default: {
            bool holds = false;
            if (!float_compare(op, x, y, holds)) return false;
            set_bool(dst, holds);
            return true;
        }
    }
}

constexpr uint8_t kKindList = 1;
constexpr uint8_t kKindDict = 2;

constexpr uint8_t kListLen = 1;
constexpr uint8_t kListPush = 2;
constexpr uint8_t kListGet = 3;

uint8_t collection_kind(const Value& v) {
    if (v.kind != ValueKind::Handle) return 0;
    if (v.h.kind == HandleKind::List) return kKindList;
    if (v.h.kind == HandleKind::Dict) return kKindDict;
    return 0;
}

// The collection kind a declared type guarantees once its runtime check passed: Array<T> is a
// list, Map<K, V> a map. 0 when the type admits other values too (any, unknown names, ...).
uint8_t declared_collection_kind(const std::string& type) {
    if (type.empty() || runtime_declared_type_matches(type, "int") || runtime_declared_type_matches(type, "string")) {
        return 0;
    }
    const bool list = runtime_declared_type_matches(type, "array<any>");
    const bool dict = runtime_declared_type_matches(type, "map<string,any>");
    if (list != dict) return list ? kKindList : kKindDict;
    return 0;
}

// A value a compiled frame may hold: anything the tree-walker treats as plain data, plus list
// and map handles (their contents are checked separately, see admit()). Other handles and
// handle-like or struct-marker strings carry ownership, scope or field semantics.
bool vm_safe(const Value& v) {
    if (v.kind == ValueKind::Handle) return collection_kind(v) != 0;
    if (v.kind != ValueKind::String) return true;
    if (value_is_struct_marker(v)) return false;
    Handle h;
    return !try_parse_legacy_handle(v.string_ref(), h);
}

// Scanning is bounded by the collection work aware code does: all scans together may visit
// kScanBase elements plus kScanPerOp for every collection operation executed. Past that, entries
// that need a scan run on the tree-walker, so repeatedly entering with a large list (a binary
// search called from interpreted code) cannot cost more than the work it replaces.
constexpr int64_t kScanBase = int64_t{1} << 22;
constexpr int64_t kScanPerOp = 8;
std::atomic<int64_t> g_scanned{0};
std::atomic<int64_t> g_collectionOps{0};

// True when every list and map reachable from vals holds only vm_safe values.
bool contents_safe(const Value* vals, std::size_t n) {
    std::vector<const Value*> pending;
    for (std::size_t i = 0; i < n; ++i) {
        if (collection_kind(vals[i])) pending.push_back(&vals[i]);
    }
    if (pending.empty()) return true;
    int64_t budget = kScanBase + kScanPerOp * g_collectionOps.load(std::memory_order_relaxed) -
                     g_scanned.load(std::memory_order_relaxed);
    int64_t visited = 0;
    std::unordered_set<uint64_t> seen;
    bool ok = true;
    auto visit = [&](const Value& v) {
        if (++visited > budget || !vm_safe(v)) {
            ok = false;
            return;
        }
        if (collection_kind(v)) pending.push_back(&v);
    };
    while (ok && !pending.empty()) {
        const Value& c = *pending.back();
        pending.pop_back();
        const uint64_t key = (static_cast<uint64_t>(c.h.kind) << 32) | c.h.id;
        if (!seen.insert(key).second) continue;
        if (c.h.kind == HandleKind::List) {
            auto it = g_lists.find(static_cast<int>(c.h.id));
            if (it == g_lists.end()) continue;
            for (const Value& e : it->second) {
                visit(e);
                if (!ok) break;
            }
        } else {
            auto it = g_dicts.find(static_cast<int>(c.h.id));
            if (it == g_dicts.end()) continue;
            for (const auto& kv : it->second) {
                visit(kv.second);
                if (!ok) break;
            }
        }
    }
    g_scanned.fetch_add(visited, std::memory_order_relaxed);
    return ok;
}

// The tree-walker runs a struct's add/sub/mul/div/eq method for `a op b` when a is not a plain
// number, even for a list (any field-less struct matches). That would be tree-walker code
// running inside an aware frame.
bool has_operator_methods(const Program& program) {
    for (const auto& sd : program.structs) {
        for (const char* name : {"add", "sub", "mul", "div", "eq"}) {
            if (find_struct_method(sd, name)) return true;
        }
    }
    return false;
}

// The tree-walker stores struct fields as "<name>.<field>" variables and copies them along
// with a value passed by name.
bool has_field_vars(const Runtime::Env& env, const std::string& name) {
    const std::string prefix = name + ".";
    for (const auto& kv : env.vars.data) {
        if (kv.first.compare(0, prefix.size(), prefix) == 0) return true;
    }
    return false;
}

bool is_memory_builtin(const std::string& name) {
    static const std::unordered_set<std::string> names{
        "alloc", "heap", "shared", "weak", "buffer", "free", "realloc", "copy", "move", "fill",
        "zero", "mem_stats", "mem.stats", "mem_alive", "mem.alive", "mem_reset_stats",
        "mem.reset_stats"};
    return names.count(name) > 0;
}

bool is_plain_type(std::string type) {
    for (char& ch : type) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    static const std::unordered_set<std::string> plain{
        "int", "i32", "i64", "long", "float", "double", "f64", "bool", "boolean", "string", "str",
        "void"};
    return plain.count(type) > 0;
}

// `string.*` and `math.*` builtins with a plain result: pure functions of their arguments.
bool is_pure_builtin(const std::string& name) {
    static const std::unordered_set<std::string> names = [] {
        std::unordered_set<std::string> out;
        auto add = [&](const std::vector<registry::Member>& members) {
            for (const auto& member : members) {
                const std::string builtin = member.builtin;
                if ((builtin.rfind("string.", 0) == 0 || builtin.rfind("math.", 0) == 0) &&
                    is_plain_type(member.returns) && std::string(member.returns) != "void") {
                    out.insert(builtin);
                }
            }
        };
        add(registry::core());
        for (const auto& module : registry::modules()) add(module.members);
        return out;
    }();
    return names.count(name) > 0;
}

// Core conversions with a plain result. 1 and 2 (int, float) also have a native path.
int core_conversion(const std::string& name) {
    if (name == "int") return 1;
    if (name == "float") return 2;
    if (name == "string" || name == "bool") return 3;
    return 0;
}

// int(v) / float(v) of an int or float without the builtin dispatcher. The dispatcher formats
// v, parses the text and re-reads its result as a literal; formatting is shortest round-trip,
// so the parse gives back v exactly. float() keeps the re-read: "5e+05" stays a string there.
// False (dispatcher path) for other values and for int() of a non-finite or out-of-range float.
bool native_conversion(int which, const Value& v, Value& out) {
    if (v.kind != ValueKind::Int && v.kind != ValueKind::Float) return false;
    if (which == 2) {
        const double d = as_double(v);
        if (!std::isfinite(d)) {
            out = value_from_legacy_string(format_float(d));
            return true;
        }
        // format_float is to_chars plus ".0" when that has no point; the re-read gives d back
        // for plain digits and keeps exponent forms ("5e+05") as strings.
        char buf[64];
        const auto res = std::to_chars(buf, buf + sizeof buf, d);
        if (res.ec != std::errc()) return false;
        if (std::find(buf, res.ptr, 'e') == res.ptr) set_float(out, d);
        else out = Value::from_string(std::string(buf, res.ptr));
        return true;
    }
    if (v.kind == ValueKind::Int) {
        out = v;
        return true;
    }
    if (!(v.f < 9.2233720368547758e18 && v.f >= -9.2233720368547758e18)) return false;  // also NaN
    set_int(out, static_cast<int64_t>(v.f));
    return true;
}

// Marks a loop-local slot that the loop never assigned, so it is not written back.
constexpr int64_t kUnsetMark = 0x5e7f5e7f5e7f5e7f;
void mark_unset(Value& v) {
    v = Value();
    v.i = kUnsetMark;
}
bool is_unset(const Value& v) { return v.kind == ValueKind::Null && v.i == kUnsetMark; }

// Frames come from a thread-local stack of fixed blocks, so a call costs no heap allocation
// and a frame never moves while a callee runs. Slots outside live frames are always null.
class Arena {
public:
    struct Mark {
        std::size_t block;
        std::size_t top;
    };

    Value* push(std::size_t n, Mark& mark) {
        mark = {block_, top_};
        if (block_ < blocks_.size() && top_ + n <= sizes_[block_]) {
            Value* frame = blocks_[block_].get() + top_;
            top_ += n;
            return frame;
        }
        const std::size_t next = blocks_.empty() ? 0 : block_ + 1;
        if (next == blocks_.size()) {
            blocks_.emplace_back();
            sizes_.push_back(0);
        }
        if (sizes_[next] < n) {  // blocks past the current one hold no live frames
            const std::size_t size = std::max<std::size_t>(n, kBlockSize);
            blocks_[next] = std::make_unique<Value[]>(size);
            sizes_[next] = size;
        }
        block_ = next;
        top_ = n;
        return blocks_[next].get();
    }

    void pop(const Mark& mark, Value* frame, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            Value& v = frame[i];
            if (v.str) v.str.reset();
            v.kind = ValueKind::Null;
            v.i = 0;
        }
        block_ = mark.block;
        top_ = mark.top;
    }

    // Env for helpers called from compiled actions; compiled code never adds names to it.
    const Runtime::Env none{};

private:
    static constexpr std::size_t kBlockSize = 1 << 14;
    std::vector<std::unique_ptr<Value[]>> blocks_;
    std::vector<std::size_t> sizes_;
    std::size_t block_{0};
    std::size_t top_{0};
};

Arena& arena() {
    thread_local Arena a;
    return a;
}

struct Frame {
    Arena& arena;
    Arena::Mark mark{};
    Value* regs;
    std::size_t size;
    Frame(Arena& a, std::size_t n) : arena(a), regs(a.push(n, mark)), size(n) {}
    ~Frame() { arena.pop(mark, regs, size); }
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
};

struct AsyncFlagRestore {
    bool previous;
    ~AsyncFlagRestore() { async_set_in_async_action(previous); }
};

std::mutex g_vmMutex;

} // namespace

struct FastCode;

struct FastFunc {
    std::unique_ptr<FastCode> owned;
    std::atomic<const FastCode*> code{nullptr};  // null while compiling or when it did not compile
};

struct FastCode {
    struct CallSite {
        const Action* action;
        FastFunc* func;
    };
    struct BuiltinSite {
        std::string name;
        std::vector<ExprPtr> args;  // identifiers naming argNames in the bridge Env
        std::vector<std::string> argNames;
    };
    struct LetCheck {
        std::string name;
        std::string declared;
        uint8_t okKinds{0};  // bit per ValueKind whose runtime type always matches `declared`
        uint8_t okCollections{0};
    };
    struct MethodSite {
        std::string name;
        int32_t argc{0};
        bool statement{false};  // `xs.push(v);`: the result goes to `_` unless the method is void
        bool isVoid{false};
        uint8_t listOp{0};  // on a list: kListLen, kListPush or kListGet run inline
    };
    // A parameter or loop variable that must hold a list or map of these kinds on entry.
    struct EntryKind {
        int32_t slot;
        uint8_t kinds;
    };

    std::vector<Ins> ins;
    std::vector<Value> consts;  // copied to regs[constBase ..] when a frame starts
    int32_t constBase{0};
    int32_t nregs{0};
    std::vector<const BinaryExpr*> bins;
    std::vector<std::unique_ptr<BinaryExpr>> ownedBins;  // `x op= y` rewritten as `x op y`
    std::vector<LetCheck> lets;
    std::vector<CallSite> calls;
    std::vector<BuiltinSite> builtins;
    std::vector<MethodSite> methods;
    std::vector<EntryKind> entryKinds;
    int32_t iterators{0};
    // Loops: slot i holds the script variable names[i] ("" for a hidden counter).
    std::vector<std::string> names;
    std::vector<bool> assigned;
    // Loops: names that must not be local variables on entry (builtin module prefixes).
    std::vector<std::string> unbound;
    bool hasCalls{false};
    // Reads list or map contents: every list and map entering from the tree-walker is scanned.
    bool aware{false};
    // Never runs tree-walker code: every call goes to compiled code that is slow-free itself.
    bool slowFree{true};
};

namespace {

bool entry_kinds_match(const FastCode& code, const Value* r) {
    for (const auto& ek : code.entryKinds) {
        // A loop variable first assigned inside the loop is unset on entry.
        if (!is_unset(r[ek.slot]) && !(collection_kind(r[ek.slot]) & ek.kinds)) return false;
    }
    return true;
}

// Whether values may enter code that was compiled for them.
bool admit(const FastCode& code, const Value* vals, std::size_t n) {
    return !code.aware || contents_safe(vals, n);
}

// State of one for-each loop in a running frame.
struct IterState {
    uint8_t kind{0};  // 0 done, kKindList, kKindDict
    int id{0};
    std::size_t index{0};
    std::size_t count{0};
    // The tree-walker iterates a copy of the map, in the copy's order.
    std::optional<std::unordered_map<std::string, Value>> snapshot;
    std::unordered_map<std::string, Value>::const_iterator it;
};

// Collection operations a frame ran, added to g_collectionOps when it ends.
struct OpCount {
    int64_t n{0};
    ~OpCount() {
        if (n) g_collectionOps.fetch_add(n, std::memory_order_relaxed);
    }
};

} // namespace

class FastVm {
public:
    FastVm(const Runtime& rt, const Program* program) : rt_(rt), program_(program) {}

    const Program* program() const { return program_; }

    // Callers hold g_vmMutex.
    FastFunc* func_locked(const Action& action);
    template <class Node> const FastCode* loop_locked(const Node& node);

    static Value run_func(const Runtime& rt, const FastCode& code, const Value* args, int32_t argc, Arena& ar);
    template <class Node>
    static bool run_loop(const Runtime& rt, const Node& node, Runtime::ExecContext& ctx, Runtime::Env& env);
    static Value call_slow(const Runtime& rt, const Action& action, const Value* args, int32_t argc);

    class Compiler;

private:
    static bool exec(const Runtime& rt, const FastCode& code, Value* r, const Runtime::Env& env, Value& ret, Arena& ar);
    static Value binary(const Runtime& rt, const FastCode& code, uint8_t op, int32_t bin,
                        const Value& x, const Value& y, const Runtime::Env& env);
    // Out of line so the inline int paths in exec stay small. `cache` is exec's list cache,
    // dropped when other code may run.
    ERELANG_NOINLINE static void bin_generic(const Runtime& rt, const FastCode& code, Value* r, const Ins& op,
                                             const Runtime::Env& env, std::vector<Value>*& cache);
    ERELANG_NOINLINE static bool cmp_generic(const Runtime& rt, const FastCode& code, Value* r, const Ins& op,
                                             const Runtime::Env& env, std::vector<Value>*& cache);
    static Value builtin(const Runtime& rt, const FastCode::BuiltinSite& site, const Value* args, int32_t argc);
    static void load_consts(const FastCode& code, Value* r) {
        for (std::size_t k = 0; k < code.consts.size(); ++k) r[code.constBase + k] = code.consts[k];
    }

    const Runtime& rt_;
    const Program* program_;
    std::unordered_map<const Action*, std::unique_ptr<FastFunc>> funcs_;
    std::unordered_map<const void*, std::unique_ptr<FastCode>> loops_;  // null: did not compile
};

// ---------------------------------------------------------------------------------------------
// Compiler

class FastVm::Compiler {
public:
    Compiler(FastVm& vm, bool loopRegion) : vm_(vm), rt_(vm.rt_), prog_(*vm.program_), region_(loopRegion) {}

    std::unique_ptr<FastCode> action(const Action& a, const FastFunc* self) {
        try {
            if (a.isAsync || !a.typeParams.empty()) fail();
            self_ = self;
            collect_assigned(a.body);
            for (const auto& p : a.params) {
                if (p.name.empty() || slots_.count(p.name)) fail();
                new_slot(p.name);
                paramTypes_[p.name] = p.type;
            }
            // A global written from an action goes through env_set to globalVars_.
            for (const auto& name : assigned_) {
                if (rt_.globalNames_.count(name)) fail();
            }
            block(a.body);
            return finish();
        } catch (const Fail&) {
            return nullptr;
        }
    }

    template <class Node> std::unique_ptr<FastCode> loop(const Node& node) {
        try {
            collect_stmt(node);
            stmt_node(node);
            return finish();
        } catch (const Fail&) {
            return nullptr;
        }
    }

private:
    struct Fail {};
    [[noreturn]] static void fail() { throw Fail{}; }

    // Operands while compiling: slot s is s, temp t is kTemp + t, constant k is -(k + 1).
    // finish() maps them to the frame layout [slots][constants][temps].
    static constexpr int32_t kTemp = 1 << 24;

    struct LoopLabels {
        std::vector<std::size_t> breaks;
        std::vector<std::size_t> continues;
    };

    // -- names -----------------------------------------------------------------------------

    void collect_assigned(const Block& b) {
        for (const auto& s : b.stmts) std::visit([&](const auto& n) { collect_stmt(n); }, s);
    }
    void collect_target(const ExprPtr& e) {
        if (e) {
            if (const auto* id = std::get_if<ExprIdent>(&e->node)) assigned_.insert(id->name);
        }
    }
    template <class T> void collect_stmt(const T& n) {
        if constexpr (std::is_same_v<T, LetStmt>) {
            assigned_.insert(n.name);
        } else if constexpr (std::is_same_v<T, SetStmt>) {
            if (!n.isMember) assigned_.insert(n.varOrField);
        } else if constexpr (std::is_same_v<T, ExprStmt>) {
            if (!n.expr) return;
            if (const auto* p = std::get_if<PostfixExpr>(&n.expr->node)) collect_target(p->operand);
            if (const auto* p = std::get_if<PrefixExpr>(&n.expr->node)) collect_target(p->operand);
            if (const auto* c = std::get_if<CompoundAssignExpr>(&n.expr->node)) collect_target(c->left);
        } else if constexpr (std::is_same_v<T, IfStmt>) {
            if (n.thenBlk) collect_assigned(*n.thenBlk);
            if (n.elseBlk) collect_assigned(*n.elseBlk);
        } else if constexpr (std::is_same_v<T, WhileStmt> || std::is_same_v<T, DoWhileStmt> ||
                             std::is_same_v<T, RepeatStmt>) {
            if (n.body) collect_assigned(*n.body);
        } else if constexpr (std::is_same_v<T, ForStmt>) {
            if (n.init) collect_assigned(*n.init);
            if (n.step) collect_assigned(*n.step);
            if (n.body) collect_assigned(*n.body);
        } else if constexpr (std::is_same_v<T, ForInStmt>) {
            assigned_.insert(n.var);
            if (n.valueVar) assigned_.insert(*n.valueVar);
            if (n.body) collect_assigned(*n.body);
        } else if constexpr (std::is_same_v<T, MethodCallStmt>) {
            assigned_.insert("_");
        }
    }

    // How each variable is written, to prove that a method receiver holds a list or map.
    void note_untyped(const std::string& name) { untyped_.insert(name); }
    void note_typed(const std::string& name, const std::string& declared) {
        const uint8_t kind = declared_collection_kind(declared);
        typedKinds_[name] |= kind ? kind : kNotCollection;
    }

    int32_t new_slot(const std::string& name) {
        const auto s = static_cast<int32_t>(slotNames_.size());
        slotNames_.push_back(name);
        if (!name.empty()) slots_[name] = s;
        return s;
    }

    // In an action every variable is a parameter or assigned somewhere in it; anything else is
    // a global, an action reference or similar, which the tree-walker resolves. In a loop any
    // name may be a variable of the enclosing scope; run_loop binds it on entry.
    int32_t var(const std::string& name) {
        if (auto it = slots_.find(name); it != slots_.end()) return it->second;
        if (!region_ && !assigned_.count(name)) fail();
        return new_slot(name);
    }

    // -- emitting --------------------------------------------------------------------------

    std::size_t emit(Op op, int32_t a = 0, int32_t b = 0, int32_t c = 0, int32_t d = 0, uint8_t sub = 0) {
        Ins in;
        in.op = op;
        in.sub = sub;
        in.a = a;
        in.b = b;
        in.c = c;
        in.d = d;
        code_->ins.push_back(in);
        return code_->ins.size() - 1;
    }
    int32_t here() const { return static_cast<int32_t>(code_->ins.size()); }
    void patch(std::size_t at, int32_t target) {
        Ins& in = code_->ins[at];
        switch (in.op) {
            case Op::Jmp: in.a = target; break;
            case Op::JmpFalse: case Op::JmpTrue: in.b = target; break;
            case Op::CmpJmpFalse: case Op::CmpJmpTrue: case Op::Short: case Op::IterNext: in.c = target; break;
            default: break;
        }
    }

    int32_t konst(Value v) {
        code_->consts.push_back(std::move(v));
        return -static_cast<int32_t>(code_->consts.size());
    }
    int32_t temp() { return temps(1); }
    int32_t temps(int32_t n) {
        const int32_t base = kTemp + tempTop_;
        tempTop_ += n;
        tempMax_ = std::max(tempMax_, tempTop_);
        return base;
    }
    int32_t add_bin(const BinaryExpr& b) {
        code_->bins.push_back(&b);
        return static_cast<int32_t>(code_->bins.size() - 1);
    }

    // Receivers: every write is a typed let whose type guarantees an accepted kind; parameters
    // and loop variables from outside are checked on entry. Awareness: see FastCode::aware.
    void settle_collections() {
        FastCode& c = *code_;
        for (const auto& [name, kinds] : receivers_) {
            if (!kinds || untyped_.count(name)) fail();
            const uint8_t written = typedKinds_.count(name) ? typedKinds_.at(name) : 0;
            if (written & ~kinds) fail();  // includes kNotCollection
            const int32_t slot = slots_.at(name);
            if (auto p = paramTypes_.find(name); p != paramTypes_.end()) {
                const uint8_t declared = declared_collection_kind(p->second);
                if (declared && !(declared & kinds)) fail();
                c.entryKinds.push_back(FastCode::EntryKind{slot, kinds});
            } else if (region_) {
                c.entryKinds.push_back(FastCode::EntryKind{slot, kinds});
            }
        }
        bool callsAware = false;
        for (const auto& site : c.calls) {
            if (site.func == self_) continue;
            const FastCode* callee = site.func->code.load(std::memory_order_acquire);
            if (!callee || !callee->slowFree) c.slowFree = false;
            if (callee && callee->aware) callsAware = true;
        }
        const bool operators = has_operator_methods(prog_);
        if (needAware_ && (!c.slowFree || operators)) fail();
        c.aware = needAware_ || (callsAware && c.slowFree && !operators);
    }

    // Register written by an op, or -1. IterNext with two variables also writes d.
    static int32_t written_register(const Ins& in) {
        switch (in.op) {
            case Op::Jmp: case Op::JmpFalse: case Op::JmpTrue: case Op::CmpJmpFalse: case Op::CmpJmpTrue:
            case Op::Print: case Op::CheckType: case Op::DictPut: case Op::IndexSet: case Op::IterInit:
            case Op::Ret: case Op::End:
                return -1;
            default:
                return in.a;
        }
    }

    // Rewrites int-friendly Bin and compare-jumps into dedicated ops (see ERELANG_FAST_OPS).
    static void specialize(FastCode& c) {
        const auto constEnd = c.constBase + static_cast<int32_t>(c.consts.size());
        std::vector<bool> constWritten(c.consts.size(), false);
        for (const Ins& in : c.ins) {
            const int32_t targets[2] = {written_register(in), in.op == Op::IterNext && in.sub ? in.d : -1};
            for (int32_t t : targets) {
                if (t >= c.constBase && t < constEnd) constWritten[static_cast<std::size_t>(t - c.constBase)] = true;
            }
        }
        auto int_const = [&](int32_t reg) {
            if (reg < c.constBase || reg >= constEnd) return false;
            const auto k = static_cast<std::size_t>(reg - c.constBase);
            return !constWritten[k] && c.consts[k].kind == ValueKind::Int;
        };
        for (Ins& in : c.ins) {
            if (in.op == Op::Bin) {
                const bool k = int_const(in.c);
                switch (static_cast<ValueBinOp>(in.sub)) {
                    case ValueBinOp::Add: in.op = k ? Op::AddIntK : Op::AddInt; break;
                    case ValueBinOp::Sub: in.op = k ? Op::SubIntK : Op::SubInt; break;
                    case ValueBinOp::Mul: in.op = k ? Op::MulIntK : Op::MulInt; break;
                    default: break;
                }
            } else if (in.op == Op::CmpJmpFalse || in.op == Op::CmpJmpTrue) {
                const bool whenTrue = in.op == Op::CmpJmpTrue;
                const bool k = int_const(in.b);
                // For ints !(a < b) is a >= b, so a false-jump becomes the inverse comparison.
                Op lt = Op::JmpLt, le = Op::JmpLe, gt = Op::JmpGt, ge = Op::JmpGe, eq = Op::JmpEq, ne = Op::JmpNe;
                if (k) {
                    lt = Op::JmpLtK; le = Op::JmpLeK; gt = Op::JmpGtK; ge = Op::JmpGeK; eq = Op::JmpEqK; ne = Op::JmpNeK;
                }
                Op next = in.op;
                switch (static_cast<ValueBinOp>(in.sub)) {
                    case ValueBinOp::Lt: next = whenTrue ? lt : ge; break;
                    case ValueBinOp::Le: next = whenTrue ? le : gt; break;
                    case ValueBinOp::Gt: next = whenTrue ? gt : le; break;
                    case ValueBinOp::Ge: next = whenTrue ? ge : lt; break;
                    case ValueBinOp::Eq: case ValueBinOp::StrictEq: next = whenTrue ? eq : ne; break;
                    case ValueBinOp::Ne: case ValueBinOp::StrictNe: next = whenTrue ? ne : eq; break;
                    default: break;
                }
                if (next != in.op) {
                    in.op = next;
                    if (whenTrue) in.sub |= kJumpWhenTrue;
                }
            }
        }
        for (std::size_t i = 0; i + 1 < c.ins.size(); ++i) {
            Ins& step = c.ins[i];
            const Op jump = c.ins[i + 1].op;
            const bool lt = jump == Op::JmpLt || jump == Op::JmpLtK;
            const bool le = jump == Op::JmpLe || jump == Op::JmpLeK;
            if (!lt && !le) continue;
            if (step.op == Op::IncInt) step.op = lt ? Op::IncIntLt : Op::IncIntLe;
            else if (step.op == Op::AddIntK) step.op = lt ? Op::AddIntKLt : Op::AddIntKLe;
        }
    }

    std::unique_ptr<FastCode> finish() {
        emit(Op::End);
        settle_collections();
        FastCode& c = *code_;
        const auto slots = static_cast<int32_t>(slotNames_.size());
        c.constBase = slots;
        const int32_t tempBase = slots + static_cast<int32_t>(c.consts.size());
        c.nregs = tempBase + tempMax_;
        auto map = [&](int32_t x) {
            if (x < 0) return c.constBase + (-x - 1);
            if (x >= kTemp) return tempBase + (x - kTemp);
            return x;
        };
        for (Ins& in : c.ins) {
            switch (in.op) {
                case Op::Move: case Op::Unary: case Op::Short: case Op::ToCount:
                case Op::CmpJmpFalse: case Op::CmpJmpTrue: case Op::Concat:
                    in.a = map(in.a);
                    in.b = map(in.b);
                    break;
                case Op::Bin: case Op::DictPut: case Op::Range: case Op::Index: case Op::IndexSet:
                    in.a = map(in.a);
                    in.b = map(in.b);
                    in.c = map(in.c);
                    break;
                case Op::NewList:
                    in.a = map(in.a);
                    in.b = map(in.b);
                    break;
                case Op::Method:
                    in.a = map(in.a);
                    in.c = map(in.c);
                    in.d = map(in.d);
                    break;
                case Op::IterNext:
                    in.a = map(in.a);
                    if (in.sub) in.d = map(in.d);
                    break;
                case Op::JmpFalse: case Op::JmpTrue: case Op::Print: case Op::IncInt: case Op::CheckType:
                case Op::Ret: case Op::NewDict: case Op::IterInit:
                    in.a = map(in.a);
                    break;
                case Op::Call: case Op::Builtin:
                    in.a = map(in.a);
                    in.c = map(in.c);
                    break;
                case Op::Jmp: case Op::End:
                    break;
                case Op::AddInt: case Op::SubInt: case Op::MulInt:
                case Op::AddIntK: case Op::SubIntK: case Op::MulIntK:
                case Op::JmpLt: case Op::JmpLe: case Op::JmpGt: case Op::JmpGe: case Op::JmpEq: case Op::JmpNe:
                case Op::JmpLtK: case Op::JmpLeK: case Op::JmpGtK: case Op::JmpGeK: case Op::JmpEqK: case Op::JmpNeK:
                case Op::IncIntLt: case Op::IncIntLe: case Op::AddIntKLt: case Op::AddIntKLe:
                    break;  // made by specialize() after mapping
            }
        }
        specialize(c);
        c.names = slotNames_;
        c.assigned.reserve(slotNames_.size());
        for (const auto& name : slotNames_) c.assigned.push_back(name.empty() || assigned_.count(name) > 0);
        return std::move(code_);
    }

    // -- expressions -----------------------------------------------------------------------

    static std::optional<Value> literal(const Expr& e) {
        if (std::holds_alternative<ExprNull>(e.node)) return Value::null_value();
        if (const auto* b = std::get_if<ExprBool>(&e.node)) return Value::from_bool(b->v);
        if (const auto* n = std::get_if<ExprNumber>(&e.node)) {
            if (!n->isFloatLiteral) return Value::from_int(n->v);
            try {
                return Value::from_float(std::stod(n->raw));
            } catch (...) {
                return Value::from_float(static_cast<double>(n->v));
            }
        }
        if (const auto* s = std::get_if<ExprString>(&e.node)) {
            if (s->raw || s->v.find('{') == std::string::npos) return Value::from_string(s->v);
        }
        return std::nullopt;
    }

    int32_t value(const Expr& e) {
        if (const auto* id = std::get_if<ExprIdent>(&e.node)) return var(id->name);
        if (auto lit = literal(e)) return konst(std::move(*lit));
        const int32_t t = temp();
        into(e, t);
        return t;
    }

    // Writes dst only as the last step (or when a short-circuit decides), so `x = f(x)` reads
    // the old x throughout.
    void into(const Expr& e, int32_t dst) {
        const int32_t saved = tempTop_;
        if (std::holds_alternative<ExprIdent>(e.node) || literal(e)) {
            emit(Op::Move, dst, value(e));
        } else if (const auto* s = std::get_if<ExprString>(&e.node)) {
            interpolate(s->v, dst);
        } else if (const auto* b = std::get_if<BinaryExpr>(&e.node)) {
            binary(*b, dst);
        } else if (const auto* u = std::get_if<UnaryExpr>(&e.node)) {
            if (!u->expr) fail();
            ValueUnOp op;
            switch (u->op) {
                case UnOp::Neg: op = ValueUnOp::Neg; break;
                case UnOp::Not: op = ValueUnOp::Not; break;
                case UnOp::BitNot: op = ValueUnOp::BitNot; break;
                default: fail();
            }
            emit(Op::Unary, dst, value(*u->expr), 0, 0, static_cast<uint8_t>(op));
        } else if (const auto* t = std::get_if<TernaryExpr>(&e.node)) {
            if (!t->cond || !t->thenExpr || !t->elseExpr) fail();
            const std::size_t toElse = cond_jump(*t->cond);
            into(*t->thenExpr, dst);
            const std::size_t toEnd = emit(Op::Jmp);
            patch(toElse, here());
            into(*t->elseExpr, dst);
            patch(toEnd, here());
        } else if (const auto* fc = std::get_if<FunctionCallExpr>(&e.node)) {
            if (!fc->typeArgs.empty()) fail();
            const auto dot = fc->name.rfind('.');
            if (dot == std::string::npos) {
                action_call(fc->name, fc->args, dst, true);
            } else {
                const std::string receiver = fc->name.substr(0, dot);
                const bool local = slots_.count(receiver) || assigned_.count(receiver);
                if (!local && is_pure_builtin(fc->name)) {
                    builtin_call(*fc, dst);
                } else if (local || (region_ && is_identifier_text(receiver))) {
                    method_call(receiver, fc->name.substr(dot + 1), fc->args, dst, false);
                } else {
                    fail();
                }
            }
        } else if (const auto* ix = std::get_if<IndexExpr>(&e.node)) {
            if (!ix->object || !ix->index) fail();
            const int32_t object = value(*ix->object);
            const int32_t index = value(*ix->index);
            emit(Op::Index, dst, object, index);
            needAware_ = true;
        } else if (const auto* list = std::get_if<ListLiteralExpr>(&e.node)) {
            const auto n = static_cast<int32_t>(list->elements.size());
            const int32_t base = temps(n);
            for (int32_t i = 0; i < n; ++i) {
                const ExprPtr& el = list->elements[static_cast<std::size_t>(i)];
                if (!el) fail();
                into(*el, base + i);
            }
            emit(Op::NewList, dst, base, n);
            needAware_ = true;
        } else if (const auto* dict = std::get_if<DictLiteralExpr>(&e.node)) {
            // The map is created before its entries are evaluated, as in the tree-walker.
            const int32_t handle = temp();
            emit(Op::NewDict, handle, static_cast<int32_t>(dict->entries.size() / 2));
            for (std::size_t i = 0; i + 1 < dict->entries.size(); i += 2) {
                const ExprPtr& key = dict->entries[i];
                const ExprPtr& val = dict->entries[i + 1];
                if (!key || !val || is_raw_string(*val)) fail();
                const int32_t pairSaved = tempTop_;
                const int32_t k = value(*key);
                const int32_t v = value(*val);
                emit(Op::DictPut, handle, k, v);
                tempTop_ = pairSaved;
            }
            emit(Op::Move, dst, handle);
            needAware_ = true;
        } else if (const auto* range = std::get_if<RangeExpr>(&e.node)) {
            if (!range->start || !range->end) fail();
            const int32_t start = value(*range->start);
            const int32_t end = value(*range->end);
            emit(Op::Range, dst, start, end, 0, range->exclusive ? 1 : 0);
            needAware_ = true;
        } else {
            fail();
        }
        tempTop_ = saved;
    }

    // eval_arg re-reads a runtime-built (raw) string literal as a value; into() keeps it a string.
    static bool is_raw_string(const Expr& e) {
        const auto* s = std::get_if<ExprString>(&e.node);
        return s && s->raw;
    }

    // `recv.method(args)` on a list or map, as Runtime::collection_method runs it.
    void method_call(const std::string& receiver, const std::string& method, const std::vector<ExprPtr>& args,
                     int32_t dst, bool statement) {
        if (method == "forEach" || is_memory_builtin(receiver + "." + method)) fail();
        for (const auto& en : prog_.enums) {
            if (en.name == receiver) fail();  // `Enum.Variant(...)` takes precedence
        }
        uint8_t kinds = 0;
        if (collection_method_known(HandleKind::List, method)) kinds |= kKindList;
        if (collection_method_known(HandleKind::Dict, method)) kinds |= kKindDict;
        if (!kinds) fail();
        const int32_t recv = var(receiver);
        auto [it, inserted] = receivers_.try_emplace(receiver, kinds);
        if (!inserted) it->second &= kinds;
        const auto n = static_cast<int32_t>(args.size());
        const int32_t base = temps(n);
        for (int32_t i = 0; i < n; ++i) {
            const ExprPtr& arg = args[static_cast<std::size_t>(i)];
            if (!arg || (statement && is_raw_string(*arg))) fail();
            into(*arg, base + i);
        }
        FastCode::MethodSite site;
        site.name = method;
        site.argc = n;
        site.statement = statement;
        site.isVoid = collection_method_is_void(method);
        const std::string_view listName = list_method_name(method);
        if (listName == "len") site.listOp = kListLen;
        else if (listName == "push" && n == 1) site.listOp = kListPush;
        else if (listName == "get" && n == 1) site.listOp = kListGet;
        code_->methods.push_back(std::move(site));
        emit(Op::Method, dst, static_cast<int32_t>(code_->methods.size() - 1), recv, base);
        needAware_ = true;
    }

    void binary(const BinaryExpr& b, int32_t dst) {
        if (!b.left || !b.right) fail();
        const auto op = static_cast<uint8_t>(to_vbin(b.op));
        const int32_t bin = add_bin(b);
        if (b.op == BinOp::And || b.op == BinOp::Or || b.op == BinOp::Coalesce) {
            const int32_t left = value(*b.left);
            const uint8_t kind = b.op == BinOp::And ? 0 : (b.op == BinOp::Or ? 1 : 2);
            const std::size_t decided = emit(Op::Short, dst, left, 0, 0, kind);
            const int32_t right = value(*b.right);
            emit(Op::Bin, dst, left, right, bin, op);
            patch(decided, here());
            return;
        }
        const int32_t left = value(*b.left);
        const int32_t right = value(*b.right);
        emit(Op::Bin, dst, left, right, bin, op);
    }

    // Jump taken when cond is false (or true with whenTrue); the caller patches its target.
    std::size_t cond_jump(const Expr& cond, bool whenTrue = false) {
        const int32_t saved = tempTop_;
        std::size_t at;
        const auto* b = std::get_if<BinaryExpr>(&cond.node);
        if (b && b->left && b->right && is_comparison(b->op)) {
            const int32_t bin = add_bin(*b);
            const int32_t left = value(*b->left);
            const int32_t right = value(*b->right);
            at = emit(whenTrue ? Op::CmpJmpTrue : Op::CmpJmpFalse, left, right, 0, bin,
                      static_cast<uint8_t>(to_vbin(b->op)));
        } else {
            at = emit(whenTrue ? Op::JmpTrue : Op::JmpFalse, value(cond));
        }
        tempTop_ = saved;
        return at;
    }

    // An interpolation key the tree-walker evaluates inside try/catch (printing the key as
    // written when it throws) compiles only if it cannot throw.
    static bool cannot_throw(const Expr& e) {
        if (std::holds_alternative<ExprIdent>(e.node)) return true;
        if (literal(e)) return true;
        if (const auto* b = std::get_if<BinaryExpr>(&e.node)) {
            switch (b->op) {
                case BinOp::Div: case BinOp::Mod: case BinOp::Pow: case BinOp::Shl: case BinOp::Shr:
                    return false;
                default:
                    return b->left && b->right && cannot_throw(*b->left) && cannot_throw(*b->right);
            }
        }
        if (const auto* u = std::get_if<UnaryExpr>(&e.node)) {
            return u->expr && (u->op == UnOp::Neg || u->op == UnOp::Not || u->op == UnOp::BitNot) &&
                   cannot_throw(*u->expr);
        }
        if (const auto* t = std::get_if<TernaryExpr>(&e.node)) {
            return t->cond && t->thenExpr && t->elseExpr && cannot_throw(*t->cond) &&
                   cannot_throw(*t->thenExpr) && cannot_throw(*t->elseExpr);
        }
        return false;
    }

    // Mirrors Runtime::eval_string for a string literal containing `{`.
    void interpolate(const std::string& input, int32_t dst) {
        struct Part {
            std::string text;
            const Expr* expr = nullptr;
            int32_t slot = -1;
        };
        std::vector<Part> parts;
        std::string pending;
        auto flush = [&] {
            if (!pending.empty()) parts.push_back(Part{std::move(pending)});
            pending.clear();
        };
        for (std::size_t i = 0; i < input.size();) {
            if (input[i] == '{') {
                const std::size_t close = input.find('}', i + 1);
                if (close != std::string::npos) {
                    const std::string raw = input.substr(i + 1, close - i - 1);
                    const std::string key = trim_copy(raw);
                    const bool quantifier = !raw.empty() && std::isdigit(static_cast<unsigned char>(raw[0])) &&
                                            raw.find_first_not_of("0123456789,") == std::string::npos;
                    if (quantifier) {
                        pending += '{';
                        pending += raw;
                        pending += '}';
                    } else if (is_identifier_text(key)) {
                        flush();
                        Part p;
                        p.slot = var(key);
                        parts.push_back(std::move(p));
                    } else {
                        auto parsed = rt_.parse_interpolation_expr(key);
                        if (!parsed || !*parsed || !cannot_throw(**parsed)) fail();
                        flush();
                        Part p;
                        p.expr = parsed->get();
                        keepAlive_.push_back(*parsed);
                        parts.push_back(std::move(p));
                    }
                    i = close + 1;
                    continue;
                }
            }
            pending.push_back(input[i++]);
        }
        flush();
        if (parts.empty() || (parts.size() == 1 && parts[0].slot < 0 && !parts[0].expr)) {
            emit(Op::Move, dst, konst(Value::from_string(parts.empty() ? std::string() : parts[0].text)));
            return;
        }
        const auto n = static_cast<int32_t>(parts.size());
        const int32_t base = temps(n);
        for (int32_t k = 0; k < n; ++k) {
            const Part& p = parts[static_cast<std::size_t>(k)];
            if (p.expr) {
                into(*p.expr, base + k);
            } else if (p.slot >= 0) {
                emit(Op::Move, base + k, p.slot);
            } else {
                emit(Op::Move, base + k, konst(Value::from_string(p.text)));
            }
        }
        emit(Op::Concat, dst, base, n);
    }

    // Calls to script actions. Same lookup order as the tree-walker for an undotted name:
    // memory builtins, struct constructors, then actions.
    void action_call(const std::string& name, const std::vector<ExprPtr>& args, int32_t dst, bool expression) {
        if (is_memory_builtin(name) || find_struct_decl(prog_, name)) fail();
        const Action* callee = rt_.find_action(prog_, name);
        if (!callee && expression) {
            conversion_call(name, args, dst);
            return;
        }
        if (!callee || callee->isAsync || !callee->typeParams.empty()) fail();
        if (prog_.strict && callee->visibility != Visibility::Public) fail();
        if (args.size() != callee->params.size()) fail();
        FastFunc* func = vm_.func_locked(*callee);
        // A callee that runs on the tree-walker must return a plain value.
        if (!func->code.load(std::memory_order_acquire) && !is_plain_type(callee->returnType)) fail();
        const auto n = static_cast<int32_t>(args.size());
        const int32_t base = temps(n);
        for (int32_t i = 0; i < n; ++i) {
            if (!args[static_cast<std::size_t>(i)]) fail();
            into(*args[static_cast<std::size_t>(i)], base + i);
        }
        code_->calls.push_back(FastCode::CallSite{callee, func});
        code_->hasCalls = true;
        emit(Op::Call, dst, static_cast<int32_t>(code_->calls.size() - 1), base, n);
    }

    // The tree-walker reaches eval_builtin_call for `string.len(...)` only after ruling out an
    // enum, a variable, an object, a struct or an action under that name; the same checks here
    // (and on loop entry for the enclosing scope) let the compiled call go there directly.
    void builtin_call(const FunctionCallExpr& fc, int32_t dst) {
        if (!is_pure_builtin(fc.name)) fail();
        const std::string receiver = fc.name.substr(0, fc.name.rfind('.'));
        if (slots_.count(receiver) || assigned_.count(receiver)) fail();
        if (rt_.globalVars_.count(receiver) || rt_.globalVars_.count(fc.name)) fail();
        if (find_struct_decl(prog_, fc.name) || rt_.find_action(prog_, fc.name)) fail();
        for (const auto& en : prog_.enums) {
            if (en.name == receiver) fail();
        }
        if (region_) {
            code_->unbound.push_back(receiver);
            code_->unbound.push_back(fc.name);
        }
        emit_builtin(fc.name, fc.args, dst, 0);
    }

    // int(x), float(x), string(x), bool(x): reached after the tree-walker found no action,
    // struct or function variable under that name.
    void conversion_call(const std::string& name, const std::vector<ExprPtr>& args, int32_t dst) {
        const int which = core_conversion(name);
        if (!which || args.size() != 1) fail();
        if (slots_.count(name) || assigned_.count(name) || rt_.globalVars_.count(name)) fail();
        if (region_) code_->unbound.push_back(name);
        emit_builtin(name, args, dst, static_cast<uint8_t>(which < 3 ? which : 0));
    }

    void emit_builtin(const std::string& name, const std::vector<ExprPtr>& args, int32_t dst, uint8_t native) {
        FastCode::BuiltinSite site;
        site.name = name;
        const auto n = static_cast<int32_t>(args.size());
        const int32_t base = temps(n);
        for (int32_t i = 0; i < n; ++i) {
            if (!args[static_cast<std::size_t>(i)]) fail();
            std::string argName = "%fv" + std::to_string(i);
            site.args.push_back(std::make_shared<Expr>(Expr{ExprIdent{argName}}));
            site.argNames.push_back(std::move(argName));
            into(*args[static_cast<std::size_t>(i)], base + i);
        }
        code_->builtins.push_back(std::move(site));
        emit(Op::Builtin, dst, static_cast<int32_t>(code_->builtins.size() - 1), base, n, native);
    }

    // -- statements ------------------------------------------------------------------------

    void block(const Block& b) {
        for (const auto& s : b.stmts) {
            tempTop_ = 0;
            std::visit([&](const auto& n) { stmt_node(n); }, s);
        }
    }

    template <class T> void stmt_node(const T&) { fail(); }

    void stmt_node(const PrintStmt& n) {
        if (!n.value) fail();
        emit(Op::Print, value(*n.value));
    }

    void stmt_node(const LetStmt& n) {
        if (n.pattern || n.sizeExpr || !n.value || n.name.empty()) fail();
        if (!n.declaredType.empty() &&
            (find_struct_decl(prog_, n.declaredType) || rt_.find_entity(prog_, n.declaredType))) {
            fail();
        }
        const std::string norm = normalize_runtime_type_name(n.declaredType);
        const bool check = !n.declaredType.empty() && norm != "string" && norm != "str";
        if (check && ((!norm.empty() && norm.front() == '*') || norm.rfind("heap<", 0) == 0 ||
                      norm.rfind("own<", 0) == 0 || norm.rfind("shared<", 0) == 0 ||
                      norm.rfind("weak<", 0) == 0 || norm.rfind("buffer<", 0) == 0 || norm == "pointer")) {
            fail();
        }
        const int32_t slot = var(n.name);
        if (n.declaredType.empty()) note_untyped(n.name);
        else note_typed(n.name, n.declaredType);
        if (!check) {
            into(*n.value, slot);
            return;
        }
        const int32_t t = temp();
        into(*n.value, t);
        FastCode::LetCheck let{n.name, n.declaredType};
        auto matches = [&](const char* actual) { return runtime_declared_type_matches(n.declaredType, actual); };
        if (matches("int")) let.okKinds |= 1u << static_cast<unsigned>(ValueKind::Int);
        if (matches("double")) let.okKinds |= 1u << static_cast<unsigned>(ValueKind::Float);
        if (matches("bool")) let.okKinds |= 1u << static_cast<unsigned>(ValueKind::Bool);
        if (matches("array<any>")) let.okCollections |= kKindList;
        if (matches("map<string,any>")) let.okCollections |= kKindDict;
        code_->lets.push_back(std::move(let));
        emit(Op::CheckType, t, static_cast<int32_t>(code_->lets.size() - 1));
        emit(Op::Move, slot, t);
    }

    void stmt_node(const SetStmt& n) {
        if (n.isMember || !n.value) fail();
        note_untyped(n.varOrField);
        into(*n.value, var(n.varOrField));
    }

    void stmt_node(const ExprStmt& n) {
        if (!n.expr) return;
        auto target = [&](const ExprPtr& e) -> int32_t {
            if (!e) fail();
            const auto* id = std::get_if<ExprIdent>(&e->node);
            if (!id) fail();
            note_untyped(id->name);
            return var(id->name);
        };
        if (const auto* p = std::get_if<PostfixExpr>(&n.expr->node)) {
            emit(Op::IncInt, target(p->operand), p->isInc ? 1 : -1);
        } else if (const auto* p = std::get_if<PrefixExpr>(&n.expr->node)) {
            emit(Op::IncInt, target(p->operand), p->isInc ? 1 : -1);
        } else if (const auto* c = std::get_if<CompoundAssignExpr>(&n.expr->node)) {
            const int32_t slot = target(c->left);
            if (!c->right) fail();
            code_->ownedBins.push_back(std::make_unique<BinaryExpr>(BinaryExpr{c->op, c->left, c->right}));
            binary(*code_->ownedBins.back(), slot);
        } else {
            into(*n.expr, temp());
        }
    }

    void stmt_node(const ActionCallStmt& n) {
        if (!n.typeArgs.empty()) fail();
        action_call(n.name, n.args, temp(), false);
    }

    void stmt_node(const MethodCallStmt& n) {
        if (rt_.globalNames_.count("_")) fail();
        note_untyped("_");
        method_call(n.objectName, n.method, n.args, var("_"), true);
    }

    void stmt_node(const IndexSetStmt& n) {
        if (!n.object || !n.index || !n.value) fail();
        const int32_t object = value(*n.object);
        const int32_t index = value(*n.index);
        const int32_t v = value(*n.value);
        emit(Op::IndexSet, v, object, index);
        needAware_ = true;
    }

    void stmt_node(const ForInStmt& n) {
        if (!n.iterable || !n.body || n.var.empty()) fail();
        const int32_t iterable = value(*n.iterable);
        const int32_t k = code_->iterators++;
        const uint8_t pair = n.valueVar ? 1 : 0;
        emit(Op::IterInit, iterable, k, 0, 0, pair);
        note_untyped(n.var);
        const int32_t item = var(n.var);
        int32_t second = 0;
        if (n.valueVar) {
            note_untyped(*n.valueVar);
            second = var(*n.valueVar);
        }
        const int32_t top = here();
        const std::size_t next = emit(Op::IterNext, item, k, 0, second, pair);
        loops_.emplace_back();
        block(*n.body);
        const int32_t cont = here();
        emit(Op::Jmp, top);
        patch(next, here());
        close_loop(cont, here());
        needAware_ = true;
    }

    void stmt_node(const ReturnStmt& n) {
        const int32_t v = (n.value && *n.value) ? value(**n.value) : konst(Value::from_string(""));
        emit(Op::Ret, v);
    }

    void stmt_node(const IfStmt& n) {
        if (!n.cond || !n.thenBlk) fail();
        const std::size_t toElse = cond_jump(*n.cond);
        block(*n.thenBlk);
        if (n.elseBlk) {
            const std::size_t toEnd = emit(Op::Jmp);
            patch(toElse, here());
            block(*n.elseBlk);
            patch(toEnd, here());
        } else {
            patch(toElse, here());
        }
    }

    void stmt_node(const BreakStmt&) {
        if (loops_.empty()) fail();
        loops_.back().breaks.push_back(emit(Op::Jmp));
    }

    void stmt_node(const ContinueStmt&) {
        if (loops_.empty()) fail();
        loops_.back().continues.push_back(emit(Op::Jmp));
    }

    void close_loop(int32_t continueTarget, int32_t endTarget) {
        for (std::size_t at : loops_.back().breaks) patch(at, endTarget);
        for (std::size_t at : loops_.back().continues) patch(at, continueTarget);
        loops_.pop_back();
    }

    // Loops test their condition at the bottom: one jump per iteration instead of two.
    void stmt_node(const WhileStmt& n) {
        if (!n.cond || !n.body) fail();
        const std::size_t toCheck = emit(Op::Jmp);
        const int32_t top = here();
        loops_.emplace_back();
        block(*n.body);
        const int32_t check = here();
        tempTop_ = 0;
        patch(toCheck, check);
        patch(cond_jump(*n.cond, true), top);
        close_loop(check, here());
    }

    void stmt_node(const DoWhileStmt& n) {
        if (!n.cond || !n.body) fail();
        const int32_t top = here();
        loops_.emplace_back();
        block(*n.body);
        const int32_t check = here();
        tempTop_ = 0;
        patch(cond_jump(*n.cond, true), top);
        close_loop(check, here());
    }

    void stmt_node(const ForStmt& n) {
        if (!n.body) fail();
        if (n.cond && !*n.cond) fail();
        if (n.init) block(*n.init);
        const std::size_t toCheck = emit(Op::Jmp);
        const int32_t top = here();
        loops_.emplace_back();
        block(*n.body);
        const int32_t step = here();
        if (n.step) block(*n.step);
        patch(toCheck, here());
        tempTop_ = 0;
        if (n.cond) {
            patch(cond_jump(**n.cond, true), top);
        } else {
            emit(Op::Jmp, top);
        }
        close_loop(step, here());
    }

    void stmt_node(const RepeatStmt& n) {
        if (!n.count || !n.body) fail();
        const int32_t count = new_slot("");
        const int32_t index = new_slot("");
        emit(Op::ToCount, count, value(*n.count));
        emit(Op::Move, index, konst(Value::from_int(0)));
        const int32_t top = here();
        // Both operands are always ints, so the tree-walker path (bins index) is never needed.
        const std::size_t exit = emit(Op::CmpJmpFalse, index, count, 0, -1,
                                      static_cast<uint8_t>(ValueBinOp::Lt));
        loops_.emplace_back();
        block(*n.body);
        const int32_t next = here();
        emit(Op::IncInt, index, 1);
        emit(Op::Jmp, top);
        patch(exit, here());
        close_loop(next, here());
    }

    static constexpr uint8_t kNotCollection = 0x80;

    FastVm& vm_;
    const Runtime& rt_;
    const Program& prog_;
    bool region_;
    const FastFunc* self_{nullptr};
    std::unordered_map<std::string, std::string> paramTypes_;
    std::unordered_map<std::string, uint8_t> receivers_;   // name -> kinds all its methods accept
    std::unordered_map<std::string, uint8_t> typedKinds_;  // name -> kinds its typed lets declare
    std::unordered_set<std::string> untyped_;              // names written without a declared type
    bool needAware_{false};
    std::unique_ptr<FastCode> code_ = std::make_unique<FastCode>();
    std::unordered_map<std::string, int32_t> slots_;
    std::unordered_set<std::string> assigned_;
    std::vector<std::string> slotNames_;
    std::vector<ExprPtr> keepAlive_;  // parsed interpolation expressions (cached by the runtime too)
    int32_t tempTop_{0};
    int32_t tempMax_{0};
    std::vector<LoopLabels> loops_;
};

// ---------------------------------------------------------------------------------------------
// Caches

FastFunc* FastVm::func_locked(const Action& action) {
    auto [it, inserted] = funcs_.try_emplace(&action);
    if (!inserted) return it->second.get();
    it->second = std::make_unique<FastFunc>();
    FastFunc* func = it->second.get();  // stable: recursive compiles may rehash funcs_
    Compiler compiler(*this, false);
    func->owned = compiler.action(action, func);    func->code.store(func->owned.get(), std::memory_order_release);    return func;
}

template <class Node> const FastCode* FastVm::loop_locked(const Node& node) {
    auto [it, inserted] = loops_.try_emplace(&node);
    if (inserted) {
        Compiler compiler(*this, true);
        auto code = compiler.loop(node);        loops_[&node] = std::move(code);  // compiling may have rehashed loops_
        return loops_[&node].get();
    }
    return it->second.get();
}

// ---------------------------------------------------------------------------------------------
// Execution

Value FastVm::binary(const Runtime& rt, const FastCode& code, uint8_t op, int32_t bin,
                     const Value& x, const Value& y, const Runtime::Env& env) {
    if ((is_scalar(x) && is_scalar(y)) || bin < 0) return apply_binary(static_cast<ValueBinOp>(op), x, y);
    // Same call the tree-walker makes for strings: units, pointer arithmetic, struct operators.
    std::unordered_map<std::string, Value> leftFields;
    if (!is_scalar(x)) leftFields = rt.lastReturnFields_;
    return rt.eval_binary_slow(*code.bins[static_cast<std::size_t>(bin)], x, y, std::move(leftFields),
                               rt.lastReturnFields_, env);
}

// Bin: the inline mixed-number path, then apply_binary / the tree-walker.
void FastVm::bin_generic(const Runtime& rt, const FastCode& code, Value* r, const Ins& op,
                         const Runtime::Env& env, std::vector<Value>*& cache) {
    const Value& x = r[op.b];
    const Value& y = r[op.c];
    if (mixed_operands(x, y) && float_binary(op.sub, x, y, r[op.a])) return;
    cache = nullptr;
    r[op.a] = binary(rt, code, op.sub, op.d, x, y, env);
}

// A specialized compare-jump on operands that are not both ints: whether to jump.
bool FastVm::cmp_generic(const Runtime& rt, const FastCode& code, Value* r, const Ins& op,
                         const Runtime::Env& env, std::vector<Value>*& cache) {
    const Value& x = r[op.a];
    const Value& y = r[op.b];
    const auto cmp = static_cast<uint8_t>(op.sub & ~kJumpWhenTrue);
    bool holds = false;
    if (x.kind == ValueKind::Int && y.kind == ValueKind::Int) {
        holds = int_compare(cmp, x.i, y.i);
    } else if (!mixed_operands(x, y) || !float_compare(cmp, x, y, holds)) {
        cache = nullptr;
        holds = is_truthy(binary(rt, code, cmp, op.d, x, y, env));
    }
    return holds == ((op.sub & kJumpWhenTrue) != 0);
}

Value FastVm::builtin(const Runtime& rt, const FastCode::BuiltinSite& site, const Value* args, int32_t argc) {
    thread_local Runtime::Env bridge;
    struct Clear {
        Runtime::Env& env;
        const FastCode::BuiltinSite& site;
        ~Clear() {
            for (const auto& name : site.argNames) env.vars.data[name] = Value();
        }
    } clear{bridge, site};
    for (int32_t k = 0; k < argc; ++k) bridge.vars.data[site.argNames[static_cast<std::size_t>(k)]] = args[k];
    return value_from_legacy_string(rt.eval_builtin_call(site.name, site.args, bridge, true));
}

// Same binding and bookkeeping as the tree-walker's call to a script action.
Value FastVm::call_slow(const Runtime& rt, const Action& action, const Value* args, int32_t argc) {
    Runtime::Env callEnv;
    for (int32_t i = 0; i < argc; ++i) callEnv.vars[action.params[static_cast<std::size_t>(i)].name] = args[i];
    rt.prepare_action_slots(callEnv, action);
    AsyncFlagRestore restore{async_in_async_action()};
    async_set_in_async_action(false);
    Runtime::ExecContext child;
    rt.exec_block(action.body, *rt.currentProgram_, child, callEnv);
    for (auto& thread : child.threads) {
        if (thread.joinable()) thread.join();
    }
    rt.lastReturnFields_ = std::move(child.returnFields);
    return child.returned ? child.returnValue : Value::null_value();
}

Value FastVm::run_func(const Runtime& rt, const FastCode& code, const Value* args, int32_t argc, Arena& ar) {
    Frame frame(ar, static_cast<std::size_t>(code.nregs));
    Value* r = frame.regs;
    for (int32_t i = 0; i < argc; ++i) r[i] = args[i];
    load_consts(code, r);
    Value ret;
    if (exec(rt, code, r, ar.none, ret, ar)) return ret;
    return Value::null_value();
}

bool FastVm::exec(const Runtime& rt, const FastCode& code, Value* r, const Runtime::Env& env, Value& ret, Arena& ar) {
    const Ins* const base = code.ins.data();
    const Ins* ip = base;
    std::unique_ptr<IterState[]> iters;
    if (code.iterators) iters = std::make_unique<IterState[]>(static_cast<std::size_t>(code.iterators));
    OpCount ops;
    const Ins* in = nullptr;
    // GCC and Clang: each op jumps straight to the next one's handler (one indirect branch per
    // op, predicted per site). Elsewhere, and after a plain `break`, the switch dispatches.
#if defined(__GNUC__)
#define ERELANG_OP_LABEL(name) &&op_##name,
    static const void* const labels[] = {ERELANG_FAST_OPS(ERELANG_OP_LABEL)};
#undef ERELANG_OP_LABEL
    static_assert(sizeof labels / sizeof labels[0] == kOpCount);
#define CASE(name) case Op::name: op_##name:
#define NEXT                                                \
    do {                                                    \
        in = ip++;                                          \
        goto* labels[static_cast<std::size_t>(in->op)];     \
    } while (0)
#else
#define CASE(name) case Op::name:
#define NEXT break
#endif
    // Storage of the list last used here. g_lists entries are never erased while a program runs
    // (only Runtime::run clears them), so the pointer stays valid until other code runs: every
    // op that may run tree-walker code or a callee drops it.
    std::vector<Value>* cachedList = nullptr;
    uint32_t cachedListId = 0;
    auto list_of = [&](uint32_t id) -> std::vector<Value>* {
        if (cachedList && id == cachedListId) return cachedList;
        auto it = g_lists.find(static_cast<int>(id));
        if (it == g_lists.end()) return nullptr;
        cachedListId = id;
        cachedList = &it->second;
        return cachedList;
    };
    // The float branch is float_binary's for + - *.
#define INT_BIN(name, wrap, fop, konst)                                     \
    CASE(name) {                                                            \
        const Value& x = r[in->b];                                          \
        const Value& y = r[in->c];                                          \
        if (x.kind == ValueKind::Int && (konst || y.kind == ValueKind::Int)) { \
            set_int(r[in->a], wrap(x.i, y.i));                              \
        } else if (is_number(x) && is_number(y)) { /* not both ints */     \
            set_float(r[in->a], as_double(x) fop as_double(y));             \
        } else {                                                            \
            bin_generic(rt, code, r, *in, env, cachedList);                                               \
        }                                                                   \
        NEXT;                                                               \
    }
    // The loop jump after a fused step (see IncIntLt), decided here for ints. A real branch, not
    // a select: a conditional move would make the next dispatch wait for the comparison.
#define FUSED_JMP(cmp)                                                      \
    {                                                                       \
        const Ins* jump = ip;                                               \
        const Value& x = r[jump->a];                                        \
        const Value& y = r[jump->b];                                        \
        if (x.kind == ValueKind::Int && y.kind == ValueKind::Int) {         \
            if (x.i cmp y.i) [[likely]] {                                   \
                ip = base + jump->c;                                        \
                NEXT;                                                       \
            }                                                               \
            ++ip;                                                           \
        }                                                                   \
    }
#define INC_INT_JMP(name, cmp)                                              \
    CASE(name) {                                                            \
        Value& v = r[in->a];                                                \
        set_int(v, wrap_add(v.kind == ValueKind::Int ? v.i : value_as_int(v), in->b)); \
        FUSED_JMP(cmp)                                                      \
        NEXT;                                                               \
    }
#define ADD_K_JMP(name, cmp)                                                \
    CASE(name) {                                                            \
        const Value& x = r[in->b];                                          \
        if (x.kind == ValueKind::Int) {                                     \
            set_int(r[in->a], wrap_add(x.i, r[in->c].i));                   \
            FUSED_JMP(cmp)                                                  \
        } else if (x.kind == ValueKind::Float) {                            \
            set_float(r[in->a], x.f + static_cast<double>(r[in->c].i));     \
        } else {                                                            \
            bin_generic(rt, code, r, *in, env, cachedList);                 \
        }                                                                   \
        NEXT;                                                               \
    }
#define INT_JMP(name, cmp, konst)                                           \
    CASE(name) {                                                            \
        const Value& x = r[in->a];                                          \
        const Value& y = r[in->b];                                          \
        if (x.kind == ValueKind::Int && (konst || y.kind == ValueKind::Int)) { \
            if (x.i cmp y.i) ip = base + in->c;                             \
        } else if (cmp_generic(rt, code, r, *in, env, cachedList)) {                                      \
            ip = base + in->c;                                              \
        }                                                                   \
        NEXT;                                                               \
    }
    for (;;) {
        in = ip++;
        switch (in->op) {
            CASE(Move) {
                const Value& s = r[in->b];
                Value& d = r[in->a];
                if (s.kind != ValueKind::String && !d.str) {
                    d.kind = s.kind;
                    std::memcpy(&d.i, &s.i, sizeof d.i);  // the whole payload union
                } else {
                    d = s;
                }
                NEXT;
            }
            INT_BIN(AddInt, wrap_add, +, false)
            INT_BIN(SubInt, wrap_sub, -, false)
            INT_BIN(MulInt, wrap_mul, *, false)
            INT_BIN(AddIntK, wrap_add, +, true)
            INT_BIN(SubIntK, wrap_sub, -, true)
            INT_BIN(MulIntK, wrap_mul, *, true)
            INT_JMP(JmpLt, <, false)
            INT_JMP(JmpLe, <=, false)
            INT_JMP(JmpGt, >, false)
            INT_JMP(JmpGe, >=, false)
            INT_JMP(JmpEq, ==, false)
            INT_JMP(JmpNe, !=, false)
            INT_JMP(JmpLtK, <, true)
            INT_JMP(JmpLeK, <=, true)
            INT_JMP(JmpGtK, >, true)
            INT_JMP(JmpGeK, >=, true)
            INT_JMP(JmpEqK, ==, true)
            INT_JMP(JmpNeK, !=, true)
            INC_INT_JMP(IncIntLt, <)
            INC_INT_JMP(IncIntLe, <=)
            ADD_K_JMP(AddIntKLt, <)
            ADD_K_JMP(AddIntKLe, <=)
            CASE(Bin) {
                const Value& x = r[in->b];
                const Value& y = r[in->c];
                if (!(x.kind == ValueKind::Int && y.kind == ValueKind::Int && int_binary(in->sub, x.i, y.i, r[in->a]))) {
                    bin_generic(rt, code, r, *in, env, cachedList);
                }
                NEXT;
            }
            CASE(Unary) {
                const Value& x = r[in->b];
                if (in->sub == static_cast<uint8_t>(ValueUnOp::Neg)) {
                    if (x.kind == ValueKind::Int) {
                        set_int(r[in->a], wrap_sub(0, x.i));
                        break;
                    }
                    if (x.kind == ValueKind::Float) {
                        set_float(r[in->a], -x.f);
                        break;
                    }
                }
                r[in->a] = apply_unary(static_cast<ValueUnOp>(in->sub), x);
                break;
            }
            CASE(Short) {
                const Value& x = r[in->b];
                if (in->sub == 0) {
                    if (!is_truthy(x)) {
                        set_bool(r[in->a], false);
                        ip = base + in->c;
                    }
                } else if (in->sub == 1) {
                    if (is_truthy(x)) {
                        set_bool(r[in->a], true);
                        ip = base + in->c;
                    }
                } else if (x.kind != ValueKind::Null) {
                    r[in->a] = x;
                    ip = base + in->c;
                }
                break;
            }
            CASE(Jmp)
                ip = base + in->a;
                NEXT;
            CASE(JmpFalse)
                if (!is_truthy(r[in->a])) ip = base + in->b;
                NEXT;
            CASE(JmpTrue)
                if (is_truthy(r[in->a])) ip = base + in->b;
                NEXT;
            CASE(CmpJmpFalse)
            CASE(CmpJmpTrue) {
                const Value& x = r[in->a];
                const Value& y = r[in->b];
                bool holds = false;
                if (x.kind == ValueKind::Int && y.kind == ValueKind::Int) {
                    holds = int_compare(in->sub, x.i, y.i);
                } else if (!mixed_operands(x, y) || !float_compare(in->sub, x, y, holds)) {
                    cachedList = nullptr;
                    holds = is_truthy(binary(rt, code, in->sub, in->d, x, y, env));
                }
                if (holds == (in->op == Op::CmpJmpTrue)) ip = base + in->c;
                NEXT;
            }
            CASE(Print) {
                const Value& v = r[in->a];
                if (v.kind == ValueKind::String) {
                    std::cout << v.string_ref() << '\n';
                } else {
                    std::cout << to_display_string(v) << '\n';
                }
                break;
            }
            CASE(Concat) {
                std::string out;
                for (int32_t k = 0; k < in->c; ++k) {
                    const Value& v = r[in->b + k];
                    if (v.kind == ValueKind::String) {
                        out += v.string_ref();
                    } else {
                        out += to_display_string(v);
                    }
                }
                r[in->a] = Value::from_string(std::move(out));
                break;
            }
            CASE(IncInt)
                set_int(r[in->a], wrap_add(value_as_int(r[in->a]), in->b));
                NEXT;
            CASE(CheckType) {
                const FastCode::LetCheck& let = code.lets[static_cast<std::size_t>(in->b)];
                const Value& v = r[in->a];
                if (((let.okKinds >> static_cast<unsigned>(v.kind)) & 1u) || (collection_kind(v) & let.okCollections)) {
                    break;
                }
                const std::string actual = Runtime::runtime_value_type(v, env);
                if (!runtime_declared_type_matches(let.declared, actual)) {
                    throw std::runtime_error("Type mismatch in declaration '" + let.name + "': declared " +
                                             let.declared + " but got " + actual);
                }
                break;
            }
            CASE(ToCount)
                set_int(r[in->a], to_int(to_display_string(r[in->b])));
                break;
            CASE(Call) {
                const FastCode::CallSite& site = code.calls[static_cast<std::size_t>(in->b)];
                const FastCode* callee = site.func->code.load(std::memory_order_acquire);
                const Value* args = r + in->c;
                const auto argc = static_cast<std::size_t>(in->d);
                // An aware caller's lists were scanned already; others may have been changed by
                // tree-walker code since.
                const bool fast = callee && entry_kinds_match(*callee, args) &&
                                  (code.aware || admit(*callee, args, argc));
                cachedList = nullptr;
                Value result = fast ? run_func(rt, *callee, args, in->d, ar) : call_slow(rt, *site.action, args, in->d);
                r[in->a] = std::move(result);
                break;
            }
            CASE(Builtin) {
                if (in->sub && native_conversion(in->sub, r[in->c], r[in->a])) break;
                cachedList = nullptr;
                Value result = builtin(rt, code.builtins[static_cast<std::size_t>(in->b)], r + in->c, in->d);
                r[in->a] = std::move(result);
                break;
            }
            CASE(NewList) {
                std::vector<Value> items(r + in->b, r + in->b + in->c);
                r[in->a] = make_list_value(std::move(items));
                ++ops.n;
                break;
            }
            CASE(NewDict)
                r[in->a] = new_dict_value(static_cast<std::size_t>(in->b));
                break;
            CASE(DictPut) {
                const Value& key = r[in->b];
                auto& dict = g_dicts[static_cast<int>(value_handle_id(r[in->a]))];
                if (key.kind == ValueKind::String) dict[key.string_ref()] = r[in->c];
                else dict[to_display_string(key)] = r[in->c];
                ++ops.n;
                break;
            }
            CASE(Range)
                r[in->a] = make_range_list(value_as_int(r[in->b]), value_as_int(r[in->c]), in->sub != 0);
                ++ops.n;
                break;
            CASE(Index) {
                const Value& object = r[in->b];
                const Value& key = r[in->c];
                if (value_is_handle(object, HandleKind::List) && key.kind == ValueKind::Int) {
                    // collection_index_get for a list and an int index.
                    std::vector<Value>* list = list_of(object.h.id);
                    if (list && key.i >= 0 && static_cast<uint64_t>(key.i) < list->size()) {
                        r[in->a] = (*list)[static_cast<std::size_t>(key.i)];
                    } else {
                        set_int(r[in->a], 0);
                    }
                    ++ops.n;
                    NEXT;
                }
                // Beyond lists and maps the tree-walker indexes buffers and structs, which never
                // enter a frame; anything else gives null.
                Value item;
                (void)collection_index_get(r[in->b], r[in->c], item);
                r[in->a] = std::move(item);
                ++ops.n;
                break;
            }
            CASE(IndexSet)
                if (value_is_handle(r[in->b], HandleKind::List) && r[in->c].kind == ValueKind::Int) {
                    std::vector<Value>* list = list_of(r[in->b].h.id);
                    const int64_t at = r[in->c].i;
                    if (list && at >= 0 && static_cast<uint64_t>(at) < list->size()) {
                        (*list)[static_cast<std::size_t>(at)] = r[in->a];
                        ++ops.n;
                        NEXT;
                    }
                }
                if (!collection_index_set(r[in->b], r[in->c], r[in->a])) {
                    throw std::runtime_error("index assignment requires buffer, list or map");
                }
                ++ops.n;
                break;
            CASE(Method) {
                const FastCode::MethodSite& site = code.methods[static_cast<std::size_t>(in->b)];
                const Value& recv = r[in->c];
                if (!collection_kind(recv)) {
                    // Entry checks and typed lets rule this out.
                    throw std::runtime_error("internal error: compiled " + site.name + "() on a value that is not a list or map");
                }
                std::vector<Value>* list =
                    site.listOp && recv.h.kind == HandleKind::List ? list_of(recv.h.id) : nullptr;
                // Inline forms of collection_method's len / push / get; anything else (a missing
                // list, a push to a fixed-size array) goes through it.
                if (list && site.listOp == kListLen) {
                    set_int(r[in->a], static_cast<int64_t>(list->size()));
                    ++ops.n;
                    NEXT;
                }
                if (list && site.listOp == kListPush && !list_is_fixed(static_cast<int>(recv.h.id))) {
                    list->push_back(r[in->d]);
                    if (!site.statement || !site.isVoid) r[in->a] = Value::null_value();
                    ++ops.n;
                    NEXT;
                }
                if (list && site.listOp == kListGet) {
                    const int64_t at = value_as_int(r[in->d]);
                    if (at >= 0 && static_cast<uint64_t>(at) < list->size()) {
                        r[in->a] = (*list)[static_cast<std::size_t>(at)];
                    } else {
                        r[in->a] = Value::from_string("");
                    }
                    ++ops.n;
                    NEXT;
                }
                std::optional<Value> result;
                if (site.argc == 0) {
                    static const std::vector<Value> none;
                    result = rt.collection_method(recv, site.name, none);
                } else {
                    // collection_method never re-enters compiled code, so one buffer per thread is enough.
                    thread_local std::vector<Value> args;
                    struct Clear {
                        std::vector<Value>& v;
                        ~Clear() { v.clear(); }
                    } clear{args};
                    args.assign(r + in->d, r + in->d + site.argc);
                    result = rt.collection_method(recv, site.name, args);
                }
                if (!result) {
                    throw std::runtime_error("internal error: compiled " + site.name + "() is not a list or map method");
                }
                if (!site.statement || result->kind != ValueKind::Null || !site.isVoid) r[in->a] = std::move(*result);
                ++ops.n;
                break;
            }
            CASE(IterInit) {
                IterState& st = iters[static_cast<std::size_t>(in->b)];
                const Value& v = r[in->a];
                st.kind = 0;
                st.snapshot.reset();
                if (value_is_handle(v, HandleKind::List)) {
                    if (in->sub) throw std::runtime_error("List iteration supports only one variable: for (item : list)");
                    st.id = static_cast<int>(value_handle_id(v));
                    auto it = g_lists.find(st.id);
                    if (it != g_lists.end()) {
                        st.kind = kKindList;
                        st.index = 0;
                        st.count = it->second.size();
                    }
                } else if (value_is_handle(v, HandleKind::Dict)) {
                    auto it = g_dicts.find(static_cast<int>(value_handle_id(v)));
                    if (it != g_dicts.end()) {
                        st.snapshot.emplace(it->second);
                        st.it = st.snapshot->cbegin();
                        st.kind = kKindDict;
                    }
                }
                break;
            }
            CASE(IterNext) {
                IterState& st = iters[static_cast<std::size_t>(in->b)];
                bool done = true;
                if (st.kind == kKindList) {
                    if (st.index < st.count) {
                        std::vector<Value>* cur = list_of(static_cast<uint32_t>(st.id));
                        if (cur && st.index < cur->size()) {
                            r[in->a] = (*cur)[st.index++];
                            done = false;
                        }
                    }
                } else if (st.kind == kKindDict && st.it != st.snapshot->cend()) {
                    r[in->a] = value_from_legacy_string(st.it->first);
                    if (in->sub) r[in->d] = st.it->second;
                    ++st.it;
                    done = false;
                }
                if (done) {
                    st.kind = 0;
                    st.snapshot.reset();
                    ip = base + in->c;
                }
                ++ops.n;
                break;
            }
            CASE(Ret)
                ret = r[in->a];
                return true;
            CASE(End)
                return false;
        }
    }
#undef ADD_K_JMP
#undef INC_INT_JMP
#undef FUSED_JMP
#undef INT_JMP
#undef INT_BIN
#undef NEXT
#undef CASE
}

template <class Node>
bool FastVm::run_loop(const Runtime& rt, const Node& node, Runtime::ExecContext& ctx, Runtime::Env& env) {
    if (Runtime::debug_enabled() || rt.hasDynamicActions_.load(std::memory_order_acquire) ||
        !rt.currentProgram_ || !env.cells.empty() || env.useSlots) {
        return false;
    }
    std::shared_ptr<FastVm> vm;  // keeps the code alive if a nested run replaces the cache
    const FastCode* code = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_vmMutex);
        if (!rt.fastVm_ || rt.fastVm_->program() != rt.currentProgram_) {
            rt.fastVm_ = std::make_shared<FastVm>(rt, rt.currentProgram_);
        }
        vm = rt.fastVm_;
        code = vm->loop_locked(node);
    }
    if (!code) return false;
    for (const auto& name : code->unbound) {
        if (env.vars.data.count(name) || env.objects.count(name)) return false;
    }

    Arena& ar = arena();
    Frame frame(ar, static_cast<std::size_t>(code->nregs));
    Value* r = frame.regs;
    const std::size_t nvars = code->names.size();
    std::vector<char> external(nvars, 0);
    for (std::size_t i = 0; i < nvars; ++i) {
        const std::string& name = code->names[i];
        if (name.empty()) continue;
        if (const Value* bound = rt.env_find(env, name)) {
            if (!vm_safe(*bound)) return false;
            if (code->hasCalls) {
                // A callee could change a global the frame has a copy of, or read struct
                // fields passed along with a string argument.
                if (!env.vars.data.count(name)) return false;
                if (value_may_carry_fields(bound) && has_field_vars(env, name)) return false;
            }
            r[i] = *bound;
            external[i] = 1;
        } else {
            if (!code->assigned[i]) return false;
            mark_unset(r[i]);
        }
    }
    if (!entry_kinds_match(*code, r) || !admit(*code, r, nvars)) return false;
    load_consts(*code, r);

    auto write_back = [&] {
        for (std::size_t i = 0; i < nvars; ++i) {
            const std::string& name = code->names[i];
            if (name.empty()) continue;
            if (external[i] ? code->assigned[i] : !is_unset(r[i])) rt.env_set(env, name, r[i]);
        }
    };
    Value ret;
    bool returned = false;
    try {
        returned = exec(rt, *code, r, env, ret, ar);
    } catch (...) {
        write_back();
        throw;
    }
    write_back();
    if (returned) {
        ctx.returnFields.clear();
        ctx.returned = true;
        ctx.returnValue = std::move(ret);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Runtime entry points

bool Runtime::fast_call(const Action& action, const std::vector<ExprPtr>& args, const Env& env,
                        bool statementCall, Value& result) const {
    if (action.isAsync || args.size() != action.params.size() || debug_enabled() ||
        hasDynamicActions_.load(std::memory_order_acquire) || !currentProgram_) {
        return false;
    }
    std::shared_ptr<FastVm> vm;  // keeps the code alive if a nested run replaces the cache
    const FastCode* code = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_vmMutex);
        if (!fastVm_ || fastVm_->program() != currentProgram_) {
            fastVm_ = std::make_shared<FastVm>(*this, currentProgram_);
        }
        vm = fastVm_;
        code = vm->func_locked(action)->code.load(std::memory_order_acquire);
    }
    if (!code) return false;

    // Arguments passed by name bind extra state (objects, struct fields) in the tree-walker.
    for (const auto& arg : args) {
        if (!arg) return false;
        if (const auto* id = std::get_if<ExprIdent>(&arg->node)) {
            if (!env.objects.empty() && env.objects.count(id->name)) return false;
            const Value* bound = env_find(env, id->name);
            if (!bound || !vm_safe(*bound)) return false;
            if (value_may_carry_fields(bound) && has_field_vars(env, id->name)) return false;
        }
    }

    std::vector<Value> values;
    values.reserve(args.size());
    for (const auto& arg : args) {
        const bool literalArg = !std::holds_alternative<ExprIdent>(arg->node);
        values.push_back(statementCall && literalArg ? eval_arg(*arg, env) : eval_value(*arg, env));
    }
    const bool admitted = std::all_of(values.begin(), values.end(), vm_safe) &&
                          entry_kinds_match(*code, values.data()) && admit(*code, values.data(), values.size());
    if (!admitted) {
        // An argument holds a handle, a struct or a list with one of those inside: bind it the
        // tree-walker's way.
        Env callEnv;
        for (std::size_t i = 0; i < values.size(); ++i) {
            const std::string& parameter = action.params[i].name;
            callEnv.vars[parameter] = values[i];
            if (!statementCall && value_is_struct_marker(values[i])) {
                for (const auto& kv : lastReturnFields_) callEnv.vars[parameter + "." + kv.first] = kv.second;
            }
        }
        prepare_action_slots(callEnv, action);
        AsyncFlagRestore restore{async_in_async_action()};
        async_set_in_async_action(false);
        ExecContext child;
        exec_block(action.body, *currentProgram_, child, callEnv);
        for (auto& thread : child.threads) {
            if (thread.joinable()) thread.join();
        }
        if (!statementCall) {
            lastReturnFields_ = std::move(child.returnFields);
            result = child.returned ? child.returnValue : Value::null_value();
        }
        return true;
    }

    AsyncFlagRestore restore{async_in_async_action()};
    async_set_in_async_action(false);
    Value out = FastVm::run_func(*this, *code, values.data(), static_cast<int32_t>(values.size()), arena());
    if (!statementCall) {
        lastReturnFields_.clear();
        result = std::move(out);
    }
    return true;
}

bool Runtime::fast_loop(const WhileStmt& loop, ExecContext& ctx, Env& env) const {
    return FastVm::run_loop(*this, loop, ctx, env);
}
bool Runtime::fast_loop(const ForStmt& loop, ExecContext& ctx, Env& env) const {
    return FastVm::run_loop(*this, loop, ctx, env);
}
bool Runtime::fast_loop(const DoWhileStmt& loop, ExecContext& ctx, Env& env) const {
    return FastVm::run_loop(*this, loop, ctx, env);
}
bool Runtime::fast_loop(const RepeatStmt& loop, ExecContext& ctx, Env& env) const {
    return FastVm::run_loop(*this, loop, ctx, env);
}
bool Runtime::fast_loop(const ForInStmt& loop, ExecContext& ctx, Env& env) const {
    return FastVm::run_loop(*this, loop, ctx, env);
}

void Runtime::fast_vm_reset() const {
    std::lock_guard<std::mutex> lock(g_vmMutex);
    fastVm_.reset();
}

} // namespace erelang
