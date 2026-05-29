#include "erelang/cabi.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <optional>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <functional>
#include <cstdlib>
#include <cstring>

#include "erelang/lexer.hpp"
#include "erelang/parser.hpp"
#include "erelang/typechecker.hpp"
#include "erelang/optimizer.hpp"
#include "erelang/symboltable.hpp"
#include "erelang/modules.hpp"
#include "erelang/runtime.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace erelang;
namespace fs = std::filesystem;

static std::string slurp_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss; ss << in.rdbuf();
    return ss.str();
}

static std::optional<fs::path> materialize_module_file(const std::string& importName) {
    std::string norm = importName; for (auto& ch : norm) if (ch=='\\') ch = '/';
    std::vector<std::string> names;
    if (norm.find('.') == std::string::npos) names = {norm+".0bs", norm+".obsecret"}; else names = {norm};
    const auto mods = get_registered_modules();
    auto ends_with = [](const std::string& s, const std::string& suf){ return s.size()>=suf.size() && s.rfind(suf)==s.size()-suf.size(); };
    for (const auto& m : mods) {
        for (size_t i=0; i<m.file_count; ++i) {
            const ModuleFile& f = m.files[i]; if (!f.name || !f.contents) continue;
            std::string fn = f.name; for (auto& ch : fn) if (ch=='\\') ch = '/';
            for (const auto& nm : names) {
                std::string tail = std::string("/") + nm;
                if (ends_with(fn, tail)) {
                    fs::path out = fs::temp_directory_path() / "erelang_modules" / fn; std::error_code ec; fs::create_directories(out.parent_path(), ec);
                    std::ofstream o(out, std::ios::binary); if (!o) break; o << f.contents; o.close(); return out;
                }
            }
        }
    }
    return std::nullopt;
}

static void set_error_string(char** out_error, const std::string& message) {
    if (!out_error) return;
    char* buf = static_cast<char*>(::malloc(message.size() + 1));
    if (!buf) return;
    std::memcpy(buf, message.c_str(), message.size() + 1);
    *out_error = buf;
}

static int run_programs(std::vector<Program>& ordered, int argc, const char* argv[], int flags, char** out_error) {
    if (ordered.empty()) {
        set_error_string(out_error, "No program loaded");
        return 1;
    }
    const bool debug = (flags & 0x1) != 0;
    (void)debug;
    Program merged;
    for (size_t i = 0; i + 1 < ordered.size(); ++i) {
        const Program& m = ordered[i];
        merged.actions.insert(merged.actions.end(), m.actions.begin(), m.actions.end());
        merged.hooks.insert(merged.hooks.end(), m.hooks.begin(), m.hooks.end());
        merged.entities.insert(merged.entities.end(), m.entities.begin(), m.entities.end());
    }
    Program mainProg = ordered.back();
    if (!merged.actions.empty()) mainProg.actions.insert(mainProg.actions.begin(), merged.actions.begin(), merged.actions.end());
    if (!merged.hooks.empty()) mainProg.hooks.insert(mainProg.hooks.begin(), merged.hooks.begin(), merged.hooks.end());
    if (!merged.entities.empty()) mainProg.entities.insert(mainProg.entities.begin(), merged.entities.begin(), merged.entities.end());
    if (!mainProg.runTarget) {
        for (const auto& a : mainProg.actions) {
            if (a.name == "main") {
                mainProg.runTarget = std::string("main");
                break;
            }
        }
    }
    SymbolTable symtab;
    for (const auto& a : mainProg.actions) symtab.add(a.name, "action");
    for (const auto& e : mainProg.entities) symtab.add(e.name, "entity");
    TypeChecker tc;
    auto tcRes = tc.check(mainProg);
    if (!tcRes.ok) {
        std::ostringstream es;
        for (auto& d : tcRes.diagnostics) {
            es << d.code << ": " << d.message;
            if (!d.context.empty()) es << " (" << d.context << ")";
            es << "\n";
        }
        set_error_string(out_error, es.str());
        return 1;
    }
    (void)optimize_program(mainProg);
    std::vector<std::string> args;
    args.reserve(argc);
    for (int i = 0; i < argc; ++i) args.emplace_back(argv[i] ? argv[i] : "");
    Runtime::set_cli_args(args);
    Runtime rt;
    return rt.run(mainProg);
}

extern "C" {

OB_API int ob_run_file(const char* main_file, int argc, const char* argv[], int flags, char** out_error) {
    try {
        fs::path exeP;
#ifdef _WIN32
        {
            wchar_t buf[MAX_PATH]; DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
            exeP = fs::path(std::wstring(buf, n));
        }
#endif
        fs::path buildDir = exeP.empty() ? fs::current_path() : exeP.parent_path();
        std::optional<fs::path> projectRoot; if (!buildDir.empty()) projectRoot = buildDir.parent_path();
        load_dynamic_modules_in_dir(buildDir);
        const bool debug = (flags & 0x1) != 0;
        std::optional<fs::path> debugDriver;
        if (debug && projectRoot) {
            const fs::path libDir = *projectRoot / "examples" / "lib";
            const fs::path debugDir = *projectRoot / "examples" / "debug";
            const fs::path tryLibElan = libDir / "debugger.elan";
            const fs::path tryLibObs = libDir / "debugger.0bs";
            const fs::path tryDbgElan = debugDir / "debug.elan";
            const fs::path tryDbgObs = debugDir / "debug.0bs";
            if (fs::exists(tryLibElan)) {
                debugDriver = tryLibElan;
            } else if (fs::exists(tryLibObs)) {
                debugDriver = tryLibObs;
            } else if (fs::exists(tryDbgElan)) {
                debugDriver = tryDbgElan;
            } else if (fs::exists(tryDbgObs)) {
                debugDriver = tryDbgObs;
            }
        }
        std::unordered_set<std::string> visited; std::vector<Program> ordered;
        auto load_prog = [&](auto&& self, const fs::path& file) -> void {
            fs::path ap = fs::absolute(file); std::string key = ap.string(); if (visited.count(key)) return; visited.insert(key);
            LexerOptions lxopts; lxopts.enableDurations = true; lxopts.enableUnits = true; lxopts.enablePolyIdentifiers = true; lxopts.emitDocComments = true; lxopts.emitComments = false;
            Lexer lx(slurp_text(ap), lxopts); auto tokens = lx.lex(); Parser ps(std::move(tokens), key); Program prog = ps.parse();
            for (auto& a : prog.actions) a.sourcePath = key; for (auto& h : prog.hooks) h.sourcePath = key; for (auto& e : prog.entities) e.sourcePath = key; for (auto& g : prog.globals) g.sourcePath = key;
            for (const auto& imp : prog.imports) {
                fs::path ip = imp.path; if (!ip.has_extension()) { fs::path c1=ip; c1.replace_extension(".0bs"); fs::path c2=ip; c2.replace_extension(".obsecret"); if (fs::exists(c1)) ip=c1; else if (fs::exists(c2)) ip=c2; }
                bool loaded = false;
                if (ip.is_relative()) { fs::path tryLocal = ap.parent_path() / ip; if (fs::exists(tryLocal)) { self(self, tryLocal); loaded = true; } }
                else if (fs::exists(ip)) { self(self, ip); loaded = true; }
                if (!loaded) { if (auto mp = materialize_module_file(imp.path)) self(self, *mp); }
            }
            ordered.push_back(std::move(prog));
        };
        if (debugDriver) load_prog(load_prog, *debugDriver);
        load_prog(load_prog, fs::path(main_file));
        return run_programs(ordered, argc, argv, flags, out_error);
    } catch (const std::exception& ex) {
        set_error_string(out_error, ex.what());
        return 1;
    }
}

OB_API int ob_collect_files(const char* main_file,
                            void (*on_file)(const char* path, const char* contents, void* user),
                            void* user,
                            char** out_error) {
    try {
        std::unordered_set<std::string> visited;
        auto load = [&](auto&& self, const fs::path& f) -> void {
            fs::path ap = fs::absolute(f); std::string key = ap.string(); if (visited.count(key)) return; visited.insert(key);
            std::string contents = slurp_text(ap);
            if (on_file) on_file(key.c_str(), contents.c_str(), user);
            LexerOptions lxopts; lxopts.enableDurations = true; lxopts.enableUnits = true; lxopts.enablePolyIdentifiers = true; lxopts.emitDocComments = true; lxopts.emitComments = false;
            Lexer lx(contents, lxopts); auto tokens = lx.lex(); Parser ps(std::move(tokens), key); Program prog = ps.parse();
            for (const auto& imp : prog.imports) {
                fs::path ip = imp.path; if (!ip.has_extension()) { fs::path c1=ip; c1.replace_extension(".0bs"); fs::path c2=ip; c2.replace_extension(".obsecret"); if (fs::exists(c1)) ip=c1; else if (fs::exists(c2)) ip=c2; }
                bool loaded = false;
                if (ip.is_relative()) { fs::path tryLocal = ap.parent_path() / ip; if (fs::exists(tryLocal)) { self(self, tryLocal); loaded = true; } }
                else if (fs::exists(ip)) { self(self, ip); loaded = true; }
                if (!loaded) { if (auto mp = materialize_module_file(imp.path)) self(self, *mp); }
            }
        };
        load(load, fs::path(main_file));
        return 0;
    } catch (const std::exception& ex) {
        set_error_string(out_error, ex.what());
        return 1;
    }
}

OB_API int ob_run_embedded(const char* main_file,
                           const char* const* paths,
                           const char* const* contents,
                           int file_count,
                           int argc,
                           const char* argv[],
                           int flags,
                           char** out_error) {
    try {
        std::unordered_map<std::string, std::string> embedded;
        for (int i = 0; i < file_count; ++i) {
            if (paths && contents && paths[i] && contents[i]) {
                embedded.emplace(fs::absolute(fs::path(paths[i])).string(), contents[i]);
            }
        }
        auto load_text = [&](const std::string& key) -> std::string {
            auto it = embedded.find(fs::absolute(fs::path(key)).string());
            if (it != embedded.end()) return it->second;
            return slurp_text(fs::path(key));
        };
        auto resolve_import = [&](const std::string& basePath, const std::string& importName) -> std::optional<std::string> {
            fs::path base = fs::absolute(basePath);
            fs::path ip = importName;
            std::vector<fs::path> candidates;
            if (!ip.has_extension()) {
                fs::path c1 = ip; c1.replace_extension(".0bs");
                fs::path c2 = ip; c2.replace_extension(".obsecret");
                fs::path c3 = ip; c3.replace_extension(".elan");
                candidates = {c1, c2, c3};
            } else {
                candidates = {ip};
            }
            auto ends_with = [](const std::string& s, const std::string& suffix) {
                return s.size() >= suffix.size() && s.rfind(suffix) == s.size() - suffix.size();
            };
            for (const auto& candidate : candidates) {
                fs::path resolved = candidate.is_relative() ? base.parent_path() / candidate : candidate;
                const std::string key = fs::absolute(resolved).string();
                if (embedded.count(key) || fs::exists(resolved)) return key;
            }
            for (const auto& candidate : candidates) {
                const std::string tail = "/" + candidate.generic_string();
                for (const auto& kv : embedded) {
                    std::string normalized = fs::path(kv.first).generic_string();
                    if (ends_with(normalized, tail)) return kv.first;
                }
            }
            if (auto moduleFile = materialize_module_file(importName)) return moduleFile->string();
            return std::nullopt;
        };

        std::unordered_set<std::string> visited;
        std::vector<Program> ordered;
        std::function<void(const std::string&)> load_prog = [&](const std::string& file) {
            const std::string key = fs::absolute(fs::path(file)).string();
            if (visited.count(key)) return;
            visited.insert(key);
            LexerOptions lxopts;
            lxopts.enableDurations = true;
            lxopts.enableUnits = true;
            lxopts.enablePolyIdentifiers = true;
            lxopts.emitDocComments = true;
            lxopts.emitComments = false;
            Lexer lx(load_text(key), lxopts);
            auto tokens = lx.lex();
            Parser ps(std::move(tokens), key);
            Program prog = ps.parse();
            for (auto& a : prog.actions) a.sourcePath = key;
            for (auto& h : prog.hooks) h.sourcePath = key;
            for (auto& e : prog.entities) e.sourcePath = key;
            for (auto& g : prog.globals) g.sourcePath = key;
            for (const auto& imp : prog.imports) {
                if (imp.pluginGlob) continue;
                if (auto next = resolve_import(key, imp.path)) load_prog(*next);
            }
            ordered.push_back(std::move(prog));
        };

        load_prog(main_file ? main_file : "");
        return run_programs(ordered, argc, argv, flags, out_error);
    } catch (const std::exception& ex) {
        set_error_string(out_error, ex.what());
        return 1;
    }
}

OB_API void ob_free_string(char* s) { if (s) ::free(s); }

} // extern "C"
