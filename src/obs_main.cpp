// erelang CLI: supports static in-process runner (default) and legacy dynamic DLL loader
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <filesystem>
#include <cstdlib>
#include <stdexcept>
#include <functional>
#include <chrono>
#include <array>
#include <optional>
#include <span>
#include <string_view>
#ifdef _WIN32
#include <windows.h>
#endif
// Static runner: include full compiler/runtime stack
#ifdef ERELANG_STATIC_RUNNER
#include "erelang/lexer.hpp"
#include "erelang/parser.hpp"
#include "erelang/runtime.hpp"
#include "erelang/typechecker.hpp"
#include "erelang/optimizer.hpp"
#include "erelang/symboltable.hpp"
#include "erelang/modules.hpp"
#include "erelang/version.hpp"
#include "erelang/policy.hpp"
#include "erelang/plugins.hpp"
#else
// Dynamic path: only C ABI
#include "erelang/cabi.h"
#endif

namespace fs = std::filesystem;

static fs::path resolve_executable_path(const char* argv0) {
#ifdef _WIN32
    wchar_t buffer[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (len > 0) {
        return fs::path(std::wstring(buffer, len));
    }
#endif
    if (argv0 && argv0[0] != '\0') {
        std::error_code ec;
        fs::path candidate = fs::absolute(fs::path(argv0), ec);
        if (!ec && !candidate.empty()) {
            if (fs::exists(candidate)) {
                return candidate;
            }
        }
    }
    return fs::current_path();
}

static fs::path resolve_executable_dir(const char* argv0) {
    const fs::path executablePath = resolve_executable_path(argv0);
    if (fs::is_directory(executablePath)) {
        return executablePath;
    }
    if (!executablePath.empty()) {
        const fs::path parent = executablePath.parent_path();
        if (!parent.empty()) {
            return parent;
        }
    }
    return fs::current_path();
}

#ifdef _WIN32
static bool extract_rcdata_to_file(LPCWSTR name, const fs::path& outPath);
#ifndef RT_RCDATA_NUM
#define RT_RCDATA_NUM 10
#endif
#endif

static void print_banner() {
    std::cout << "erelang / Erelang v" << erelang::BuildInfo::version() << " -- use --help for options" << std::endl;
}

static void print_help() {
    std::cout << "Usage:\n"
                 "  erelang --help\n"
                 "  erelang --about\n"
                 "  erelang --bootstrap  (force show bootstrap manifest UI even if args given)\n"
                 "  erelang <path\\to\\file.(0bs|obsecret)> [--debug]\n"
                 "  erelang --compile <path\\to\\file.(0bs|obsecret)> [--output <path\\to\\out.exe>] [--static|--dynamic]\n"
                 "  erelang --make-debug [--output <path\\to\\debug.exe>]\n"
                 "\n"
                 "Description:\n"
                 "  Run a .0bs directly or compile it to a standalone .exe.\n"
                 "  --compile builds a standalone Windows .exe for the given erelang file.\n"
                 "  Imports are resolved at compile-time and embedded into the executable.\n"
                 "  Each compiled app writes manifest.erelang next to the exe with build + content metadata.\n"
                 "  By default, the compiled app links statically. Use --dynamic to link against erelang.dll (if available) for a tiny stub.\n"
                 "  --make-debug builds a debugger exe from examples/debug/debug.0bs.\n"
                 "  --debug when running loads the debug driver and prefers debug_main.\n";
}

static std::string slurp_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("Failed to open: " + p.string());
    std::ostringstream ss; ss << in.rdbuf();
    return ss.str();
}

// Static-runner helpers
#ifdef ERELANG_STATIC_RUNNER
static std::string read_all_text(const std::string& abspath) {
    std::ifstream in(abspath, std::ios::binary);
    if (!in) throw std::runtime_error(std::string("File not found: ") + abspath);
    std::ostringstream ss; ss << in.rdbuf();
    return ss.str();
}

static std::optional<std::string> resolve_import_local(const std::string& basePath, const std::string& imp,
                                                       const std::unordered_map<std::string,std::string>* embedded = nullptr) {
    fs::path ap = fs::absolute(basePath);
    std::string normalizedImp = imp;
    for (auto& ch : normalizedImp) {
        if (ch == '\\') ch = '/';
    }
    std::string loweredImp = normalizedImp;
    std::transform(loweredImp.begin(), loweredImp.end(), loweredImp.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (loweredImp.rfind("builtin/", 0) == 0 || loweredImp.rfind("builtin:", 0) == 0) {
        return std::nullopt;
    }

    fs::path ip = normalizedImp;
    std::vector<fs::path> candidates;
    if (!ip.has_extension()) {
        fs::path c1 = ip; c1.replace_extension(".0bs");
        fs::path c2 = ip; c2.replace_extension(".obsecret");
        candidates = {c1, c2, ip};
    } else {
        candidates = {ip};
    }
    auto ends_with = [](const std::string& s, const std::string& suf){ return s.size()>=suf.size() && s.rfind(suf)==s.size()-suf.size(); };
    if (ip.is_relative()) {
        std::vector<fs::path> roots;
        roots.push_back(ap.parent_path());
        roots.push_back(fs::current_path());
        roots.push_back(fs::current_path() / "std");
        for (fs::path cursor = ap.parent_path(); !cursor.empty(); cursor = cursor.parent_path()) {
            roots.push_back(cursor / "std");
            if (cursor == cursor.root_path()) break;
        }

        for (const auto& root : roots) {
            for (const auto& c : candidates) {
                fs::path tryLocal = root / c;
                if (embedded) {
                    auto it = embedded->find(tryLocal.string());
                    if (it != embedded->end()) return it->first;
                }
                if (fs::exists(tryLocal)) return tryLocal.string();
            }
        }
        if (embedded) {
            for (auto& c : candidates) {
                std::string tail = std::string("/") + c.generic_string();
                for (const auto& kv : *embedded) { if (ends_with(kv.first, tail)) return kv.first; }
            }
        }
    const auto mods = erelang::get_registered_modules();
        for (const auto& m : mods) {
            for (size_t i=0; i<m.file_count; ++i) {
                const erelang::ModuleFile& f = m.files[i];
                if (!f.name) continue;
                std::string fn = f.name; for (auto& ch : fn) if (ch=='\\') ch='/';
                for (auto& c : candidates) {
                    std::string fntail = std::string("/") + c.generic_string();
                    if (ends_with(fn, fntail)) return std::string("mod://") + f.name;
                }
            }
        }
        return std::nullopt;
    }
    for (auto& c : candidates) { fs::path abs = fs::absolute(c); if (fs::exists(abs)) return abs.string(); }
    return std::nullopt;
}

static std::string load_file_local(const std::string& abspath,
                                   const std::unordered_map<std::string,std::string>* embedded = nullptr) {
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
    if (embedded) {
        auto it = embedded->find(abspath);
        if (it != embedded->end()) return it->second;
    }
    return read_all_text(abspath);
}

static void load_program_recursive(const std::string& file,
                                   std::unordered_set<std::string>& visited,
                                   std::vector<erelang::Program>& ordered) {
    fs::path p = fs::absolute(file);
    std::string key = p.string();
    if (visited.count(key)) return;
    visited.insert(key);
    erelang::LexerOptions lxopts; lxopts.enableDurations = true; lxopts.enableUnits = true; lxopts.enablePolyIdentifiers = true; lxopts.emitDocComments = true; lxopts.emitComments = false;
    erelang::Lexer lx(load_file_local(key), lxopts);
    auto tokens = lx.lex();
    erelang::Parser ps(std::move(tokens));
    erelang::Program prog = ps.parse();
    for (auto& a : prog.actions) a.sourcePath = key;
    for (auto& h : prog.hooks) h.sourcePath = key;
    for (auto& e : prog.entities) e.sourcePath = key;
    for (auto& g : prog.globals) g.sourcePath = key;
    for (const auto& imp : prog.imports) {
        if (imp.pluginGlob) continue;
        auto next = resolve_import_local(key, imp.path);
        if (next) load_program_recursive(*next, visited, ordered);
    }
    ordered.push_back(std::move(prog));
}
#endif // ERELANG_STATIC_RUNNER

#ifdef ERELANG_STATIC_RUNNER
static void load_plugins_into_programs(const std::vector<erelang::PluginManifest>& plugins,
                                       std::unordered_set<std::string>& visited,
                                       std::vector<erelang::Program>& ordered) {
    for (const auto& manifest : plugins) {
        for (const auto& script : manifest.scriptFiles) {
            try {
                load_program_recursive(script.string(), visited, ordered);
            } catch (const std::exception& ex) {
                std::cerr << "[plugins] failed to load " << script.string() << ": " << ex.what() << "\n";
            }
        }
    }
}
#endif

static std::string generate_bootstrap_source(const fs::path& mainFile,
                                             const std::vector<std::pair<fs::path,std::string>>& files) {
    std::ostringstream cpp;
        cpp << R"CPP(#include <string>
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
    )CPP";
    for (const auto& [path, content] : files) {
        std::string p = fs::absolute(path).generic_string();
        cpp << "{" << '"' << p << '"' << ", R\"OBX(\n" << content << "\n)OBX\"}" << ",\n";
    }
    cpp << R"CPP(        };

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
        const std::string embedded = )CPP";
    cpp << '"' << fs::absolute(mainFile).generic_string() << '"';
    cpp << R"CPP(;
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
)CPP";
    return cpp.str();
}

struct ScopedCurrentPath {
    explicit ScopedCurrentPath(const fs::path& newPath)
        : previous(fs::current_path()) {
        fs::current_path(newPath);
    }

    ScopedCurrentPath(const ScopedCurrentPath&) = delete;
    ScopedCurrentPath& operator=(const ScopedCurrentPath&) = delete;

    ~ScopedCurrentPath() {
        try {
            fs::current_path(previous);
        } catch (...) {
            // Best-effort restore; ignore failures.
        }
    }

private:
    fs::path previous;
};

static int run_command(const std::string& cmd, const fs::path& workdir) {
    if (workdir.empty()) {
        return -1;
    }
#ifdef _WIN32
    const std::string full = "cmd /c \"" + cmd + "\"";
#else
    const std::string full = cmd;
#endif
    ScopedCurrentPath guard(workdir);
    return std::system(full.c_str());
}

#ifdef _WIN32
static bool extract_rcdata_to_file(LPCWSTR name, const fs::path& outPath) {
    HMODULE hMod = GetModuleHandleW(nullptr);
    if (!hMod) return false;
    HRSRC hRes = FindResourceW(hMod, name, MAKEINTRESOURCEW(RT_RCDATA_NUM));
    if (!hRes) return false;
    HGLOBAL hData = LoadResource(hMod, hRes);
    if (!hData) return false;
    DWORD sz = SizeofResource(hMod, hRes);
    void* p = LockResource(hData);
    if (!p || sz == 0) return false;
    std::error_code ec;
    fs::create_directories(outPath.parent_path(), ec);
    std::ofstream o(outPath, std::ios::binary);
    if (!o) return false;
    o.write(static_cast<const char*>(p), static_cast<std::streamsize>(sz));
    return static_cast<bool>(o);
}
#endif

// Tiny loader for erelang.dll (legacy dynamic mode)
#ifndef ERELANG_STATIC_RUNNER
struct ObDll {
    using PFN_run = int(*)(const char*, int, const char*[], int, char**);
    using PFN_collect = int(*)(const char*, void(*)(const char*, const char*, void*), void*, char**);
    using PFN_free = void(*)(char*);
#ifdef _WIN32
    HMODULE mod = nullptr;
#endif
    PFN_run run = nullptr;
    PFN_collect collect = nullptr;
    PFN_free freeStr = nullptr;

    bool load(const fs::path& dir) {
#ifdef _WIN32
        fs::path cand1 = dir / "erelang.dll";
        fs::path cand2 = dir / "liberelang.dll"; // MinGW naming
        fs::path path = fs::exists(cand1) ? cand1 : (fs::exists(cand2) ? cand2 : fs::path());
        if (path.empty()) return false;
        mod = LoadLibraryW(std::wstring(path.wstring()).c_str());
        if (!mod) return false;
        run = reinterpret_cast<PFN_run>(GetProcAddress(mod, "ob_run_file"));
        collect = reinterpret_cast<PFN_collect>(GetProcAddress(mod, "ob_collect_files"));
        freeStr = reinterpret_cast<PFN_free>(GetProcAddress(mod, "ob_free_string"));
        return run && collect && freeStr;
#else
        (void)dir; return false;
#endif
    }
} g_ob;
#endif

namespace erelang::cli {

enum class CommandKind {
    Banner,
    Help,
    Version,
    ListBuiltins,
    About,
    Bootstrap,
    Run,
    Compile,
    MakeDebug
};

struct BootstrapOptions {
    bool triggeredByAbout{false};
    float uiScale{0.0f};
    bool vsync{true};
};

struct RunOptions {
    fs::path script{};
    bool debug{false};
    std::vector<std::string> scriptArgs{};
};

struct CompileOptions {
    fs::path input{};
    std::optional<fs::path> output{};
    bool preferStatic{false};
    bool preferDynamic{false};
};

struct MakeDebugOptions {
    std::optional<fs::path> output{};
};

struct Command {
    CommandKind kind{CommandKind::Banner};
    BootstrapOptions bootstrap{};
    RunOptions run{};
    CompileOptions compile{};
    MakeDebugOptions makeDebug{};
};

[[nodiscard]] Command parse(std::span<const std::string_view> args) {
    Command cmd;

    if (args.empty()) {
        return cmd;
    }

    bool bootstrapRequested = false;
    bool aboutRequested = false;
    float forcedUiScale = 0.0f;
    bool defaultVsync = true;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto token = args[i];
        if (token == "--bootstrap") {
            bootstrapRequested = true;
        } else if (token == "--about") {
            aboutRequested = true;
        } else if (token == "--ui-scale" && i + 1 < args.size()) {
            ++i;
            try {
                forcedUiScale = std::max(0.5f, std::stof(std::string{args[i]}));
            } catch (...) {
                forcedUiScale = 0.0f;
            }
        } else if (token == "--no-vsync") {
            defaultVsync = false;
        }
    }

    cmd.bootstrap.uiScale = forcedUiScale;
    cmd.bootstrap.vsync = defaultVsync;
    cmd.bootstrap.triggeredByAbout = aboutRequested;

    if (bootstrapRequested) {
        cmd.kind = CommandKind::Bootstrap;
        return cmd;
    }

    const auto& first = args.front();
    if ((first == "--help" || first == "-h") && args.size() == 1) {
        cmd.kind = CommandKind::Help;
        return cmd;
    }
    if (first == "--version" && args.size() == 1) {
        cmd.kind = CommandKind::Version;
        return cmd;
    }
    if (first == "--list-builtins" && args.size() == 1) {
        cmd.kind = CommandKind::ListBuiltins;
        return cmd;
    }
    if (first == "--about" && args.size() == 1) {
        cmd.kind = CommandKind::About;
        return cmd;
    }
    if (first == "--compile" && args.size() >= 2) {
        cmd.kind = CommandKind::Compile;
        cmd.compile.input = fs::path(std::string{args[1]});
        for (std::size_t i = 2; i < args.size(); ++i) {
            const auto token = args[i];
            if (token == "--output" && i + 1 < args.size()) {
                cmd.compile.output = fs::path(std::string{args[i + 1]});
                ++i;
            } else if (token == "--static") {
                cmd.compile.preferStatic = true;
            } else if (token == "--dynamic") {
                cmd.compile.preferDynamic = true;
            }
        }
        if (!cmd.compile.preferStatic && !cmd.compile.preferDynamic) {
            cmd.compile.preferStatic = true;
        }
        return cmd;
    }
    if (first == "--make-debug") {
        cmd.kind = CommandKind::MakeDebug;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--output" && i + 1 < args.size()) {
                cmd.makeDebug.output = fs::path(std::string{args[i + 1]});
                ++i;
            }
        }
        return cmd;
    }

    if (!first.empty() && first.front() != '-') {
        cmd.kind = CommandKind::Run;
        cmd.run.script = fs::path(std::string{first});
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--debug") {
                cmd.run.debug = true;
            } else {
                cmd.run.scriptArgs.emplace_back(std::string{args[i]});
            }
        }
        return cmd;
    }

    if (first == "--help" || first == "-h") {
        cmd.kind = CommandKind::Help;
    }

    return cmd;
}

} // namespace erelang::cli

namespace erelang {

struct BuildLayout {
    fs::path exeDir;
    fs::path binDir;
    fs::path buildRoot;
    fs::path projectRoot;
    std::string config;
};

[[nodiscard]] BuildLayout detect_build_layout(const fs::path& exePath) {
    BuildLayout layout{};
    layout.exeDir = exePath.parent_path();
    layout.binDir = layout.exeDir;
    const std::string tail = layout.exeDir.filename().string();
    const std::array<std::string, 4> knownConfigs{ "Debug", "Release", "RelWithDebInfo", "MinSizeRel" };
    if (std::find(knownConfigs.begin(), knownConfigs.end(), tail) != knownConfigs.end()) {
        layout.config = tail;
        layout.binDir = layout.exeDir.parent_path();
    }

    fs::path probe = layout.exeDir;
    for (int depth = 0; depth < 6 && !probe.empty(); ++depth) {
        if (fs::exists(probe / "CMakeCache.txt")) {
            layout.buildRoot = probe;
            break;
        }
        probe = probe.parent_path();
    }
    if (layout.buildRoot.empty()) {
        layout.buildRoot = layout.binDir;
    }

    if (!layout.buildRoot.empty()) {
        layout.projectRoot = layout.buildRoot.parent_path();
    }
    if (layout.projectRoot.empty()) {
        layout.projectRoot = layout.buildRoot;
    }
    return layout;
}

struct ExecutionContext {
    fs::path executablePath;
    fs::path executableDir;
    std::vector<erelang::PluginManifest> pluginManifests;
#ifdef ERELANG_STATIC_RUNNER
    std::vector<erelang::Runtime::PluginRecord> runtimePlugins;
#endif
};

[[nodiscard]] ExecutionContext build_execution_context(const char* argv0) {
    ExecutionContext ctx;
    ctx.executablePath = resolve_executable_path(argv0);
    ctx.executableDir = resolve_executable_dir(argv0);
    ctx.pluginManifests = erelang::discover_plugins(ctx.executableDir, &std::cerr);
#ifdef ERELANG_STATIC_RUNNER
    ctx.runtimePlugins.reserve(ctx.pluginManifests.size());
    for (const auto& manifest : ctx.pluginManifests) {
        erelang::Runtime::PluginRecord record;
        record.id = manifest.id;
        record.slug = manifest.baseDirectory.filename().string();
        record.name = manifest.name;
        record.version = manifest.version;
        record.author = manifest.author;
        record.target = manifest.target;
        record.description = manifest.description;
        record.dependencies = manifest.dependencies;
        record.baseDirectory = manifest.baseDirectory;
        record.manifestPath = manifest.manifestPath;
        record.coreProperties = manifest.coreProperties;
        record.hookBindings = manifest.hookBindings;
    record.dslSpec = manifest.dslSpec;
        record.onLoad = manifest.onLoadAction;
        record.onUnload = manifest.onUnloadAction;
        record.dataHook = manifest.dataHookAction;
        ctx.runtimePlugins.push_back(std::move(record));
    }
#endif
    return ctx;
}

[[nodiscard]] int handle_banner() {
    print_banner();
    return 0;
}

[[nodiscard]] int handle_help() {
    print_help();
    return 0;
}

[[nodiscard]] int handle_version() {
    print_banner();
    return 0;
}

[[nodiscard]] int handle_list_builtins() {
    constexpr std::array builtins{
        "now_ms","now_iso","env","username","computer_name","machine_guid","uuid","rand_int","hwid","args_count","args_get",
        "read_text","write_text","append_text","file_exists","mkdirs","copy_file","move_file","delete_file","list_files","cwd","chdir",
        "path_join","path_dirname","path_basename","path_ext",
        "list_new","list_push","list_get","list_len","list_join","list_clear","list_remove_at",
        "dict_new","dict_set","dict_get","dict_has","dict_keys","dict_values",
    "http_get","http_download","hls_download_best","url_encode",
    "network.ip.flush","network.ip.release","network.ip.renew","network.ip.registerdns",
    "win_window_create","win_button_create","win_checkbox_create","win_radiobutton_create","win_slider_create","win_textbox_create","win_label_create","win_on","win_show","win_loop","win_get_text","win_set_text","win_get_check","win_set_check","win_get_slider","win_set_slider","win_close","win_auto_scale","win_set_scale","win_message_box",
    "ui_window_create","ui_label","ui_button","ui_checkbox","ui_radio","ui_slider","ui_textbox","ui_same_line","ui_newline","ui_spacer","ui_separator","ui_load",
        "data_new","data_set","data_get","data_has","data_keys","data_save","data_load",
        "hash_fnv1a","random_bytes","regex_match","regex_find","regex_replace","perm_grant","perm_revoke","perm_has","perm_list",
        "bin_new","bin_from_hex","bin_to_hex","bin_len","bin_get","bin_set","bin_fill","bin_slice",
        "thread_run","thread_join","thread_done","collatz_len","collatz_sweep","collatz_best_steps","collatz_avg_steps",
        "dev_meta","audit","advance_time"
    };

    for (const auto* builtin : builtins) {
        std::cout << builtin << '\n';
    }
    return 0;
}

[[nodiscard]] int handle_bootstrap(const cli::BootstrapOptions& options, const ExecutionContext& ctx) {
    (void)options;
    const fs::path exePath = ctx.executablePath.empty() ? fs::absolute(fs::path("erelang.exe")) : fs::absolute(ctx.executablePath);
    const fs::path root = exePath.parent_path().parent_path();
    const fs::path manifest = root / "src" / "info" / "planned.obmanifest";

    struct ManifestData {
        std::string name = "erelang";
        std::string author;
        std::string version;
        std::string website;
        std::string description;
        std::vector<std::string> features;
        std::vector<std::string> roadmap;
    } data;

    if (fs::exists(manifest)) {
        try {
            std::istringstream iss(slurp_file(manifest));
            std::string line;
            const auto trim = [](std::string value) {
                const auto first = value.find_first_not_of(" \t\r\n");
                const auto last = value.find_last_not_of(" \t\r\n");
                if (first == std::string::npos || last == std::string::npos) {
                    return std::string{};
                }
                return value.substr(first, last - first + 1);
            };
            while (std::getline(iss, line)) {
                if (line.empty() || line.front() == '#') {
                    continue;
                }
                const auto pos = line.find('=');
                if (pos == std::string::npos) {
                    const std::string token = trim(line);
                    if (!token.empty() && (token.front() == '-' || token.front() == '*')) {
                        if (token.size() > 1) {
                            data.roadmap.push_back(trim(token.substr(1)));
                        }
                    }
                    continue;
                }
                std::string key = trim(line.substr(0, pos));
                std::string value = trim(line.substr(pos + 1));
                if (key == "name") {
                    data.name = std::move(value);
                } else if (key == "author") {
                    data.author = std::move(value);
                } else if (key == "version") {
                    data.version = std::move(value);
                } else if (key == "website") {
                    data.website = std::move(value);
                } else if (key == "description") {
                    data.description = std::move(value);
                } else if (key == "features") {
                    std::istringstream featuresStream(value);
                    std::string feature;
                    while (std::getline(featuresStream, feature, ',')) {
                        feature = trim(feature);
                        if (!feature.empty()) {
                            data.features.push_back(std::move(feature));
                        }
                    }
                }
            }
        } catch (const std::exception& ex) {
            std::cerr << "[bootstrap] failed to read manifest: " << ex.what() << '\n';
        }
    }

    print_banner();
    std::cout << "==== erelang Bootstrap Overview ====\n";
    std::cout << data.name;
    if (!data.version.empty()) {
        std::cout << " v" << data.version;
    }
    if (!data.author.empty()) {
        std::cout << " by " << data.author;
    }
    std::cout << '\n';

    if (!data.description.empty()) {
        std::cout << data.description << "\n";
    }
    if (!data.website.empty()) {
        std::cout << "Website: " << data.website << "\n";
    }
    if (!data.features.empty()) {
        std::cout << "Features:";
        for (const auto& feature : data.features) {
            std::cout << "\n  - " << feature;
        }
        std::cout << "\n";
    }
    if (!data.roadmap.empty()) {
        std::cout << "Roadmap:";
        for (const auto& entry : data.roadmap) {
            std::cout << "\n  * " << entry;
        }
        std::cout << "\n";
    }

    std::cout << "(Bootstrap UI now uses standard console output; scripts should rely on win_* built-ins for GUIs.)\n";
    return 0;
}

[[nodiscard]] int handle_about(const cli::BootstrapOptions& options, const ExecutionContext& ctx) {
    print_banner();
    std::cout << "Reasons it leans DSL-like / special-purpose\n\n";
    std::cout << "Scope is narrow\n\n";
    std::cout << "It bakes in things like GUI, filesystem, UUIDs, debugging, and entities.\n\n";
    std::cout << "There’s no mention of broad ecosystems (e.g., networking libraries, math/science packages, concurrency models beyond parallel {}).\n\n";
    std::cout << "Small type system\n\n";
    std::cout << "Only str, int, bool.\n\n";
    std::cout << "No complex generics, floats, or advanced memory management.\n\n";
    std::cout << "Built-in focus\n\n";
    std::cout << "The language is opinionated: it decides what’s first-class (Window, Gui, entity) rather than being extensible like Python or C++.\n";
    return 0;
}

[[nodiscard]] int handle_run(const cli::RunOptions& options,
                             ExecutionContext& ctx,
                             const std::vector<std::string>& originalArgs) {
    if (options.script.empty()) {
        std::cerr << "No script specified." << std::endl;
        return 1;
    }

    const fs::path input = options.script;
    if (!fs::exists(input)) {
        std::cerr << "Input not found: " << input << '\n';
        return 1;
    }

    erelang::PolicyManager::instance().load("policy.cfg");
    erelang::Runtime::set_cli_args(originalArgs);

    try {
#ifdef ERELANG_STATIC_RUNNER
        std::vector<std::string> cliArgs = options.scriptArgs;
        std::unordered_set<std::string> visited;
        std::vector<erelang::Program> ordered;
        load_plugins_into_programs(ctx.pluginManifests, visited, ordered);
        if (options.debug) {
            const auto layout = detect_build_layout(ctx.executablePath);
            fs::path probe = layout.projectRoot;
            fs::path debugScript;
            for (int depth = 0; depth < 6 && !probe.empty(); ++depth) {
                const fs::path debugDir = probe / "examples" / "debug";
                const fs::path candidateElan = debugDir / "debug.elan";
                const fs::path candidateObs = debugDir / "debug.0bs";
                if (fs::exists(candidateElan)) {
                    debugScript = candidateElan;
                    break;
                }
                if (fs::exists(candidateObs)) {
                    debugScript = candidateObs;
                    break;
                }
                probe = probe.parent_path();
            }
            if (!debugScript.empty()) {
                load_program_recursive(debugScript.string(), visited, ordered);
            }
        }
        load_program_recursive(fs::absolute(input).string(), visited, ordered);
        if (ordered.empty()) {
            std::cerr << "No program loaded\n";
            return 1;
        }

        erelang::Program merged;
        for (std::size_t i = 0; i + 1 < ordered.size(); ++i) {
            const auto& mod = ordered[i];
            merged.actions.insert(merged.actions.end(), mod.actions.begin(), mod.actions.end());
            merged.hooks.insert(merged.hooks.end(), mod.hooks.begin(), mod.hooks.end());
            merged.entities.insert(merged.entities.end(), mod.entities.begin(), mod.entities.end());
        }

        erelang::Program mainProgram = ordered.back();
        if (!merged.actions.empty()) {
            mainProgram.actions.insert(mainProgram.actions.begin(), merged.actions.begin(), merged.actions.end());
        }
        if (!merged.hooks.empty()) {
            mainProgram.hooks.insert(mainProgram.hooks.begin(), merged.hooks.begin(), merged.hooks.end());
        }
        if (!merged.entities.empty()) {
            mainProgram.entities.insert(mainProgram.entities.begin(), merged.entities.begin(), merged.entities.end());
        }

        const auto append_aliases = [&](const erelang::Program& src) {
            for (const auto& alias : src.pluginAliases) {
                if (std::find(mainProgram.pluginAliases.begin(), mainProgram.pluginAliases.end(), alias) == mainProgram.pluginAliases.end()) {
                    mainProgram.pluginAliases.push_back(alias);
                }
            }
        };
        for (const auto& programSrc : ordered) {
            append_aliases(programSrc);
        }

        if (options.debug) {
            bool hasDebugMain = false;
            for (const auto& action : mainProgram.actions) {
                if (action.name == "debug_main") {
                    hasDebugMain = true;
                    break;
                }
            }
            if (hasDebugMain) {
                mainProgram.runTarget = std::string{"debug_main"};
            }
        }
        if (!mainProgram.runTarget) {
            for (const auto& action : mainProgram.actions) {
                if (action.name == "main") {
                    mainProgram.runTarget = std::string{"main"};
                    break;
                }
            }
        }

        erelang::TypeChecker typeChecker;
        auto tcResult = typeChecker.check(mainProgram);
        if (!tcResult.ok) {
            for (const auto& diag : tcResult.diagnostics) {
                const char* tag = diag.severity == erelang::Severity::Warning ? "[warn] "
                                  : (diag.severity == erelang::Severity::Note ? "[note] " : "[error] ");
                std::cerr << tag << diag.code << ": " << diag.message;
                if (!diag.context.empty()) {
                    std::cerr << " (" << diag.context << ")";
                }
                std::cerr << '\n';
            }
            return 1;
        }

        (void)erelang::optimize_program(mainProgram);

        erelang::Runtime runtime;
        runtime.register_plugins(ctx.runtimePlugins);
        erelang::Runtime::set_cli_args(cliArgs);

        return runtime.run(mainProgram);
#else
        const fs::path dir = ctx.executableDir;
        if (!g_ob.load(dir)) {
            std::cerr << "Failed to load erelang.dll from: " << dir << '\n';
            return 1;
        }

        std::vector<const char*> cargv;
        cargv.reserve(options.scriptArgs.size());
        for (const auto& arg : options.scriptArgs) {
            cargv.push_back(arg.c_str());
        }

        char* err = nullptr;
        const int flags = options.debug ? 0x1 : 0;
        const int rc = g_ob.run(input.string().c_str(), static_cast<int>(cargv.size()), cargv.data(), flags, &err);
        if (rc != 0 && err) {
            std::cerr << err << '\n';
            g_ob.freeStr(err);
        }
        return rc;
#endif
    } catch (const std::exception& ex) {
        std::cerr << "Run error: " << ex.what() << '\n';
        return 1;
    }
}

} // namespace erelang

int main(int argc, char** argv) {
    // If no args, just show banner (no GUI)
    if (argc <= 1) { print_banner(); return 0; }
    // Collect raw args first for detection
    bool bootstrapRequested = false; // now only via --about or legacy --bootstrap
    float forcedUiScale = 0.0f;
    bool defaultVsync = true;
    for (int i=1;i<argc;++i) {
        std::string a = argv[i];
        if (a == "--about" || a == "--bootstrap") bootstrapRequested = true; // repurpose --about to show the rich UI
        else if (a == "--ui-scale" && i+1 < argc) { forcedUiScale = std::max(0.5f, std::stof(argv[++i])); }
        else if (a == "--no-vsync") { defaultVsync = false; }
    }
    if (bootstrapRequested) {
        erelang::ExecutionContext ctx = erelang::build_execution_context(argc > 0 ? argv[0] : nullptr);
        erelang::cli::BootstrapOptions opts{};
        opts.uiScale = forcedUiScale;
        opts.vsync = defaultVsync;
        return erelang::handle_bootstrap(opts, ctx);
    }
    std::vector<std::string> args(argv + 1, argv + argc);
    // Pass CLI args to runtime for deterministic seed scan
    erelang::Runtime::set_cli_args(args);
    // Load policy file if present (policy.cfg) in current directory
    erelang::PolicyManager::instance().load("policy.cfg");

    erelang::ExecutionContext ctx = erelang::build_execution_context(argc > 0 ? argv[0] : nullptr);
    [[maybe_unused]] auto& pluginManifests = ctx.pluginManifests;
#ifdef ERELANG_STATIC_RUNNER
    [[maybe_unused]] auto& runtimePluginRecords = ctx.runtimePlugins;
#endif
    if (args.size() == 1 && (args[0] == "--help" || args[0] == "-h")) { print_help(); return 0; }
    if (!args.empty() && !args[0].empty() && args[0][0] != '-') {
        erelang::cli::RunOptions runOptions;
        runOptions.script = fs::path(args[0]);
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--debug") {
                runOptions.debug = true;
            } else {
                runOptions.scriptArgs.emplace_back(args[i]);
            }
        }
        return erelang::handle_run(runOptions, ctx, args);
    }

    if (!args.empty() && args[0] == "--make-debug") {
        // Resolve project root and build/debug output
    const auto layout = erelang::detect_build_layout(fs::absolute(fs::path(argv[0])));
        fs::path input;
        {
            fs::path probe = layout.projectRoot;
            for (int depth = 0; depth < 6 && !probe.empty(); ++depth) {
                const fs::path debugDir = probe / "examples" / "debug";
                const fs::path candidateElan = debugDir / "debug.elan";
                const fs::path candidateObs = debugDir / "debug.0bs";
                if (fs::exists(candidateElan)) {
                    input = candidateElan;
                    break;
                }
                if (fs::exists(candidateObs)) {
                    input = candidateObs;
                    break;
                }
                probe = probe.parent_path();
            }
            if (input.empty()) {
                std::cerr << "debug script not found (tried debug.elan, debug.0bs); searched upwards from: " << layout.projectRoot << "\n";
                return 1;
            }
        }
        fs::path output;
        for (size_t i=1; i<args.size(); ++i) {
            if (args[i] == "--output" && i+1 < args.size()) { output = args[i+1]; ++i; }
        }
        if (output.empty()) {
            output = layout.exeDir / "debug.exe";
        }
        // Reuse compile flow by simulating --compile
    std::vector<std::string> faux = {"--compile", input.string(), "--output", output.string(), "--static"};
        // Build library path checks (shared with compile branch)
        // Fall through into compile logic by resetting args
        args = std::move(faux);
    }

    if (args.size() >= 2 && args[0] == "--compile") {
        fs::path input = args[1];
        if (!fs::exists(input)) { std::cerr << "Input not found: " << input << "\n"; return 1; }

        fs::path output;
        bool preferStatic = false;
        bool preferDynamic = false;
        for (size_t i=2; i<args.size(); ++i) {
            if (args[i] == "--output" && i+1 < args.size()) { output = args[i+1]; ++i; }
            else if (args[i] == "--static") { preferStatic = true; }
            else if (args[i] == "--dynamic") { preferDynamic = true; }
        }
        // Default to static unless user explicitly requests dynamic
        if (!preferStatic && !preferDynamic) preferStatic = true;
        if (output.empty()) {
            output = input;
            output.replace_extension(".exe");
        }

        // Collect files and contents (static runner does this locally; dynamic uses DLL)
        std::vector<std::pair<fs::path,std::string>> orderedFiles;
#ifdef ERELANG_STATIC_RUNNER
        {
            std::unordered_set<std::string> visited;
            std::function<void(const std::string&)> load_file = [&](const std::string& absPath){
                fs::path p = fs::absolute(absPath);
                std::string key = p.string();
                if (visited.count(key)) return;
                visited.insert(key);
                erelang::LexerOptions lxopts; lxopts.enableDurations = true; lxopts.enableUnits = true; lxopts.enablePolyIdentifiers = true; lxopts.emitDocComments = true; lxopts.emitComments = false;
                std::string text = read_all_text(key);
                erelang::Lexer lx(text, lxopts);
                auto toks = lx.lex();
                erelang::Parser ps(std::move(toks));
                erelang::Program prog = ps.parse();
                orderedFiles.emplace_back(key, std::move(text));
                for (const auto& imp : prog.imports) {
                    if (imp.pluginGlob) continue;
                    auto next = resolve_import_local(key, imp.path);
                    if (next) load_file(*next);
                }
            };
            load_file(fs::absolute(input).string());
        }
#else
        {
            fs::path exeP = fs::absolute(fs::path(argv[0]));
            fs::path dir = exeP.parent_path();
            if (!g_ob.load(dir)) { std::cerr << "Failed to load erelang.dll from: " << dir << "\n"; return 1; }
            struct Ctx { std::vector<std::pair<fs::path,std::string>>* files; } ctx{ &orderedFiles };
            auto on_file = [](const char* path, const char* contents, void* user){
                auto* c = reinterpret_cast<Ctx*>(user);
                c->files->emplace_back(fs::path(path), std::string(contents ? contents : ""));
            };
            char* err = nullptr; int r = g_ob.collect(input.string().c_str(), on_file, &ctx, &err);
            if (r != 0) { if (err) { std::cerr << err << "\n"; g_ob.freeStr(err); } return r; }
        }
#endif

        // Scan and include .olib/.ol libraries
        auto include_lib_sources = [&](const fs::path& libRoot){
            std::error_code ec;
            if (!fs::exists(libRoot, ec)) return;
            for (auto& entry : fs::directory_iterator(libRoot, ec)) {
                if (!entry.is_directory() && !entry.is_regular_file()) continue;
                fs::path p = entry.path();
                // .olib as directory
                if (entry.is_directory() && p.extension() == ".olib") {
                    fs::path manifest = p / "manifest.erelang";
                    if (fs::exists(manifest)) {
                        std::ifstream in(manifest);
                        std::string line, libname;
                        std::vector<std::string> filesList;
                        while (std::getline(in, line)) {
                            if (line.empty() || line[0] == '#') continue;
                            auto pos = line.find('=');
                            if (pos == std::string::npos) continue;
                            std::string k = line.substr(0, pos); std::string v = line.substr(pos+1);
                            auto trim = [](std::string s){ size_t a=s.find_first_not_of(" \t\r\n"); size_t b=s.find_last_not_of(" \t\r\n"); return a==std::string::npos?std::string():s.substr(a,b-a+1); };
                            k = trim(k); v = trim(v);
                            if (k == "name") libname = v;
                            else if (k == "files") {
                                std::string cur; std::istringstream ss(v);
                                while (std::getline(ss, cur, ',')) { filesList.push_back(trim(cur)); }
                                // also split on ';'
                                std::vector<std::string> tmp;
                                for (auto& f : filesList) {
                                    size_t pos2=0; while ((pos2=f.find(';'))!=std::string::npos) { tmp.push_back(f.substr(0,pos2)); f.erase(0,pos2+1);} tmp.push_back(f);
                                }
                                filesList = tmp;
                            }
                        }
                        for (auto& rf : filesList) {
                            if (rf.empty()) continue;
                            fs::path fp = p / rf;
                            if (!fp.has_extension()) {
                                fs::path c1 = fp; c1.replace_extension(".0bs");
                                fs::path c2 = fp; c2.replace_extension(".obsecret");
                                if (fs::exists(c1)) fp = c1; else if (fs::exists(c2)) fp = c2;
                            }
                            if (fs::exists(fp)) {
                                std::ifstream fin(fp, std::ios::binary); std::ostringstream ss; ss << fin.rdbuf();
                                // synthetic key ends with /<rel>
                                orderedFiles.emplace_back(fs::path("/__olib__/") / (libname.empty()?p.stem().string():libname) / rf, ss.str());
                            }
                        }
                    }
                }
                // .ol single-file manifest
                if (entry.is_regular_file() && p.extension() == ".ol") {
                    std::ifstream in(p); std::string line, libname;
                    std::vector<std::string> filesList;
                    while (std::getline(in, line)) {
                        if (line.empty() || line[0]=='#') continue;
                        auto pos = line.find('=');
                        if (pos == std::string::npos) continue;
                        std::string k = line.substr(0,pos); std::string v=line.substr(pos+1);
                        auto trim = [](std::string s){ size_t a=s.find_first_not_of(" \t\r\n"); size_t b=s.find_last_not_of(" \t\r\n"); return a==std::string::npos?std::string():s.substr(a,b-a+1); };
                        k = trim(k); v=trim(v);
                        if (k=="name") libname=v;
                        else if (k=="file") filesList.push_back(v);
                    }
                    for (auto& rf : filesList) {
                        if (rf.empty()) continue;
                        fs::path fp = p.parent_path() / rf; if (!fp.has_extension()) { fs::path c1=fp; c1.replace_extension(".0bs"); fs::path c2=fp; c2.replace_extension(".obsecret"); if (fs::exists(c1)) fp=c1; else if (fs::exists(c2)) fp=c2; }
                        if (fs::exists(fp)) { std::ifstream fin(fp, std::ios::binary); std::ostringstream ss; ss << fin.rdbuf();
                            orderedFiles.emplace_back(fs::path("/__ol__/") / (libname.empty()?p.stem().string():libname) / rf, ss.str());
                        }
                    }
                }
            }
        };
    // default lib roots
    const auto layout = erelang::detect_build_layout(fs::absolute(fs::path(argv[0])));
    include_lib_sources(layout.projectRoot / "libs");
    include_lib_sources(layout.projectRoot / "Erelang" / "libs");
    include_lib_sources(layout.buildRoot / "libs");

    const std::string& configName = layout.config;
    auto locate_file = [](const std::vector<fs::path>& roots, const std::vector<std::string>& names) -> fs::path {
        for (const auto& root : roots) {
            if (root.empty()) continue;
            for (const auto& name : names) {
                fs::path candidate = root / name;
                if (fs::exists(candidate)) {
                    return candidate;
                }
            }
        }
        return {};
    };

    std::vector<fs::path> dllRoots;
    if (!configName.empty()) {
        dllRoots.push_back(layout.binDir / "dll" / configName);
        dllRoots.push_back(layout.buildRoot / "bin" / "dll" / configName);
        dllRoots.push_back(layout.buildRoot / "bin" / configName);
    }
    dllRoots.push_back(layout.binDir / "dll");
    dllRoots.push_back(layout.binDir);
    dllRoots.push_back(layout.buildRoot);

    std::vector<fs::path> libRoots;
    if (!configName.empty()) {
        libRoots.push_back(layout.buildRoot / "lib" / configName);
        libRoots.push_back(layout.buildRoot / configName);
    }
    libRoots.push_back(layout.buildRoot / "lib");
    libRoots.push_back(layout.buildRoot);
    libRoots.push_back(layout.binDir);

    fs::path dllPath = locate_file(dllRoots, { "liberelang.dll", "erelang.dll" });
    fs::path importLibA = locate_file(libRoots, { "liberelang.dll.a" });
    fs::path importLibMSVC = locate_file(libRoots, { "erelang.lib" });
    fs::path libPath = locate_file(libRoots, { "liberelang.a", "erelang.lib" });
        bool canUseDynamic = fs::exists(dllPath) && (fs::exists(importLibA) || fs::exists(importLibMSVC));
        bool useDynamic = (preferDynamic) && canUseDynamic;
        // Temporary: MinGW dynamic linking not yet supported (unresolved C++ symbols). Fallback to static.
        if (useDynamic && fs::exists(importLibA)) {
            std::cout << "[erelang] --dynamic requested, but MinGW import library detected; falling back to static until DLL exports are stabilized.\n";
            useDynamic = false;
            preferStatic = true;
        }
        if (!useDynamic && !fs::exists(libPath)) {
#ifdef _WIN32
            // Try to extract embedded library resource (RCDATA) into a temp file
            auto hMod = GetModuleHandleW(nullptr);
            HRSRC hRes = FindResourceW(hMod, L"OB_LIB", MAKEINTRESOURCEW(RT_RCDATA_NUM));
            if (hRes) {
                HGLOBAL hData = LoadResource(hMod, hRes);
                if (hData) {
                    DWORD sz = SizeofResource(hMod, hRes);
                    void* p = LockResource(hData);
                    if (p && sz > 0) {
                        try {
                            fs::path tempDir = fs::temp_directory_path() / "erelang_embed";
                            fs::create_directories(tempDir);
                            fs::path outA = tempDir / "liberelang.a";
                            fs::path outLibName = tempDir / "erelang.lib";
                            {
                                std::ofstream o(outA, std::ios::binary);
                                o.write(static_cast<const char*>(p), static_cast<std::streamsize>(sz));
                            }
                            {
                                std::ofstream o(outLibName, std::ios::binary);
                                o.write(static_cast<const char*>(p), static_cast<std::streamsize>(sz));
                            }
                            if (fs::exists(outA)) {
                                libPath = outA; // Prefer MinGW .a by default
                            } else if (fs::exists(outLibName)) {
                                libPath = outLibName;
                            }
                        } catch (...) {
                            // Ignore, will fall back to error below
                        }
                    }
                }
            }
#endif
            if (!fs::exists(libPath)) {
                std::cerr << "Cannot find erelang library in build directory.\n";
                return 1;
            }
        }
    fs::path includeDir = layout.projectRoot / "include";
        if (!fs::exists(includeDir)) {
#ifdef _WIN32
            // Extract embedded headers into temp if source tree not present
            fs::path tempInc = fs::temp_directory_path() / "erelang_embed" / "include" / "erelang";
            bool ok1 = extract_rcdata_to_file(L"OB_INC_LEXER", tempInc / "lexer.hpp");
            bool ok2 = extract_rcdata_to_file(L"OB_INC_PARSER", tempInc / "parser.hpp");
            bool ok3 = extract_rcdata_to_file(L"OB_INC_RUNTIME", tempInc / "runtime.hpp");
            bool ok4 = extract_rcdata_to_file(L"OB_INC_TYPECHECKER", tempInc / "typechecker.hpp");
            bool ok5 = extract_rcdata_to_file(L"OB_INC_OPTIMIZER", tempInc / "optimizer.hpp");
            bool ok6 = extract_rcdata_to_file(L"OB_INC_SYMBOLTABLE", tempInc / "symboltable.hpp");
            bool ok7 = extract_rcdata_to_file(L"OB_INC_MODULES", tempInc / "modules.hpp");
            bool ok8 = extract_rcdata_to_file(L"OB_INC_STDLIB", tempInc / "stdlib.hpp");
            bool ok9 = extract_rcdata_to_file(L"OB_INC_FFI", tempInc / "ffi.hpp");
            bool ok10 = extract_rcdata_to_file(L"OB_INC_ERROR", tempInc / "error.hpp");
            bool ok11 = extract_rcdata_to_file(L"OB_INC_AST", tempInc / "ast.hpp");
            bool ok12 = extract_rcdata_to_file(L"OB_INC_GC", tempInc / "gc.hpp");
            if (ok1 && ok2 && ok3 && ok4 && ok5 && ok6 && ok7 && ok8 && ok9 && ok10 && ok11 && ok12) {
                includeDir = tempInc.parent_path(); // .../include
            } else {
                std::cerr << "Include directory not found and failed to extract embedded headers." << "\n";
                return 1;
            }
#else
            std::cerr << "Include directory not found: " << includeDir << "\n"; return 1;
#endif
        }

    fs::path tempProj = layout.buildRoot / "erelang_compile" / input.stem();
        fs::create_directories(tempProj);
    if (useDynamic) {
        fs::path chosenImplib = fs::exists(importLibA) ? importLibA : importLibMSVC;
        std::cout << "[erelang] Using dynamic erelang: " << dllPath << " with import lib " << chosenImplib << "\n";
    } else {
        std::cout << "[erelang] Using lib: " << libPath << "\n";
    }
    std::cout << "[erelang] Using includes: " << includeDir << "\n";
    std::cout << "[erelang] Temp project: " << tempProj << "\n";
        // Generate main.cpp
        std::ofstream outCpp(tempProj / "main.cpp", std::ios::binary);
        outCpp << generate_bootstrap_source(input, orderedFiles);
        outCpp.close();
        // Generate CMakeLists.txt
    std::ofstream outCmake(tempProj / "CMakeLists.txt", std::ios::binary);
    const std::string libLoc = libPath.generic_string();
    const std::string incLoc = includeDir.generic_string();
    outCmake << "cmake_minimum_required(VERSION 3.20)\n"
        "project(erelangApp LANGUAGES CXX)\n"
        "set(CMAKE_CXX_STANDARD 20)\n"
        "if(NOT CMAKE_BUILD_TYPE)\n"
        "  set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING \"\" FORCE)\n"
        "endif()\n"
        "if(MINGW)\n"
        "  add_compile_options(-Os -ffunction-sections -fdata-sections)\n"
        "  add_link_options(-s -Wl,--gc-sections)\n"
        "endif()\n"
    "add_executable(app main.cpp)\n";
    if (useDynamic) {
        if (fs::exists(importLibA)) {
            outCmake << "# MinGW: link directly to import library\n"
                        "target_include_directories(app PRIVATE \"" << incLoc << "\")\n"
                        "target_link_libraries(app PRIVATE \"" << (importLibA.generic_string()) << "\" advapi32 user32 gdi32 comctl32 rpcrt4 winhttp)\n";
        } else {
            const std::string dllLoc = dllPath.generic_string();
            const std::string implibLoc = importLibMSVC.generic_string();
            outCmake << "# MSVC: import shared lib via IMPORTED target\n"
                        "add_library(erelang SHARED IMPORTED)\n"
                        "set_target_properties(erelang PROPERTIES IMPORTED_LOCATION \"" << dllLoc << "\")\n"
                        "set_target_properties(erelang PROPERTIES IMPORTED_IMPLIB \"" << implibLoc << "\")\n"
                        "target_include_directories(erelang INTERFACE \"" << incLoc << "\")\n"
                        "target_link_libraries(app PRIVATE erelang advapi32 user32 gdi32 comctl32 rpcrt4 winhttp)\n";
        }
    } else {
    outCmake << "add_library(erelang STATIC IMPORTED)\n"
            "set_target_properties(erelang PROPERTIES IMPORTED_LOCATION \"" << libLoc << "\")\n"
            "target_include_directories(erelang INTERFACE \"" << incLoc << "\")\n"
            "target_include_directories(app PRIVATE \"" << incLoc << "\")\n"
            "target_link_libraries(app PRIVATE erelang advapi32 user32 gdi32 comctl32 rpcrt4 winhttp)\n";
    }
        outCmake.close();

    fs::path tempBuild = tempProj / "build";
        fs::create_directories(tempBuild);

    std::string generator = "MinGW Makefiles";
    std::string ext = libPath.extension().string();
    for (auto& c : ext) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    if (ext == ".lib") generator = "NMake Makefiles";
    std::string cfg = std::string("cmake -S ") + '"' + tempProj.string() + '"' + " -B " + '"' + tempBuild.string() + '"' + " -G \"" + generator + "\" -DCMAKE_BUILD_TYPE=MinSizeRel";
    std::cout << "[erelang] CMake configure: " << cfg << "\n";
        if (run_command(cfg, tempProj) != 0) { std::cerr << "CMake configure failed\n"; return 1; }
        std::string bld = std::string("cmake --build ") + '"' + tempBuild.string() + '"' + " --config Release";
    std::cout << "[erelang] CMake build: " << bld << "\n";
        if (run_command(bld, tempProj) != 0) { std::cerr << "Build failed\n"; return 1; }


    fs::path produced = tempBuild / "app.exe";
        if (!fs::exists(produced)) {
            std::cerr << "Expected output not found: " << produced << "\n"; return 1;
        }
    if (!output.parent_path().empty()) {
        if (!output.parent_path().empty()) {
            fs::create_directories(output.parent_path());
        }
    }
    std::error_code ec;
    if (fs::exists(output)) fs::remove(output, ec);
    ec = {};
    fs::copy_file(produced, output, fs::copy_options::overwrite_existing, ec);
    if (ec) { std::cerr << "Failed to copy output: " << ec.message() << "\n"; return 1; }
        
        std::vector<std::string> copiedOdlls;
        auto copy_odlls = [&](const fs::path& libRoot){
            std::error_code ec;
            if (!fs::exists(libRoot, ec)) return;
            for (auto& entry : fs::directory_iterator(libRoot, ec)) {
                if (entry.is_regular_file() && entry.path().extension() == ".odll") {
                    fs::path dst = output.parent_path() / entry.path().filename();
                    fs::copy_file(entry.path(), dst, fs::copy_options::overwrite_existing, ec);
                    copiedOdlls.push_back(dst.filename().generic_string());
                }
                if (entry.is_directory() && entry.path().extension() == ".olib") {
                    // nested odlls
                    for (auto& e2 : fs::directory_iterator(entry.path(), ec)) {
                        if (e2.is_regular_file() && e2.path().extension() == ".odll") {
                            fs::path dst2 = output.parent_path() / e2.path().filename();
                            fs::copy_file(e2.path(), dst2, fs::copy_options::overwrite_existing, ec);
                            copiedOdlls.push_back(dst2.filename().generic_string());
                        }
                    }
                }
            }
        };
    copy_odlls(layout.projectRoot / "libs");
    copy_odlls(layout.projectRoot / "Erelang" / "libs");
        // Copy manifests from .olib for reference (avoid copying sources to keep size small); record for manifest
        std::vector<std::string> copiedLibManifests;
        auto copy_manifests = [&](const fs::path& libRoot){
            std::error_code ec;
            if (!fs::exists(libRoot, ec)) return;
            for (auto& entry : fs::directory_iterator(libRoot, ec)) {
                if (entry.is_directory() && entry.path().extension() == ".olib") {
                    fs::path man = entry.path() / "manifest.erelang";
                    if (fs::exists(man)) {
                        fs::path dstDir = output.parent_path() / "libs" / entry.path().filename();
                        fs::create_directories(dstDir, ec);
                        fs::path dst = dstDir / "manifest.erelang";
                        fs::copy_file(man, dst, fs::copy_options::overwrite_existing, ec);
                        copiedLibManifests.push_back(fs::relative(dst, output.parent_path()).generic_string());
                    }
                }
                if (entry.is_regular_file() && entry.path().extension() == ".ol") {
                    fs::path dstDir = output.parent_path() / "libs";
                    fs::create_directories(dstDir, ec);
                    fs::path dst = dstDir / entry.path().filename();
                    fs::copy_file(entry.path(), dst, fs::copy_options::overwrite_existing, ec);
                    copiedLibManifests.push_back(fs::relative(dst, output.parent_path()).generic_string());
                }
            }
        };
    copy_manifests(layout.projectRoot / "libs");
    copy_manifests(layout.projectRoot / "Erelang" / "libs");

        // If using dynamic runtime, copy erelang.dll
        if (useDynamic) {
            std::error_code ec; fs::copy_file(dllPath, output.parent_path() / dllPath.filename(), fs::copy_options::overwrite_existing, ec);
        }

        // Generate manifest.erelang next to the built exe
        try {
            fs::path manifestPath = output.parent_path() / "manifest.erelang";
            std::ofstream mout(manifestPath, std::ios::binary);
            if (mout) {
                auto now = std::chrono::system_clock::now();
                std::time_t t = std::chrono::system_clock::to_time_t(now);
                // Basic fields
                mout << "name=" << output.stem().generic_string() << "\n";
                mout << "input=" << fs::absolute(input).generic_string() << "\n";
                mout << "build=" << (useDynamic ? "dynamic" : "static") << "\n";
                mout << "timestamp=" << std::string(std::ctime(&t) ? std::ctime(&t) : "") << "";
                // Embedded files list
                if (!orderedFiles.empty()) {
                    mout << "files=";
                    for (size_t i = 0; i < orderedFiles.size(); ++i) {
                        mout << orderedFiles[i].first.generic_string();
                        if (i + 1 < orderedFiles.size()) mout << ",";
                    }
                    mout << "\n";
                }
                // Copied dynamic modules
                if (!copiedOdlls.empty()) {
                    mout << "odll=";
                    for (size_t i = 0; i < copiedOdlls.size(); ++i) {
                        mout << copiedOdlls[i];
                        if (i + 1 < copiedOdlls.size()) mout << ",";
                    }
                    mout << "\n";
                }
                // Library manifests present in output
                if (!copiedLibManifests.empty()) {
                    mout << "libs=";
                    for (size_t i = 0; i < copiedLibManifests.size(); ++i) {
                        mout << copiedLibManifests[i];
                        if (i + 1 < copiedLibManifests.size()) mout << ",";
                    }
                    mout << "\n";
                }
            }
        } catch (...) { /* best-effort; ignore */ }

        // Best-effort strip (further size reduction)
#ifdef _WIN32
        {
            fs::path absOut = fs::absolute(output);
            std::string stripCmd = std::string("strip ") + '"' + absOut.string() + '"';
            (void)run_command(stripCmd, absOut.parent_path());
        }
#endif

        // Cleanup temp project
        std::error_code rmec; fs::remove_all(tempProj, rmec);

        std::cout << "Built: " << output << "\n";
        return 0;
    }

    // Default banner for any other case
    print_banner();
    return 0;
}
