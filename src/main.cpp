#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include "erelang/lexer.hpp"
#include "erelang/parser.hpp"
#include "erelang/runtime.hpp"
#include "erelang/ir.hpp"
#include "erelang/codegen_x64.hpp"
#include "erelang/plugins.hpp"
#include "erelang/creation_kit.hpp"
#include "erelang/erodsl/spec.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
std::filesystem::path locate_executable_root(int argc, char** argv) {
    namespace fs = std::filesystem;
    fs::path exeDir;
#ifdef _WIN32
    wchar_t buffer[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (len > 0) {
        exeDir = fs::path(std::wstring(buffer, len)).parent_path();
    }
#endif
    if (exeDir.empty()) {
        if (argc > 0 && argv && argv[0]) {
            exeDir = fs::absolute(fs::path(argv[0])).parent_path();
        }
        if (exeDir.empty()) {
            exeDir = fs::current_path();
        }
    }
    return exeDir;
}
} // namespace

static void static_check(const erelang::Program& prog) {
    using namespace erelang;
    bool hasMain=false;
    for (const auto& a : prog.actions) if (a.name=="main") hasMain=true;
    if (!hasMain && !prog.runTarget) std::cerr << "Warning: missing main action (no run target specified)\n";
}

int main(int argc, char** argv) {
    using namespace erelang;
    namespace fs = std::filesystem;

    const fs::path exeDir = locate_executable_root(argc, argv);

    if (argc >= 2) {
        const std::string_view command{argv[1]};
        if (command == "--creation-kit" || command == "creation-kit" || command == "--kit" || command == "kit") {
            CreationKitOptions opts;
            opts.runtimeRoot = exeDir;
            opts.userPluginRoot = ensure_user_plugin_root(&std::cerr);
            if (opts.userPluginRoot.empty()) {
                opts.userPluginRoot = exeDir / "plugins";
            }
            opts.input = &std::cin;
            opts.output = &std::cout;
            opts.log = &std::cerr;
            return run_creation_kit(opts);
        }
    }

    if (argc < 2) {
        std::cerr << "Usage: obc <file> [more ...]\n"
                     "       obc --emit-ir <file> [--out <path>]\n"
                     "       obc --emit-asm <file> [--out <path>]\n"
                     "       obc --build-native <file> [--out <path-to-exe>]\n";
        return 1;
    }

    try {
        bool emitIr = false;
        bool emitAsm = false;
        bool buildNative = false;
        std::optional<std::string> outputPath;
        std::vector<std::string> inputs;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i] ? std::string(argv[i]) : std::string();
            if (arg == "--emit-ir") {
                emitIr = true;
                continue;
            }
            if (arg == "--emit-asm") {
                emitAsm = true;
                continue;
            }
            if (arg == "--build-native") {
                buildNative = true;
                continue;
            }
            if (arg == "--out" || arg == "-o") {
                if (i + 1 >= argc) {
                    throw std::runtime_error("Missing output path after --out/-o");
                }
                outputPath = std::string(argv[++i]);
                continue;
            }
            inputs.push_back(arg);
        }
        const int modeCount = (emitIr ? 1 : 0) + (emitAsm ? 1 : 0) + (buildNative ? 1 : 0);
        if (modeCount > 1) {
            throw std::runtime_error("Use exactly one of --emit-ir, --emit-asm, or --build-native");
        }
        if ((emitIr || emitAsm || buildNative) && inputs.empty()) {
            throw std::runtime_error("No input file provided for emit mode");
        }

        std::vector<erodsl::DslSpec> languageSpecs;
        languageSpecs.push_back(erodsl::make_default_spec());
        std::unordered_map<std::string, std::size_t> extensionToLanguage;
        for (const auto& ext : languageSpecs.front().extensions) {
            extensionToLanguage[ext] = 0;
        }
        auto register_language_spec = [&](const erodsl::DslSpec& spec) {
            languageSpecs.push_back(spec);
            const std::size_t idx = languageSpecs.size() - 1;
            for (const auto& ext : languageSpecs[idx].extensions) {
                extensionToLanguage[ext] = idx;
            }
        };
        auto resolve_language = [&](const std::filesystem::path& path) -> const erodsl::DslSpec& {
            auto extStr = path.extension().string();
            if (!extStr.empty()) {
                auto normalized = erodsl::normalize_extension(extStr);
                if (auto it = extensionToLanguage.find(normalized); it != extensionToLanguage.end()) {
                    return languageSpecs[it->second];
                }
            }
            return languageSpecs.front();
        };

        auto load_file = [](const std::string& path)->std::string {
            std::ifstream in(path);
            if (!in) throw std::runtime_error("Failed to open: " + path);
            std::stringstream buffer; buffer << in.rdbuf();
            return buffer.str();
        };
        std::unordered_set<std::string> visited;
        std::vector<Program> ordered;
        auto lex_program = [&](std::string source, const erodsl::DslSpec& language) {
            LexerOptions opts = erodsl::build_lexer_options(language);
            Lexer lx(std::move(source), opts);
            auto tokens = lx.lex();
            erodsl::apply_keyword_aliases(language, tokens);
            return Parser(std::move(tokens)).parse();
        };
        std::function<void(const std::string&)> load_prog = [&](const std::string& file){
            fs::path p = fs::absolute(file);
            std::string key = p.string();
            if (visited.count(key)) return;
            visited.insert(key);
            std::string source = load_file(key);
            // Preprocess #include lines (simple, non-nested macro-like). Supports: #include "path" or <path>
            // We treat them as if the referenced file were imported (loaded first).
            {
                std::istringstream iss(source);
                std::string line;
                while (std::getline(iss, line)) {
                    std::string trimmed = line;
                    while (!trimmed.empty() && (trimmed.back()=='\r' || trimmed.back()=='\n')) trimmed.pop_back();
                    if (trimmed.rfind("#include", 0) == 0) {
                        size_t pos = trimmed.find_first_not_of(" \t", 8);
                        if (pos != std::string::npos) {
                            char open = trimmed[pos];
                            if (open=='"' || open=='<') {
                                char close = (open=='"') ? '"' : '>';
                                size_t end = trimmed.find(close, pos+1);
                                if (end != std::string::npos) {
                                    std::string inc = trimmed.substr(pos+1, end-(pos+1));
                                    for (auto& ch : inc) if (ch=='\\') ch='/';
                                    fs::path ip = inc;
                                    if (ip.is_relative()) ip = p.parent_path() / ip;
                                    load_prog(ip.string());
                                }
                            } else {
                                // bare path #include path/like/this
                                std::string inc = trimmed.substr(pos);
                                for (auto& ch : inc) if (ch=='\\') ch='/';
                                fs::path ip = inc;
                                if (ip.is_relative()) ip = p.parent_path() / ip;
                                load_prog(ip.string());
                            }
                        }
                    }
                }
            }
            const auto& language = resolve_language(p);
            Program prog = lex_program(std::move(source), language);
            // load imports first
            for (const auto& imp : prog.imports) {
                if (imp.pluginGlob) continue;
                std::string importPath = imp.path;
                for (auto& ch : importPath) {
                    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                }
                if (importPath.rfind("builtin/", 0) == 0 || importPath.rfind("builtin:", 0) == 0) {
                    continue;
                }
                fs::path ip = imp.path;
                if (ip.is_relative()) ip = p.parent_path() / ip;
                load_prog(ip.string());
            }
            // Debug: list entities found in this file
            std::cerr << "[loader] loaded " << key << " actions=" << prog.actions.size() << " entities=" << prog.entities.size() << " imports=" << prog.imports.size() << "\n";
            ordered.push_back(std::move(prog));
        };
        std::vector<Runtime::PluginRecord> pluginCallbacks;
        {
            auto manifests = discover_plugins(exeDir, &std::cerr);
            pluginCallbacks.reserve(manifests.size());
            for (const auto& manifest : manifests) {
                if (manifest.dslSpec) {
                    register_language_spec(*manifest.dslSpec);
                }
                for (const auto& script : manifest.scriptFiles) {
                    load_prog(script.string());
                }
                Runtime::PluginRecord rec;
                rec.id = manifest.id;
                rec.slug = manifest.baseDirectory.filename().string();
                rec.name = manifest.name;
                rec.version = manifest.version;
                rec.author = manifest.author;
                rec.target = manifest.target;
                rec.description = manifest.description;
                rec.dependencies = manifest.dependencies;
                rec.baseDirectory = manifest.baseDirectory;
                rec.manifestPath = manifest.manifestPath;
                rec.coreProperties = manifest.coreProperties;
                rec.dslSpec = manifest.dslSpec;
                rec.hookBindings = manifest.hookBindings;
                rec.onLoad = manifest.onLoadAction;
                rec.onUnload = manifest.onUnloadAction;
                rec.dataHook = manifest.dataHookAction;
                pluginCallbacks.push_back(std::move(rec));
            }
        }
        for (const auto& input : inputs) load_prog(input);
        if (inputs.empty()) {
            throw std::runtime_error("No input files provided");
        }
        // Merge in load order, run last file as main
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

        if (emitIr || emitAsm) {
            IRBuilder irBuilder;
            IRModule module = irBuilder.build(mainProg);
            std::string artifact;
            std::string suffix;
            if (emitIr) {
                artifact = ir_to_text(module);
                suffix = ".eir";
            } else {
                X64Codegen codegen;
                artifact = codegen.emit_nasm_win64(module);
                suffix = ".asm";
            }

            fs::path output;
            if (outputPath.has_value()) {
                output = *outputPath;
            } else {
                output = fs::path(inputs.back()).filename();
                output.replace_extension(suffix);
            }
            if (output.has_parent_path()) {
                std::error_code ec;
                fs::create_directories(output.parent_path(), ec);
            }
            std::ofstream outFile(output, std::ios::binary);
            if (!outFile) {
                throw std::runtime_error("Failed to open output: " + output.string());
            }
            outFile << artifact;
            outFile.close();
            std::cout << "Wrote " << output.string() << "\n";
            return 0;
        }

        if (buildNative) {
            IRBuilder irBuilder;
            IRModule module = irBuilder.build(mainProg);
            X64Codegen codegen;
            std::string asmText = codegen.emit_gas_win64_demo(module);

            fs::path exeOut;
            if (outputPath.has_value()) {
                exeOut = fs::path(*outputPath);
            } else {
                exeOut = fs::path(inputs.back()).filename();
                exeOut.replace_extension(".exe");
            }
            fs::path asmOut = exeOut;
            asmOut.replace_extension(".s");

            if (exeOut.has_parent_path()) {
                std::error_code ec;
                fs::create_directories(exeOut.parent_path(), ec);
            }
            if (asmOut.has_parent_path()) {
                std::error_code ec;
                fs::create_directories(asmOut.parent_path(), ec);
            }

            {
                std::ofstream asmFile(asmOut, std::ios::binary);
                if (!asmFile) {
                    throw std::runtime_error("Failed to open asm output: " + asmOut.string());
                }
                asmFile << asmText;
            }

            auto quote = [](const fs::path& p) {
                return std::string("\"") + p.string() + "\"";
            };
            const std::string cmd = std::string("gcc ") + quote(asmOut) + " -o " + quote(exeOut);
            std::cout << "[native] " << cmd << "\n";
            const int rc = std::system(cmd.c_str());
            if (rc != 0) {
                throw std::runtime_error("Native build failed via gcc (exit " + std::to_string(rc) + ")");
            }
            std::cout << "Wrote " << exeOut.string() << "\n";
            return 0;
        }

        static_check(mainProg);
        // If there is no entry point in the final program, treat as module-only invocation
        bool hasMain = false; for (const auto& a : mainProg.actions) if (a.name=="main") { hasMain = true; break; }
        if (!mainProg.runTarget && !hasMain) {
            std::cerr << "Info: no entry point found (module-only). Exiting.\n";
            return 0;
        }
        Runtime rt;
        rt.register_plugins(std::move(pluginCallbacks));
        return rt.run(mainProg);
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 1;
    }
}
