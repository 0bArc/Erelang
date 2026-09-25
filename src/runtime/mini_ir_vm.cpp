#include "erelang/mini_ir_vm.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace erelang {
namespace mini_ir {
namespace {

enum class Op : std::uint8_t {
    Const,
    ConstStr,
    Add,
    Sub,
    Mul,
    Div,
    CmpEq,
    CmpNe,
    CmpLt,
    Load,
    Store,
    Label,
    Jump,
    JumpIfFalse,
    Print,
    PrintStr,
    Pop,
    Call,
    MkEnum,
    MkStruct,
    GetField,
    SetField,
    JumpIfNotVariant,
    GetPayload,
    Return
};

struct Instr {
    Op op = Op::Return;
    std::int64_t a = 0;
    std::int64_t b = 0;
    std::string s;
};

struct Func {
    std::string name;
    int arity = 0;
    int locals = 0;
    std::vector<Instr> code;
    std::vector<int> labels;
};

struct Module {
    int local_count = 0;
    int label_count = 0;
    std::vector<Instr> entry;
    std::vector<int> entry_labels;
    std::vector<Func> funcs;
    std::unordered_map<std::string, int> func_index;
};

enum class HeapKind : std::uint8_t { Empty, String, Enum, Struct };

struct HeapObj {
    HeapKind kind = HeapKind::Empty;
    std::string tag_or_fields;
    std::string str;
    std::vector<std::int64_t> vals;
};

struct VmState {
    std::vector<HeapObj> heap;
    bool did_print = false;
    std::int64_t last_print = 0;
    std::string last_print_str;
};

[[nodiscard]] bool is_handle(std::int64_t v) { return v < 0; }
[[nodiscard]] int from_handle(std::int64_t h) { return static_cast<int>(-(h)-1); }
[[nodiscard]] std::int64_t to_handle(int idx) { return -(static_cast<std::int64_t>(idx) + 1); }

[[nodiscard]] std::int64_t alloc(VmState& st, HeapObj obj) {
    const int idx = static_cast<int>(st.heap.size());
    st.heap.push_back(std::move(obj));
    return to_handle(idx);
}

[[nodiscard]] HeapObj* heap_get(VmState& st, std::int64_t handle) {
    if (!is_handle(handle)) return nullptr;
    const int idx = from_handle(handle);
    if (idx < 0 || idx >= static_cast<int>(st.heap.size())) return nullptr;
    return &st.heap[static_cast<size_t>(idx)];
}

[[nodiscard]] bool heap_is_string(VmState& st, std::int64_t h) {
    const HeapObj* o = heap_get(st, h);
    return o && o->kind == HeapKind::String;
}

[[nodiscard]] std::string heap_as_string(VmState& st, std::int64_t h) {
    const HeapObj* o = heap_get(st, h);
    if (!o || o->kind != HeapKind::String) return {};
    return o->str;
}

[[nodiscard]] int csv_count(const std::string& s) {
    if (s.empty()) return 0;
    int n = 1;
    for (char c : s) {
        if (c == ',') ++n;
    }
    return n;
}

[[nodiscard]] std::string csv_at(const std::string& s, int idx) {
    int cur = 0;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == ',') {
            if (cur == idx) return s.substr(start, i - start);
            ++cur;
            start = i + 1;
        }
    }
    return {};
}

[[nodiscard]] int csv_index(const std::string& s, const std::string& name) {
    const int n = csv_count(s);
    for (int i = 0; i < n; ++i) {
        if (csv_at(s, i) == name) return i;
    }
    return -1;
}

[[nodiscard]] std::string dotted_tail(const std::string& n) {
    const auto pos = n.rfind('.');
    if (pos == std::string::npos) return n;
    return n.substr(pos + 1);
}

[[nodiscard]] bool char_is_digit(const std::string& s) {
    return s.size() == 1 && std::isdigit(static_cast<unsigned char>(s[0]));
}
[[nodiscard]] bool char_is_alpha(const std::string& s) {
    return s.size() == 1 && std::isalpha(static_cast<unsigned char>(s[0]));
}
[[nodiscard]] bool char_is_space(const std::string& s) {
    return s.size() == 1 && std::isspace(static_cast<unsigned char>(s[0]));
}

void build_labels(const std::vector<Instr>& code, std::vector<int>& labels) {
    int max_lab = -1;
    for (const auto& ins : code) {
        if (ins.op == Op::Label) {
            max_lab = std::max(max_lab, static_cast<int>(ins.a));
        }
    }
    labels.assign(static_cast<size_t>(max_lab + 1), 0);
    for (int i = 0; i < static_cast<int>(code.size()); ++i) {
        if (code[static_cast<size_t>(i)].op == Op::Label) {
            const int n = static_cast<int>(code[static_cast<size_t>(i)].a);
            if (n >= 0 && n < static_cast<int>(labels.size())) {
                labels[static_cast<size_t>(n)] = i;
            }
        }
    }
}

struct Cursor {
    const std::string* text = nullptr;
    size_t i = 0;

    [[nodiscard]] bool eof() const { return !text || i >= text->size(); }
    [[nodiscard]] char peek() const { return eof() ? '\0' : (*text)[i]; }
    char get() { return eof() ? '\0' : (*text)[i++]; }

    void skip_ws_line() {
        while (!eof()) {
            const char c = peek();
            if (c == ' ' || c == '\t' || c == '\r') {
                get();
                continue;
            }
            break;
        }
    }

    void skip_blank_and_comments() {
        while (!eof()) {
            skip_ws_line();
            if (peek() == '\n') {
                get();
                continue;
            }
            if (peek() == '#') {
                while (!eof() && peek() != '\n') get();
                if (peek() == '\n') get();
                continue;
            }
            break;
        }
    }

    [[nodiscard]] bool starts_with(const char* s) const {
        size_t j = 0;
        while (s[j]) {
            if (i + j >= text->size() || (*text)[i + j] != s[j]) return false;
            ++j;
        }
        return true;
    }

    bool consume(const char* s) {
        if (!starts_with(s)) return false;
        i += std::char_traits<char>::length(s);
        return true;
    }

    [[nodiscard]] std::string read_token() {
        skip_ws_line();
        std::string out;
        while (!eof()) {
            const char c = peek();
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') break;
            out.push_back(get());
        }
        return out;
    }

    [[nodiscard]] bool read_i64(std::int64_t& out) {
        skip_ws_line();
        if (eof()) return false;
        size_t start = i;
        if (peek() == '-' || peek() == '+') get();
        if (!std::isdigit(static_cast<unsigned char>(peek()))) {
            i = start;
            return false;
        }
        while (std::isdigit(static_cast<unsigned char>(peek()))) get();
        try {
            out = std::stoll(text->substr(start, i - start));
        } catch (...) {
            return false;
        }
        return true;
    }

    [[nodiscard]] bool read_len_str(std::string& out) {
        skip_ws_line();
        std::int64_t len = 0;
        if (!read_i64(len) || len < 0) return false;
        if (get() != ':') return false;
        if (i + static_cast<size_t>(len) > text->size()) return false;
        out = text->substr(i, static_cast<size_t>(len));
        i += static_cast<size_t>(len);
        return true;
    }

    void expect_eol() {
        skip_ws_line();
        if (peek() == '\n') get();
    }
};

[[nodiscard]] bool parse_instr(Cursor& c, Instr& ins, std::string& err) {
    c.skip_blank_and_comments();
    if (c.eof()) return false;
    if (c.peek() == '.') return false;

    const std::string op = c.read_token();
    if (op.empty()) return false;

    auto need_int = [&](std::int64_t& v) -> bool {
        if (!c.read_i64(v)) {
            err = "expected int after " + op;
            return false;
        }
        return true;
    };
    auto need_str = [&](std::string& s) -> bool {
        if (!c.read_len_str(s)) {
            err = "expected len:str after " + op;
            return false;
        }
        return true;
    };

    if (op == "CONST") {
        ins.op = Op::Const;
        if (!need_int(ins.a)) return false;
    } else if (op == "CONST_STR") {
        ins.op = Op::ConstStr;
        if (!need_str(ins.s)) return false;
    } else if (op == "ADD") {
        ins.op = Op::Add;
    } else if (op == "SUB") {
        ins.op = Op::Sub;
    } else if (op == "MUL") {
        ins.op = Op::Mul;
    } else if (op == "DIV") {
        ins.op = Op::Div;
    } else if (op == "CMP_EQ") {
        ins.op = Op::CmpEq;
    } else if (op == "CMP_NE") {
        ins.op = Op::CmpNe;
    } else if (op == "CMP_LT") {
        ins.op = Op::CmpLt;
    } else if (op == "LOAD") {
        ins.op = Op::Load;
        if (!need_int(ins.a)) return false;
    } else if (op == "STORE") {
        ins.op = Op::Store;
        if (!need_int(ins.a)) return false;
    } else if (op == "LABEL") {
        ins.op = Op::Label;
        if (!need_int(ins.a)) return false;
    } else if (op == "JUMP") {
        ins.op = Op::Jump;
        if (!need_int(ins.a)) return false;
    } else if (op == "JUMP_IF_FALSE") {
        ins.op = Op::JumpIfFalse;
        if (!need_int(ins.a)) return false;
    } else if (op == "PRINT") {
        ins.op = Op::Print;
    } else if (op == "PRINT_STR") {
        ins.op = Op::PrintStr;
    } else if (op == "POP") {
        ins.op = Op::Pop;
    } else if (op == "CALL") {
        ins.op = Op::Call;
        if (!need_int(ins.a)) return false;
        if (!need_str(ins.s)) return false;
    } else if (op == "MK_ENUM") {
        ins.op = Op::MkEnum;
        if (!need_int(ins.a)) return false;
        if (!need_str(ins.s)) return false;
    } else if (op == "MK_STRUCT") {
        ins.op = Op::MkStruct;
        if (!need_str(ins.s)) return false;
    } else if (op == "GET_FIELD") {
        ins.op = Op::GetField;
        if (!need_str(ins.s)) return false;
    } else if (op == "SET_FIELD") {
        ins.op = Op::SetField;
        if (!need_str(ins.s)) return false;
    } else if (op == "JUMP_IF_NOT_VARIANT") {
        ins.op = Op::JumpIfNotVariant;
        if (!need_int(ins.a)) return false;
        if (!need_str(ins.s)) return false;
    } else if (op == "GET_PAYLOAD") {
        ins.op = Op::GetPayload;
        if (!need_int(ins.a)) return false;
    } else if (op == "RETURN") {
        ins.op = Op::Return;
    } else {
        err = "unknown op " + op;
        return false;
    }
    c.expect_eol();
    return true;
}

[[nodiscard]] bool parse_code_block(Cursor& c, std::vector<Instr>& code, std::string& err) {
    while (true) {
        c.skip_blank_and_comments();
        if (c.eof() || c.peek() == '.') break;
        Instr ins;
        if (!parse_instr(c, ins, err)) {
            if (err.empty()) err = "bad instruction";
            return false;
        }
        code.push_back(std::move(ins));
    }
    return true;
}

[[nodiscard]] bool parse_module(const std::string& text, Module& mod, std::string& err) {
    Cursor c;
    c.text = &text;
    c.i = 0;
    bool saw_entry = false;

    while (!c.eof()) {
        c.skip_blank_and_comments();
        if (c.eof()) break;
        if (c.consume(".locals")) {
            std::int64_t v = 0;
            if (!c.read_i64(v)) {
                err = "bad .locals";
                return false;
            }
            mod.local_count = static_cast<int>(v);
            c.expect_eol();
            continue;
        }
        if (c.consume(".labels")) {
            std::int64_t v = 0;
            if (!c.read_i64(v)) {
                err = "bad .labels";
                return false;
            }
            mod.label_count = static_cast<int>(v);
            c.expect_eol();
            continue;
        }
        if (c.consume(".entry")) {
            c.expect_eol();
            if (!parse_code_block(c, mod.entry, err)) return false;
            build_labels(mod.entry, mod.entry_labels);
            saw_entry = true;
            continue;
        }
        if (c.consume(".func")) {
            Func fn;
            std::int64_t arity = 0;
            std::int64_t locals = 0;
            if (!c.read_len_str(fn.name) || !c.read_i64(arity) || !c.read_i64(locals)) {
                err = "bad .func header";
                return false;
            }
            fn.arity = static_cast<int>(arity);
            fn.locals = static_cast<int>(locals);
            c.expect_eol();
            if (!parse_code_block(c, fn.code, err)) return false;
            build_labels(fn.code, fn.labels);
            mod.func_index[fn.name] = static_cast<int>(mod.funcs.size());
            mod.funcs.push_back(std::move(fn));
            continue;
        }
        err = "unexpected content near offset " + std::to_string(c.i);
        return false;
    }
    if (!saw_entry) {
        err = "missing .entry";
        return false;
    }
    return true;
}

[[nodiscard]] std::int64_t locals_get(const std::vector<std::int64_t>& locals, int idx) {
    if (idx < 0 || idx >= static_cast<int>(locals.size())) return 0;
    return locals[static_cast<size_t>(idx)];
}

void locals_set(std::vector<std::int64_t>& locals, int idx, std::int64_t value) {
    if (idx < 0) return;
    if (idx >= static_cast<int>(locals.size())) {
        locals.resize(static_cast<size_t>(idx + 1), 0);
    }
    locals[static_cast<size_t>(idx)] = value;
}

[[nodiscard]] std::int64_t stack_top(const std::vector<std::int64_t>& st) {
    return st.empty() ? 0 : st.back();
}
void stack_push(std::vector<std::int64_t>& st, std::int64_t v) { st.push_back(v); }
void stack_pop(std::vector<std::int64_t>& st) {
    if (!st.empty()) st.pop_back();
}

[[nodiscard]] std::string read_file_all(const std::string& path, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "cannot open " + path;
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

[[nodiscard]] std::string fs_read(const std::string& path) {
    std::string err;
    return read_file_all(path, err);
}

[[nodiscard]] bool fs_exists(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return static_cast<bool>(in);
}

[[nodiscard]] std::string path_join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    const char last = a.back();
    if (last == '/' || last == '\\') return a + b;
    return a + "/" + b;
}

[[nodiscard]] std::string path_dirname(const std::string& p) {
    const auto pos = p.find_last_of("/\\");
    if (pos == std::string::npos) return ".";
    if (pos == 0) return p.substr(0, 1);
    return p.substr(0, pos);
}

[[nodiscard]] std::string strip_ws(std::string s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

RunResult run_code(const Module& mod,
                   const std::vector<Instr>& code,
                   const std::vector<int>& labels,
                   std::vector<std::int64_t> locals,
                   VmState& st);

[[nodiscard]] bool try_builtin(const std::string& n,
                               int argc,
                               std::vector<std::int64_t>& call_locals,
                               std::vector<std::int64_t>& stack,
                               VmState& st) {
    auto push = [&](std::int64_t v) { stack_push(stack, v); };

    if (n == "string.len" || n == "len") {
        push(static_cast<std::int64_t>(heap_as_string(st, locals_get(call_locals, 0)).size()));
        return true;
    }
    if (n == "string.substr" || n == "substr") {
        const std::string s = heap_as_string(st, locals_get(call_locals, 0));
        const int start = static_cast<int>(locals_get(call_locals, 1));
        int span = 0;
        if (argc >= 3) span = static_cast<int>(locals_get(call_locals, 2));
        else span = static_cast<int>(s.size()) - start;
        std::string out;
        if (start >= 0 && start <= static_cast<int>(s.size())) {
            const int max_span = static_cast<int>(s.size()) - start;
            if (span < 0) span = 0;
            if (span > max_span) span = max_span;
            out = s.substr(static_cast<size_t>(start), static_cast<size_t>(span));
        }
        HeapObj obj;
        obj.kind = HeapKind::String;
        obj.str = std::move(out);
        push(alloc(st, std::move(obj)));
        return true;
    }
    if (n == "string.find" || n == "find") {
        const std::string s = heap_as_string(st, locals_get(call_locals, 0));
        const std::string needle = heap_as_string(st, locals_get(call_locals, 1));
        const auto pos = s.find(needle);
        push(pos == std::string::npos ? static_cast<std::int64_t>(-1) : static_cast<std::int64_t>(pos));
        return true;
    }
    if (n == "string.strip" || n == "strip") {
        HeapObj obj;
        obj.kind = HeapKind::String;
        obj.str = strip_ws(heap_as_string(st, locals_get(call_locals, 0)));
        push(alloc(st, std::move(obj)));
        return true;
    }
    if (n == "string.starts_with" || n == "starts_with") {
        const std::string s = heap_as_string(st, locals_get(call_locals, 0));
        const std::string prefix = heap_as_string(st, locals_get(call_locals, 1));
        push(s.rfind(prefix, 0) == 0 ? 1 : 0);
        return true;
    }
    if (n == "string.ends_with" || n == "ends_with") {
        const std::string s = heap_as_string(st, locals_get(call_locals, 0));
        const std::string suffix = heap_as_string(st, locals_get(call_locals, 1));
        if (suffix.size() > s.size()) push(0);
        else push(s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0 ? 1 : 0);
        return true;
    }
    if (n == "string") {
        HeapObj obj;
        obj.kind = HeapKind::String;
        obj.str = std::to_string(locals_get(call_locals, 0));
        push(alloc(st, std::move(obj)));
        return true;
    }
    if (n == "char_is_digit") {
        push(char_is_digit(heap_as_string(st, locals_get(call_locals, 0))) ? 1 : 0);
        return true;
    }
    if (n == "char_is_alpha") {
        push(char_is_alpha(heap_as_string(st, locals_get(call_locals, 0))) ? 1 : 0);
        return true;
    }
    if (n == "char_is_space") {
        push(char_is_space(heap_as_string(st, locals_get(call_locals, 0))) ? 1 : 0);
        return true;
    }
    if (n == "char_is_ident_start") {
        const std::string s = heap_as_string(st, locals_get(call_locals, 0));
        push((char_is_alpha(s) || s == "_") ? 1 : 0);
        return true;
    }
    if (n == "char_is_ident_part") {
        const std::string s = heap_as_string(st, locals_get(call_locals, 0));
        push((char_is_alpha(s) || char_is_digit(s) || s == "_") ? 1 : 0);
        return true;
    }
    if (n == "exit") {
        push(0);
        return true;
    }
    if (n == "fs.read") {
        HeapObj obj;
        obj.kind = HeapKind::String;
        obj.str = fs_read(heap_as_string(st, locals_get(call_locals, 0)));
        push(alloc(st, std::move(obj)));
        return true;
    }
    if (n == "fs.exists") {
        push(fs_exists(heap_as_string(st, locals_get(call_locals, 0))) ? 1 : 0);
        return true;
    }
    if (n == "path.join") {
        HeapObj obj;
        obj.kind = HeapKind::String;
        obj.str = path_join(heap_as_string(st, locals_get(call_locals, 0)),
                            heap_as_string(st, locals_get(call_locals, 1)));
        push(alloc(st, std::move(obj)));
        return true;
    }
    if (n == "path.dirname") {
        HeapObj obj;
        obj.kind = HeapKind::String;
        obj.str = path_dirname(heap_as_string(st, locals_get(call_locals, 0)));
        push(alloc(st, std::move(obj)));
        return true;
    }
    return false;
}

RunResult run_code(const Module& mod,
                   const std::vector<Instr>& code,
                   const std::vector<int>& labels,
                   std::vector<std::int64_t> locals,
                   VmState& st) {
    RunResult out;
    out.ok = true;
    std::vector<std::int64_t> stack;
    int pc = 0;
    std::int64_t guard = 0;
    constexpr std::int64_t kGuard = 50000000;

    auto jump_to = [&](int lab) {
        if (lab >= 0 && lab < static_cast<int>(labels.size())) {
            pc = labels[static_cast<size_t>(lab)];
        } else {
            pc = static_cast<int>(code.size());
        }
    };

    while (guard < kGuard) {
        ++guard;
        if (pc < 0 || pc >= static_cast<int>(code.size())) {
            out.ok = false;
            out.error = "fell off end of code";
            return out;
        }
        const Instr& ins = code[static_cast<size_t>(pc++)];
        switch (ins.op) {
        case Op::Const:
            stack_push(stack, ins.a);
            break;
        case Op::ConstStr: {
            HeapObj obj;
            obj.kind = HeapKind::String;
            obj.str = ins.s;
            stack_push(stack, alloc(st, std::move(obj)));
            break;
        }
        case Op::Add: {
            const std::int64_t b = stack_top(stack);
            stack_pop(stack);
            const std::int64_t a = stack_top(stack);
            stack_pop(stack);
            if (heap_is_string(st, a) && heap_is_string(st, b)) {
                HeapObj obj;
                obj.kind = HeapKind::String;
                obj.str = heap_as_string(st, a) + heap_as_string(st, b);
                stack_push(stack, alloc(st, std::move(obj)));
            } else {
                stack_push(stack, a + b);
            }
            break;
        }
        case Op::Sub: {
            const std::int64_t b = stack_top(stack);
            stack_pop(stack);
            const std::int64_t a = stack_top(stack);
            stack_pop(stack);
            stack_push(stack, a - b);
            break;
        }
        case Op::Mul: {
            const std::int64_t b = stack_top(stack);
            stack_pop(stack);
            const std::int64_t a = stack_top(stack);
            stack_pop(stack);
            stack_push(stack, a * b);
            break;
        }
        case Op::Div: {
            const std::int64_t b = stack_top(stack);
            stack_pop(stack);
            const std::int64_t a = stack_top(stack);
            stack_pop(stack);
            stack_push(stack, b == 0 ? 0 : a / b);
            break;
        }
        case Op::CmpEq: {
            const std::int64_t b = stack_top(stack);
            stack_pop(stack);
            const std::int64_t a = stack_top(stack);
            stack_pop(stack);
            if (heap_is_string(st, a) && heap_is_string(st, b)) {
                stack_push(stack, heap_as_string(st, a) == heap_as_string(st, b) ? 1 : 0);
            } else {
                stack_push(stack, a == b ? 1 : 0);
            }
            break;
        }
        case Op::CmpNe: {
            const std::int64_t b = stack_top(stack);
            stack_pop(stack);
            const std::int64_t a = stack_top(stack);
            stack_pop(stack);
            if (heap_is_string(st, a) && heap_is_string(st, b)) {
                stack_push(stack, heap_as_string(st, a) != heap_as_string(st, b) ? 1 : 0);
            } else {
                stack_push(stack, a != b ? 1 : 0);
            }
            break;
        }
        case Op::CmpLt: {
            const std::int64_t b = stack_top(stack);
            stack_pop(stack);
            const std::int64_t a = stack_top(stack);
            stack_pop(stack);
            stack_push(stack, a < b ? 1 : 0);
            break;
        }
        case Op::Load:
            stack_push(stack, locals_get(locals, static_cast<int>(ins.a)));
            break;
        case Op::Store: {
            const std::int64_t v = stack_top(stack);
            stack_pop(stack);
            locals_set(locals, static_cast<int>(ins.a), v);
            break;
        }
        case Op::Label:
            break;
        case Op::Jump:
            jump_to(static_cast<int>(ins.a));
            break;
        case Op::JumpIfFalse: {
            const std::int64_t v = stack_top(stack);
            stack_pop(stack);
            if (v == 0) jump_to(static_cast<int>(ins.a));
            break;
        }
        case Op::Print: {
            const std::int64_t v = stack_top(stack);
            stack_pop(stack);
            if (heap_is_string(st, v)) {
                const std::string s = heap_as_string(st, v);
                std::cout << s << '\n';
                st.last_print_str = s;
                out.last_print_str = s;
            } else {
                std::cout << v << '\n';
                st.last_print = v;
                out.last_print = v;
            }
            st.did_print = true;
            out.did_print = true;
            break;
        }
        case Op::PrintStr: {
            const std::int64_t h = stack_top(stack);
            stack_pop(stack);
            const std::string s = heap_as_string(st, h);
            std::cout << s << '\n';
            st.last_print_str = s;
            out.last_print_str = s;
            st.did_print = true;
            out.did_print = true;
            break;
        }
        case Op::Pop:
            stack_pop(stack);
            break;
        case Op::Call: {
            const int argc = static_cast<int>(ins.a);
            const std::string& n = ins.s;
            std::vector<std::int64_t> call_locals;
            for (int i = argc; i > 0;) {
                --i;
                const std::int64_t v = stack_top(stack);
                stack_pop(stack);
                locals_set(call_locals, i, v);
            }
            if (try_builtin(n, argc, call_locals, stack, st)) {
                if (st.did_print) {
                    out.did_print = true;
                    out.last_print = st.last_print;
                    out.last_print_str = st.last_print_str;
                }
                break;
            }
            const auto it = mod.func_index.find(n);
            if (it == mod.func_index.end()) {
                HeapObj obj;
                obj.kind = HeapKind::Enum;
                obj.tag_or_fields = dotted_tail(n);
                obj.vals.resize(static_cast<size_t>(argc));
                for (int i = 0; i < argc; ++i) {
                    obj.vals[static_cast<size_t>(i)] = locals_get(call_locals, i);
                }
                stack_push(stack, alloc(st, std::move(obj)));
            } else {
                const Func& fn = mod.funcs[static_cast<size_t>(it->second)];
                RunResult fr = run_code(mod, fn.code, fn.labels, std::move(call_locals), st);
                if (!fr.ok) return fr;
                stack_push(stack, fr.value);
                if (fr.did_print) {
                    out.did_print = true;
                    out.last_print = fr.last_print;
                    out.last_print_str = fr.last_print_str;
                }
            }
            break;
        }
        case Op::MkEnum: {
            const int arity = static_cast<int>(ins.a);
            std::vector<std::int64_t> payloads;
            payloads.reserve(static_cast<size_t>(arity));
            for (int i = 0; i < arity; ++i) {
                payloads.push_back(stack_top(stack));
                stack_pop(stack);
            }
            // Match Elan Cons-prepend then reverse: index 0 = last popped.
            HeapObj obj;
            obj.kind = HeapKind::Enum;
            obj.tag_or_fields = ins.s;
            obj.vals = std::move(payloads);
            stack_push(stack, alloc(st, std::move(obj)));
            break;
        }
        case Op::MkStruct: {
            HeapObj obj;
            obj.kind = HeapKind::Struct;
            obj.tag_or_fields = ins.s;
            obj.vals.assign(static_cast<size_t>(csv_count(ins.s)), 0);
            stack_push(stack, alloc(st, std::move(obj)));
            break;
        }
        case Op::GetField: {
            const std::int64_t top = stack_top(stack);
            stack_pop(stack);
            std::int64_t val = 0;
            if (HeapObj* obj = heap_get(st, top)) {
                if (obj->kind == HeapKind::Struct) {
                    const int idx = csv_index(obj->tag_or_fields, ins.s);
                    if (idx >= 0 && idx < static_cast<int>(obj->vals.size())) {
                        val = obj->vals[static_cast<size_t>(idx)];
                    }
                }
            }
            stack_push(stack, val);
            break;
        }
        case Op::SetField: {
            const std::int64_t value = stack_top(stack);
            stack_pop(stack);
            const std::int64_t handle = stack_top(stack);
            stack_pop(stack);
            if (HeapObj* obj = heap_get(st, handle)) {
                if (obj->kind == HeapKind::Struct) {
                    const int fi = csv_index(obj->tag_or_fields, ins.s);
                    if (fi >= 0) {
                        if (fi >= static_cast<int>(obj->vals.size())) {
                            obj->vals.resize(static_cast<size_t>(fi + 1), 0);
                        }
                        obj->vals[static_cast<size_t>(fi)] = value;
                    }
                }
            }
            break;
        }
        case Op::JumpIfNotVariant: {
            const std::int64_t top = stack_top(stack);
            bool matched = false;
            if (HeapObj* obj = heap_get(st, top)) {
                if (obj->kind == HeapKind::Enum && obj->tag_or_fields == ins.s) {
                    matched = true;
                }
            }
            if (!matched) jump_to(static_cast<int>(ins.a));
            break;
        }
        case Op::GetPayload: {
            const std::int64_t top = stack_top(stack);
            std::int64_t payload = 0;
            if (HeapObj* obj = heap_get(st, top)) {
                if (obj->kind == HeapKind::Enum) {
                    const int idx = static_cast<int>(ins.a);
                    if (idx >= 0 && idx < static_cast<int>(obj->vals.size())) {
                        payload = obj->vals[static_cast<size_t>(idx)];
                    }
                }
            }
            stack_push(stack, payload);
            break;
        }
        case Op::Return:
            out.value = stack_top(stack);
            out.did_print = st.did_print;
            out.last_print = st.last_print;
            out.last_print_str = st.last_print_str;
            return out;
        }
    }
    out.ok = false;
    out.error = "guard tripped";
    return out;
}

} // namespace

RunResult run_text(const std::string& text) {
    Module mod;
    std::string err;
    RunResult out;
    if (!parse_module(text, mod, err)) {
        out.ok = false;
        out.error = err;
        return out;
    }
    VmState st;
    out = run_code(mod, mod.entry, mod.entry_labels, {}, st);
    if (out.ok) {
        out.did_print = st.did_print;
        out.last_print = st.last_print;
        out.last_print_str = st.last_print_str;
    }
    return out;
}

RunResult run_file(const std::string& path) {
    std::string err;
    const std::string text = read_file_all(path, err);
    if (!err.empty()) {
        RunResult out;
        out.ok = false;
        out.error = err;
        return out;
    }
    return run_text(text);
}

std::string encode_result(const RunResult& r) {
    if (!r.ok) {
        return std::string("0|") + r.error;
    }
    std::ostringstream ss;
    ss << "1|" << (r.did_print ? 1 : 0) << '|' << r.last_print << '|' << r.last_print_str;
    return ss.str();
}

} // namespace mini_ir
} // namespace erelang
