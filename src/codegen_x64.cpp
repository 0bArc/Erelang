#include "erelang/codegen_x64.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace erelang {
namespace {

std::string sanitize_label(std::string name) {
    for (char& ch : name) {
        const bool ok = (ch >= 'a' && ch <= 'z') ||
                        (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') ||
                        ch == '_';
        if (!ok) ch = '_';
    }
    if (name.empty()) name = "anon";
    return name;
}

bool is_symbol_operand(std::string_view op) {
    return !op.empty() && (op.front() == '$' || op.front() == '%');
}

bool is_int_imm(std::string_view op) {
    if (op.empty() || op.front() != '#') return false;
    std::size_t i = 1;
    if (i < op.size() && (op[i] == '+' || op[i] == '-')) ++i;
    if (i >= op.size()) return false;
    for (; i < op.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(op[i])) == 0) return false;
    }
    return true;
}

bool is_string_imm(std::string_view op) {
    return op.size() >= 2 && op.front() == '"' && op.back() == '"';
}

std::string unquote(std::string_view text) {
    if (!is_string_imm(text)) return std::string(text);
    std::string out;
    out.reserve(text.size() - 2);
    for (std::size_t i = 1; i + 1 < text.size(); ++i) {
        char ch = text[i];
        if (ch == '\\' && i + 2 < text.size()) {
            char esc = text[++i];
            switch (esc) {
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case '\\': out.push_back('\\'); break;
                case '"': out.push_back('"'); break;
                default: out.push_back(esc); break;
            }
            continue;
        }
        out.push_back(ch);
    }
    return out;
}

std::string gas_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char ch : text) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

struct FunctionAsmContext {
    std::unordered_map<std::string, int> slotOffset; // symbol -> positive offset from rbp
    std::unordered_map<std::string, std::string> stringLabel; // literal -> label
    int& nextGlobalStringId;   // shared across functions so .LC labels never collide
    std::string fnPrefix;      // per-function label prefix (IR labels are per-function)
    int seq = 0;               // per-function counter for scratch labels

    FunctionAsmContext(int& nextStringId, std::string prefix)
        : nextGlobalStringId(nextStringId), fnPrefix(std::move(prefix)) {}

    // Unique per-function scratch label (e.g. div-guard branches).
    std::string fresh(const std::string& hint) {
        return fnPrefix + sanitize_label(hint) + "_" + std::to_string(seq++);
    }

    int ensure_slot(const std::string& sym) {
        auto it = slotOffset.find(sym);
        if (it != slotOffset.end()) return it->second;
        const int off = static_cast<int>(slotOffset.size() + 1) * 8;
        slotOffset.emplace(sym, off);
        return off;
    }

    std::string ensure_string_label(const std::string& literal) {
        auto it = stringLabel.find(literal);
        if (it != stringLabel.end()) return it->second;
        const std::string label = ".LC" + std::to_string(nextGlobalStringId++);
        stringLabel.emplace(literal, label);
        return label;
    }

    // Prefix an IR branch label with this function's name so the same IR label
    // (e.g. while_start_0) emitted by two functions does not collide.
    std::string local_label(const std::string& name) const {
        return fnPrefix + sanitize_label(name);
    }
};

void collect_slots(FunctionAsmContext& ctx, const IRFunction& fn) {
    for (const auto& p : fn.params) {
        ctx.ensure_slot("$" + p);
    }
    for (const auto& ins : fn.instructions) {
        for (const auto& op : ins.operands) {
            if (is_symbol_operand(op)) ctx.ensure_slot(op);
            if (is_string_imm(op)) {
                ctx.ensure_string_label(unquote(op));
            }
        }
    }
}

void emit_load_operand(std::ostringstream& out, FunctionAsmContext& ctx, const std::string& reg, const std::string& operand) {
    if (is_symbol_operand(operand)) {
        const int off = ctx.ensure_slot(operand);
        out << "    mov " << reg << ", QWORD PTR [rbp-" << off << "]\n";
        return;
    }
    if (is_int_imm(operand)) {
        // Canonicalize to decimal: GAS would otherwise interpret a leading
        // zero (e.g. #077) as octal. The parser already resolves these with
        // base-0 semantics, so decimal output preserves the language value.
        const std::string digits(operand.substr(1));
        std::string canonical;
        try {
            std::size_t pos = 0;
            const long long v = std::stoll(digits, &pos, 0);
            canonical = (pos == digits.size()) ? std::to_string(v) : digits;
        } catch (...) {
            canonical = digits;
        }
        out << "    mov " << reg << ", " << canonical << "\n";
        return;
    }
    if (is_string_imm(operand)) {
        const std::string label = ctx.ensure_string_label(unquote(operand));
        out << "    lea " << reg << ", " << label << "[rip]\n";
        return;
    }
    out << "    mov " << reg << ", 0\n";
}

void emit_store_symbol(std::ostringstream& out, FunctionAsmContext& ctx, const std::string& symbol, const std::string& reg) {
    const int off = ctx.ensure_slot(symbol);
    out << "    mov QWORD PTR [rbp-" << off << "], " << reg << "\n";
}

int aligned_frame_size(const FunctionAsmContext& ctx) {
    const int raw = static_cast<int>(ctx.slotOffset.size()) * 8 + 32; // include shadow space
    const int aligned = (raw + 15) & ~15;
    return std::max(aligned, 32);
}

void emit_binary_arith(std::ostringstream& out, FunctionAsmContext& ctx, const IRInstruction& ins, const std::string& op) {
    if (ins.operands.size() < 3) return;
    const auto& dst = ins.operands[0];
    emit_load_operand(out, ctx, "rax", ins.operands[1]);
    emit_load_operand(out, ctx, "r10", ins.operands[2]);
    if (op == "add") out << "    add rax, r10\n";
    if (op == "sub") out << "    sub rax, r10\n";
    if (op == "mul") out << "    imul rax, r10\n";
    emit_store_symbol(out, ctx, dst, "rax");
}

void emit_compare(std::ostringstream& out, FunctionAsmContext& ctx, const IRInstruction& ins, const std::string& cc) {
    if (ins.operands.size() < 3) return;
    const auto& dst = ins.operands[0];
    emit_load_operand(out, ctx, "rax", ins.operands[1]);
    emit_load_operand(out, ctx, "r10", ins.operands[2]);
    out << "    cmp rax, r10\n";
    out << "    mov rax, 0\n";
    out << "    set" << cc << " al\n";
    emit_store_symbol(out, ctx, dst, "rax");
}

int call_reserve_size(std::size_t argCount) {
    const std::size_t stackArgCount = argCount > 4 ? (argCount - 4) : 0;
    int reserve = static_cast<int>(32 + stackArgCount * 8);
    if ((reserve & 15) != 0) reserve += 8;
    return reserve;
}

void emit_call_with_operands(std::ostringstream& out,
                             FunctionAsmContext& ctx,
                             const std::string& targetLabel,
                             const std::vector<std::string>& args) {
    const int reserve = call_reserve_size(args.size());
    out << "    sub rsp, " << reserve << "\n";

    for (std::size_t i = 4; i < args.size(); ++i) {
        emit_load_operand(out, ctx, "rax", args[i]);
        const int off = 32 + static_cast<int>((i - 4) * 8);
        out << "    mov QWORD PTR [rsp+" << off << "], rax\n";
    }

    const char* argRegs[4] = {"rcx", "rdx", "r8", "r9"};
    for (std::size_t i = 0; i < args.size() && i < 4; ++i) {
        emit_load_operand(out, ctx, argRegs[i], args[i]);
    }

    out << "    call " << targetLabel << "\n";
    out << "    add rsp, " << reserve << "\n";
}

} // namespace

std::string X64Codegen::emit_gas_win64_demo(const IRModule& module) const {
    std::ostringstream out;
    out << ".intel_syntax noprefix\n";
    out << ".text\n";
    out << ".globl main\n";
    out << ".extern puts\n";
    out << ".extern printf\n";
    out << ".extern scanf\n";
    out << ".extern Sleep\n";
    out << ".extern getchar\n";
    out << ".extern strcmp\n\n";

    std::unordered_map<std::string, std::string> fnLabel;
    for (const auto& fn : module.functions) {
        fnLabel[fn.name] = "erelang_fn_" + sanitize_label(fn.name);
    }

    const std::string entryName = module.entry.value_or(module.functions.empty() ? std::string("main") : module.functions.front().name);
    const std::string entryLabel = fnLabel.count(entryName) ? fnLabel[entryName] : "erelang_fn_main";

    out << "main:\n";
    out << "    push rbp\n";
    out << "    mov rbp, rsp\n";
    out << "    sub rsp, 32\n";
    out << "    call " << entryLabel << "\n";
    out << "    add rsp, 32\n";
    out << "    pop rbp\n";
    out << "    ret\n\n";

    std::string dataSection;
    std::unordered_map<std::string, std::string> globalStringLabels;    // text -> label (dedup for format strings)
    std::unordered_map<std::string, std::string> emittedStringData;     // label -> text (one entry per emitted label)

    auto ensure_global_string = [&](const std::string& text) {
        auto it = globalStringLabels.find(text);
        if (it != globalStringLabels.end()) return it->second;
        const std::string label = ".LG" + std::to_string(globalStringLabels.size());
        globalStringLabels[text] = label;
        emittedStringData[label] = text;
        return label;
    };

    const std::string intFmtLabel = ensure_global_string("%lld\\n");
    const std::string inputFmtLabel = ensure_global_string("%1023s");
    const std::string inputBufLabel = ".L_INPUT_BUF";
    const std::string divByZeroMsgLabel = ensure_global_string("runtime error: division by zero or overflow\\n");

    // Shared across functions so .LC string labels never collide.
    int globalStringId = 0;

    for (const auto& fn : module.functions) {
        FunctionAsmContext ctx(globalStringId, "erelang_fn_" + sanitize_label(fn.name) + "_");
        collect_slots(ctx, fn);
        const int frame = aligned_frame_size(ctx);
        const std::string fnEnd = fnLabel[fn.name] + "_end";

        out << fnLabel[fn.name] << ":\n";
        out << "    push rbp\n";
        out << "    mov rbp, rsp\n";
        out << "    sub rsp, " << frame << "\n";

        const char* paramRegs[4] = {"rcx", "rdx", "r8", "r9"};
        for (std::size_t i = 0; i < fn.params.size() && i < 4; ++i) {
            emit_store_symbol(out, ctx, "$" + fn.params[i], paramRegs[i]);
        }
        for (std::size_t i = 4; i < fn.params.size(); ++i) {
            const int incomingOff = 48 + static_cast<int>((i - 4) * 8);
            out << "    mov rax, QWORD PTR [rbp+" << incomingOff << "]\n";
            emit_store_symbol(out, ctx, "$" + fn.params[i], "rax");
        }

        for (const auto& ins : fn.instructions) {
            if (ins.opcode == "nop") {
                continue;
            }
            if (ins.opcode == "label") {
                if (!ins.operands.empty()) out << ctx.local_label(ins.operands[0]) << ":\n";
                continue;
            }
            if (ins.opcode == "jmp") {
                if (!ins.operands.empty()) out << "    jmp " << ctx.local_label(ins.operands[0]) << "\n";
                continue;
            }
            if (ins.opcode == "jz") {
                if (ins.operands.size() >= 2) {
                    emit_load_operand(out, ctx, "rax", ins.operands[0]);
                    out << "    cmp rax, 0\n";
                    out << "    je " << ctx.local_label(ins.operands[1]) << "\n";
                }
                continue;
            }
            if (ins.opcode == "jnz") {
                if (ins.operands.size() >= 2) {
                    emit_load_operand(out, ctx, "rax", ins.operands[0]);
                    out << "    cmp rax, 0\n";
                    out << "    jne " << ctx.local_label(ins.operands[1]) << "\n";
                }
                continue;
            }
            if (ins.opcode == "mov") {
                if (ins.operands.size() >= 2) {
                    emit_load_operand(out, ctx, "rax", ins.operands[1]);
                    if (is_symbol_operand(ins.operands[0])) emit_store_symbol(out, ctx, ins.operands[0], "rax");
                }
                continue;
            }
            if (ins.opcode == "add" || ins.opcode == "sub" || ins.opcode == "mul") {
                emit_binary_arith(out, ctx, ins, ins.opcode);
                continue;
            }
            if (ins.opcode == "div" || ins.opcode == "mod") {
                if (ins.operands.size() >= 3) {
                    const auto& dst = ins.operands[0];
                    emit_load_operand(out, ctx, "rax", ins.operands[1]);
                    out << "    cqo\n";
                    emit_load_operand(out, ctx, "r10", ins.operands[2]);
                    // idiv traps on divide-by-zero and INT64_MIN / -1; guard both
                    // so the process reports an error instead of SIGFPE.
                    const std::string okLabel = ctx.fresh("div_ok");
                    const std::string errLabel = ctx.fresh("div_err");
                    const std::string doneLabel = ctx.fresh("div_done");
                    out << "    test r10, r10\n";
                    out << "    jz " << errLabel << "\n";
                    out << "    cmp r10, -1\n";
                    out << "    jne " << okLabel << "\n";
                    out << "    mov r11, 0x8000000000000000\n";
                    out << "    cmp rax, r11\n";
                    out << "    je " << errLabel << "\n";
                    out << okLabel << ":\n";
                    out << "    idiv r10\n";
                    if (ins.opcode == "div") emit_store_symbol(out, ctx, dst, "rax");
                    else emit_store_symbol(out, ctx, dst, "rdx");
                    out << "    jmp " << doneLabel << "\n";
                    out << errLabel << ":\n";
                    out << "    sub rsp, 32\n";
                    out << "    lea rcx, " << divByZeroMsgLabel << "[rip]\n";
                    out << "    call puts\n";
                    out << "    add rsp, 32\n";
                    out << "    mov ecx, 1\n";
                    out << "    call exit\n";
                    out << doneLabel << ":\n";
                }
                continue;
            }
            if (ins.opcode == "neg") {
                if (ins.operands.size() >= 2) {
                    emit_load_operand(out, ctx, "rax", ins.operands[1]);
                    out << "    neg rax\n";
                    emit_store_symbol(out, ctx, ins.operands[0], "rax");
                }
                continue;
            }
            if (ins.opcode == "not") {
                if (ins.operands.size() >= 2) {
                    emit_load_operand(out, ctx, "rax", ins.operands[1]);
                    out << "    cmp rax, 0\n";
                    out << "    mov rax, 0\n";
                    out << "    sete al\n";
                    emit_store_symbol(out, ctx, ins.operands[0], "rax");
                }
                continue;
            }
            if (ins.opcode == "and" || ins.opcode == "or") {
                if (ins.operands.size() >= 3) {
                    const auto& dst = ins.operands[0];
                    emit_load_operand(out, ctx, "rax", ins.operands[1]);
                    emit_load_operand(out, ctx, "r10", ins.operands[2]);
                    if (ins.opcode == "and") out << "    and rax, r10\n";
                    else out << "    or rax, r10\n";
                    out << "    cmp rax, 0\n";
                    out << "    mov rax, 0\n";
                    out << "    setne al\n";
                    emit_store_symbol(out, ctx, dst, "rax");
                }
                continue;
            }
            if (ins.opcode == "cmp_eq") { emit_compare(out, ctx, ins, "e"); continue; }
            if (ins.opcode == "cmp_ne") { emit_compare(out, ctx, ins, "ne"); continue; }
            if (ins.opcode == "cmp_lt") { emit_compare(out, ctx, ins, "l"); continue; }
            if (ins.opcode == "cmp_le") { emit_compare(out, ctx, ins, "le"); continue; }
            if (ins.opcode == "cmp_gt") { emit_compare(out, ctx, ins, "g"); continue; }
            if (ins.opcode == "cmp_ge") { emit_compare(out, ctx, ins, "ge"); continue; }

            if (ins.opcode == "print_s") {
                if (!ins.operands.empty()) {
                    emit_call_with_operands(out, ctx, "puts", {ins.operands[0]});
                }
                continue;
            }
            if (ins.opcode == "print_i") {
                if (!ins.operands.empty()) {
                    out << "    sub rsp, 32\n";
                    out << "    lea rcx, " << intFmtLabel << "[rip]\n";
                    emit_load_operand(out, ctx, "rdx", ins.operands[0]);
                    out << "    call printf\n";
                    out << "    add rsp, 32\n";
                }
                continue;
            }
            if (ins.opcode == "print_fmt") {
                if (!ins.operands.empty()) {
                    emit_call_with_operands(out, ctx, "printf", ins.operands);
                }
                continue;
            }
            if (ins.opcode == "input") {
                if (!ins.operands.empty() && is_symbol_operand(ins.operands[0])) {
                    out << "    sub rsp, 32\n";
                    out << "    lea rcx, " << inputFmtLabel << "[rip]\n";
                    out << "    lea rdx, " << inputBufLabel << "[rip]\n";
                    out << "    call scanf\n";
                    out << "    add rsp, 32\n";
                    out << "    lea rax, " << inputBufLabel << "[rip]\n";
                    emit_store_symbol(out, ctx, ins.operands[0], "rax");
                }
                continue;
            }
            if (ins.opcode == "sleep") {
                out << "    sub rsp, 32\n";
                if (!ins.operands.empty()) {
                    emit_load_operand(out, ctx, "rax", ins.operands[0]);
                    out << "    mov ecx, eax\n";
                } else {
                    out << "    xor ecx, ecx\n";
                }
                out << "    call Sleep\n";
                out << "    add rsp, 32\n";
                continue;
            }
            if (ins.opcode == "pause") {
                out << "    sub rsp, 32\n";
                out << "    call getchar\n";
                out << "    add rsp, 32\n";
                continue;
            }
            if (ins.opcode == "wait_all") {
                continue;
            }
            if (ins.opcode == "fire") {
                continue;
            }

            if (ins.opcode == "call_name") {
                if (!ins.operands.empty()) {
                    const std::string callee = ins.operands[0];
                    auto fit = fnLabel.find(callee);
                    if (fit != fnLabel.end()) {
                        std::vector<std::string> args;
                        for (std::size_t i = 1; i < ins.operands.size(); ++i) {
                            args.push_back(ins.operands[i]);
                        }
                        emit_call_with_operands(out, ctx, fit->second, args);
                    } else {
                        // Builtin calls have no native stub; silently dropping them
                        // would leave a stale rax that the following `call` stores.
                        // Trap instead so the failure is visible.
                        out << "    # unsupported builtin call target: " << callee << "\n";
                        out << "    ud2\n";
                    }
                }
                continue;
            }
            if (ins.opcode == "call") {
                if (!ins.operands.empty() && is_symbol_operand(ins.operands[0])) {
                    emit_store_symbol(out, ctx, ins.operands[0], "rax");
                }
                continue;
            }

            if (ins.opcode == "ret") {
                if (!ins.operands.empty()) emit_load_operand(out, ctx, "rax", ins.operands[0]);
                else out << "    xor eax, eax\n";
                out << "    jmp " << fnEnd << "\n";
                continue;
            }

            out << "    # TODO unsupported IR op: " << ins.opcode << "\n";
        }

        out << fnEnd << ":\n";
        out << "    add rsp, " << frame << "\n";
        out << "    pop rbp\n";
        out << "    ret\n\n";

        for (const auto& kv : ctx.stringLabel) {
            // Every per-function .LC label must be emitted exactly once. Labels
            // are globally unique (shared counter), so keying by label is safe
            // even when two functions reference the same literal text.
            emittedStringData[kv.second] = kv.first;
        }
    }

    out << ".section .rdata,\"dr\"\n";
    std::vector<std::pair<std::string, std::string>> orderedStrings(emittedStringData.begin(), emittedStringData.end());
    std::sort(orderedStrings.begin(), orderedStrings.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    for (const auto& [label, text] : orderedStrings) {
        out << label << ": .asciz \"" << gas_escape(text) << "\"\n";
    }
    out << ".bss\n";
    out << ".align 8\n";
    out << inputBufLabel << ": .space 1024\n";

    return out.str();
}

} // namespace erelang
