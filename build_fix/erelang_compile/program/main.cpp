#include <string>
    #include <vector>
    #include <unordered_set>
    #include <unordered_map>
    #include <filesystem>
    #include <iostream>
    #include <cstdlib>
    #include <sstream>
    #include <fstream>
    #include <functional>
    #include <algorithm>
    #include "erelang/lexer.hpp"
    #include "erelang/parser.hpp"
    #include "erelang/runtime.hpp"
    #include "erelang/typechecker.hpp"
    #include "erelang/optimizer.hpp"
    #include "erelang/symboltable.hpp"
    #include "erelang/modules.hpp"
#ifdef _WIN32
    #include <windows.h>
#endif

    namespace fs = std::filesystem;

    int main(int argc, char** argv) {
        try {
            // Embedded files collected at compile time
            static const std::unordered_map<std::string, std::string> kFiles = {
    {"D:/Development/Game/Physics/examples/program.elan", R"OBX(
@erelang


public action main {

    Array<int> numbers = [1,2,3,4,5,6,7,8];

    for (n : numbers) {
        print "{n}";
    }

}


run main;

)OBX"},
        };

        auto load_file = [&](const std::string& abspath)->std::string {
            if (abspath.rfind("mod://", 0) == 0) {
                std::string name = abspath.substr(6);
                const auto mods = erelang::get_registered_modules();
                for (const auto& m : mods) {
                    for (size_t i=0; i<m.file_count; ++i) {
                        const erelang::ModuleFile& f = m.files[i];
                        if (f.name && name == f.name) return std::string(f.contents ? f.contents : "");
                    }
                }
                throw std::runtime_error(std::string("Module file not found: ") + name);
            }
            auto it = kFiles.find(abspath);
            if (it != kFiles.end()) return it->second;
            std::ifstream in(abspath, std::ios::binary);
            if (!in) throw std::runtime_error(std::string("File not found: ") + abspath);
            std::ostringstream ss; ss << in.rdbuf();
            return ss.str();
        };

        auto resolve_import = [&](const std::string& basePath, const std::string& imp)->std::optional<std::string> {
            fs::path ap = fs::absolute(basePath);
            fs::path ip = imp;
            // Build candidate extensions: .0bs and .obsecret when none provided
            std::vector<fs::path> candidates;
            if (!ip.has_extension()) {
                fs::path c1 = ip; c1.replace_extension(".0bs");
                fs::path c2 = ip; c2.replace_extension(".obsecret");
                candidates = {c1, c2};
            } else {
                candidates = {ip};
            }
            auto ends_with = [](const std::string& s, const std::string& suf){ return s.size()>=suf.size() && s.rfind(suf)==s.size()-suf.size(); };
            if (ip.is_relative()) {
                // Check embedded map and disk for each candidate
                for (auto& c : candidates) {
                    fs::path tryLocal = ap.parent_path() / c;
                    auto it = kFiles.find(tryLocal.string());
                    if (it != kFiles.end()) return it->first;
                    if (fs::exists(tryLocal)) return tryLocal.string();
                }
                // Search any embedded file that ends with requested relative path for either candidate
                for (auto& c : candidates) {
                    std::string tail = std::string("/") + c.generic_string();
                    for (const auto& kv : kFiles) { if (ends_with(kv.first, tail)) return kv.first; }
                }
                // Check registered modules (.olib/.odll) for either candidate
                const auto mods = erelang::get_registered_modules();
                for (const auto& m : mods) {
                    for (size_t i=0; i<m.file_count; ++i) {
                        const erelang::ModuleFile& f = m.files[i];
                        if (!f.name) continue;
                        std::string fn = f.name; for (auto& ch : fn) if (ch == '\\') ch = '/';
                        for (auto& c : candidates) {
                            std::string fntail = std::string("/") + c.generic_string();
                            if (ends_with(fn, fntail)) return std::string("mod://") + f.name;
                        }
                    }
                }
                return std::nullopt;
            }
            // Absolute path case
            for (auto& c : candidates) { fs::path abs = fs::absolute(c); if (fs::exists(abs)) return abs.string(); }
            return std::nullopt;
        };

        using namespace erelang;
        std::unordered_set<std::string> visited;
        std::vector<Program> ordered;

        std::function<void(const std::string&)> load_prog = [&](const std::string& file){
            fs::path p = fs::absolute(file);
            std::string key = p.string();
            if (visited.count(key)) return;
            visited.insert(key);
            bool fromEmbedded = kFiles.find(key) != kFiles.end();
            std::string source = load_file(key);
            // Preprocess #include directives (simple scan); treat as dependency first
            {
                std::istringstream iss(source); std::string line;
                while (std::getline(iss, line)) {
                    std::string t = line; while (!t.empty() && (t.back()=='\r'||t.back()=='\n')) t.pop_back();
                    if (t.rfind("#include", 0) == 0) {
                        size_t pos = t.find_first_not_of(" \t", 8);
                        if (pos != std::string::npos) {
                            char open = t[pos];
                            if (open=='"' || open=='<') {
                                char close = (open=='"') ? '"' : '>';
                                size_t end = t.find(close, pos+1);
                                if (end != std::string::npos) {
                                    std::string inc = t.substr(pos+1, end-(pos+1));
                                    for (auto& ch : inc) if (ch=='\\') ch='/';
                                    fs::path ip = inc; if (ip.is_relative()) ip = p.parent_path()/ip; load_prog(ip.string());
                                }
                            } else {
                                std::string inc = t.substr(pos); for (auto& ch : inc) if (ch=='\\') ch='/'; fs::path ip = inc; if (ip.is_relative()) ip = p.parent_path()/ip; load_prog(ip.string());
                            }
                        }
                    }
                }
            }
            erelang::LexerOptions lxopts; lxopts.enableDurations = true; lxopts.enableUnits = true; lxopts.enablePolyIdentifiers = true; lxopts.emitDocComments = true; lxopts.emitComments = false;
            Lexer lx(std::move(source), lxopts);
            auto tokens = lx.lex();
            Parser ps(std::move(tokens));
            Program prog = ps.parse();
            for (auto& a : prog.actions) a.sourcePath = key;
            for (auto& h : prog.hooks) h.sourcePath = key;
            for (auto& e : prog.entities) e.sourcePath = key;
            for (auto& g : prog.globals) g.sourcePath = key;
            for (const auto& imp : prog.imports) {
                if (imp.pluginGlob) continue;
                auto next = resolve_import(key, imp.path);
                if (next) load_prog(*next);
            }
            static const bool verbose = [](){ const char* env = std::getenv("ERELANG_EMBED_VERBOSE"); return env && *env; }();
            if (verbose) {
                std::string label = fromEmbedded ? std::string("(embedded) ") + fs::path(key).filename().string() : key;
                std::cerr << "[erelang-loader] loaded " << label << " actions=" << prog.actions.size() << " entities=" << prog.entities.size() << " imports=" << prog.imports.size() << "\n";
            }
            ordered.push_back(std::move(prog));
        };

    // Load dynamic modules shipped alongside the app (.odll)
    {
#ifdef _WIN32
        wchar_t buf[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
        fs::path exeP = fs::path(std::wstring(buf, n));
        erelang::load_dynamic_modules_in_dir(exeP.parent_path());
#endif
    }

        // Load embedded debug driver; optionally also load argv[1] target
        const std::string embedded = "D:/Development/Game/Physics/examples/program.elan";
        if (argc > 1) {
            load_prog(embedded);
            std::string target = fs::absolute(argv[1]).generic_string();
            load_prog(target);
        } else {
            load_prog(embedded);
        }

        Program merged;
        for (size_t i=0; i+1<ordered.size(); ++i) {
            const Program& m = ordered[i];
            merged.actions.insert(merged.actions.end(), m.actions.begin(), m.actions.end());
            merged.hooks.insert(merged.hooks.end(), m.hooks.begin(), m.hooks.end());
            merged.entities.insert(merged.entities.end(), m.entities.begin(), m.entities.end());
        }
        Program mainProg = ordered.back();
        if (!merged.actions.empty()) mainProg.actions.insert(mainProg.actions.begin(), merged.actions.begin(), merged.actions.end());
        if (!merged.hooks.empty()) mainProg.hooks.insert(mainProg.hooks.begin(), merged.hooks.begin(), merged.hooks.end());
        if (!merged.entities.empty()) mainProg.entities.insert(mainProg.entities.begin(), merged.entities.begin(), merged.entities.end());

        auto append_aliases = [&](const Program& src) {
            for (const auto& alias : src.pluginAliases) {
                if (std::find(mainProg.pluginAliases.begin(), mainProg.pluginAliases.end(), alias) == mainProg.pluginAliases.end()) {
                    mainProg.pluginAliases.push_back(alias);
                }
            }
        };
        for (const auto& progSrc : ordered) append_aliases(progSrc);

        // Prefer debug_main if present
        bool hasDebugMain = false;
        for (const auto& a : mainProg.actions) if (a.name == "debug_main") { hasDebugMain = true; break; }
        if (hasDebugMain) mainProg.runTarget = std::string("debug_main");
        if (!mainProg.runTarget) {
            for (const auto& a : mainProg.actions) if (a.name == "main") { mainProg.runTarget = std::string("main"); break; }
        }
        // Semantic pipeline: symbol table -> typecheck -> optimize
        erelang::SymbolTable symtab;
        for (const auto& a : mainProg.actions) symtab.add(a.name, "action");
        for (const auto& e : mainProg.entities) symtab.add(e.name, "entity");
        erelang::TypeChecker tc;
        auto tcRes = tc.check(mainProg);
        if (!tcRes.ok) {
            for (auto& d : tcRes.diagnostics) {
                const char* tag = d.severity == erelang::Severity::Warning ? "[warn] " : (d.severity == erelang::Severity::Note ? "[note] " : "[error] ");
                std::cerr << tag << d.code << ": " << d.message;
                if (!d.context.empty()) std::cerr << " (" << d.context << ")";
                std::cerr << "\n";
            }
            return 1;
        }
        auto optRes = erelang::optimize_program(mainProg);
        (void)optRes; // currently unused detail
    Runtime rt;
        // Pass CLI args through to runtime for args_count/args_get built-ins
        std::vector<std::string> cliArgs;
        for (int i = 1; i < argc; ++i) cliArgs.emplace_back(argv[i]);
        Runtime::set_cli_args(cliArgs);
        return rt.run(mainProg);
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 1;
    }
}
