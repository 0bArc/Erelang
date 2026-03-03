// SPDX-License-Identifier: Apache-2.0
// Runtime implementation: drives program execution, action dispatch,
// global initialization, and GUI event hooks (Windows only).
// Heavy translation unit – intentionally keeps broad includes local.

#include "erelang/cabi.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>

#include <algorithm>
#include <cctype>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shellapi.h> // for ShellExecuteW
#endif

// Bring in the language pipeline headers before using the types below
#include "erelang/lexer.hpp"
#include "erelang/parser.hpp"
#include "erelang/typechecker.hpp"
#include "erelang/optimizer.hpp"
#include "erelang/symboltable.hpp"
#include "erelang/modules.hpp"
#include "erelang/version.hpp"
#include "erelang/policy.hpp"
// Ensure Runtime is visible before any C-ABI definitions that reference it
#include "erelang/runtime.hpp"
using namespace erelang;
namespace fs = std::filesystem;

static std::string slurp_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss; ss << in.rdbuf();
    return ss.str();
}

// Attempt to find an import in registered modules and materialize it to a temp file
static std::optional<fs::path> materialize_module_file(const std::string& importName) {
    std::string norm = importName;
    for (auto& ch : norm) if (ch == '\\') ch = '/';
    std::vector<std::string> names;
    if (norm.find('.') == std::string::npos) {
        names = {norm + ".0bs", norm + ".obsecret"};
    } else {
        names = {norm};
    }
    const auto mods = get_registered_modules();
    auto ends_with = [](const std::string& s, const std::string& suf){ return s.size()>=suf.size() && s.rfind(suf)==s.size()-suf.size(); };
    for (const auto& m : mods) {
        for (size_t i = 0; i < m.file_count; ++i) {
            const ModuleFile& f = m.files[i];
            if (!f.name || !f.contents) continue;
            std::string fn = f.name; for (auto& ch : fn) if (ch == '\\') ch = '/';
            for (const auto& nm : names) {
                std::string tail = std::string("/") + nm;
                if (ends_with(fn, tail)) {
                    fs::path out = fs::temp_directory_path() / "erelang_modules" / fn;
                    std::error_code ec; fs::create_directories(out.parent_path(), ec);
                    std::ofstream o(out, std::ios::binary);
                    if (!o) break;
                    o << f.contents;
                    o.close();
                    return out;
                }
            }
        }
    }
    return std::nullopt;
}

static std::string trim_copy(std::string_view value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return std::string{value.substr(begin, end - begin + 1)};
}

constexpr std::string_view kBuiltinAliasPrefix = "__builtin__:";

static std::string lowercase_copy(std::string value) {
    for (auto& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

static std::optional<std::string> resolve_builtin_module_method(
    const Program& program,
    std::string_view alias,
    std::string_view method) {
    if (alias.empty() || method.empty()) {
        return std::nullopt;
    }

    std::string methodName = lowercase_copy(std::string(method));
    for (const auto& importDecl : program.imports) {
        if (!importDecl.alias || *importDecl.alias != alias || importDecl.path.empty()) {
            continue;
        }

        std::string normalizedPath = importDecl.path;
        for (auto& ch : normalizedPath) if (ch == '\\') ch = '/';
        normalizedPath = lowercase_copy(std::move(normalizedPath));

        const bool isFs = (normalizedPath == "builtin/fs" || normalizedPath == "builtin/erefs");
        const bool isPath = (normalizedPath == "builtin/path" || normalizedPath == "builtin/erepath");
        if (!isFs && !isPath) {
            continue;
        }

        if (isFs) {
            if (methodName == "read") return std::string("read_text");
            if (methodName == "write") return std::string("write_text");
            if (methodName == "append") return std::string("append_text");
            if (methodName == "exists") return std::string("file_exists");
            if (methodName == "mkdir") return std::string("mkdirs");
            if (methodName == "copy") return std::string("copy_file");
            if (methodName == "move") return std::string("move_file");
            if (methodName == "remove") return std::string("delete_file");
            if (methodName == "list") return std::string("list_files");
            if (methodName == "cwd") return std::string("cwd");
            if (methodName == "chdir") return std::string("chdir");
            if (methodName == "join") return std::string("path_join");
            if (methodName == "parent") return std::string("path_dirname");
            if (methodName == "name") return std::string("path_basename");
            if (methodName == "ext") return std::string("path_ext");
        }

        if (isPath) {
            if (methodName == "join") return std::string("path_join");
            if (methodName == "parent") return std::string("path_dirname");
            if (methodName == "name") return std::string("path_basename");
            if (methodName == "ext") return std::string("path_ext");
            if (methodName == "exists") return std::string("file_exists");
        }
    }

    return std::nullopt;
}

static void bind_builtin_module_aliases(const Program& program, std::unordered_map<std::string, std::string>& vars) {
    auto bind_alias = [&](const std::string& alias, const char* method, const char* builtin) {
        vars[alias + "." + method] = std::string(kBuiltinAliasPrefix) + builtin;
    };

    for (const auto& importDecl : program.imports) {
        if (!importDecl.alias || importDecl.path.empty()) {
            continue;
        }

        std::string normalizedPath = importDecl.path;
        for (auto& ch : normalizedPath) if (ch == '\\') ch = '/';
        normalizedPath = lowercase_copy(std::move(normalizedPath));
        const std::string& alias = *importDecl.alias;

        const bool isFs = (normalizedPath == "builtin/fs" || normalizedPath == "builtin/erefs");
        const bool isPath = (normalizedPath == "builtin/path" || normalizedPath == "builtin/erepath");
        if (!isFs && !isPath) {
            continue;
        }

        if (isFs) {
            bind_alias(alias, "read", "read_text");
            bind_alias(alias, "write", "write_text");
            bind_alias(alias, "append", "append_text");
            bind_alias(alias, "exists", "file_exists");
            bind_alias(alias, "mkdir", "mkdirs");
            bind_alias(alias, "copy", "copy_file");
            bind_alias(alias, "move", "move_file");
            bind_alias(alias, "remove", "delete_file");
            bind_alias(alias, "list", "list_files");
            bind_alias(alias, "cwd", "cwd");
            bind_alias(alias, "chdir", "chdir");
            bind_alias(alias, "join", "path_join");
            bind_alias(alias, "parent", "path_dirname");
            bind_alias(alias, "name", "path_basename");
            bind_alias(alias, "ext", "path_ext");
        }

        if (isPath) {
            bind_alias(alias, "join", "path_join");
            bind_alias(alias, "parent", "path_dirname");
            bind_alias(alias, "name", "path_basename");
            bind_alias(alias, "ext", "path_ext");
            bind_alias(alias, "exists", "file_exists");
        }
    }
}

static std::string join_strings(std::vector<std::string> items, char separator = ',') {
    if (items.empty()) {
        return {};
    }
    std::sort(items.begin(), items.end());
    std::ostringstream oss;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i) {
            oss << separator;
        }
        oss << items[i];
    }
    return oss.str();
}

static std::pair<std::string, std::string> split_core_query(const std::string& query) {
    const auto trimmed = trim_copy(query);
    if (trimmed.empty()) {
        return {"", ""};
    }
    auto pos = trimmed.find(':');
    if (pos == std::string::npos) pos = trimmed.find('.');
    if (pos == std::string::npos) pos = trimmed.find('/');
    if (pos == std::string::npos) {
        return {"", trimmed};
    }
    auto left = trim_copy(trimmed.substr(0, pos));
    auto right = trim_copy(trimmed.substr(pos + 1));
    return {left, right};
}

// C ABI exported symbols moved to cabi_exports.cpp (shared DLL only)
// runtime.hpp already included above
#include <iostream>
#include <chrono>
#include <stdexcept>
#include <cstdlib>
#include <cmath>
#include <random>
#include <filesystem>
#include <sstream>
#include <fstream>
#include <mutex>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winreg.h>
#include <commctrl.h>
#include <rpc.h>
#endif

namespace erelang {

namespace {

[[nodiscard]] std::string lowercase_ascii_copy(std::string_view value) {
    std::string result{value};
    for (char& ch : result) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return result;
}

[[nodiscard]] const std::vector<std::string>& hook_actions_for(const Runtime::PluginRecord& plugin, std::string_view hookName) {
    static const std::vector<std::string> kEmpty;
    const std::string key = lowercase_ascii_copy(hookName);
    auto it = plugin.hookBindings.find(key);
    if (it != plugin.hookBindings.end()) {
        return it->second;
    }
    return kEmpty;
}

void join_threads(std::vector<std::thread>& threads) {
    for (auto& th : threads) {
        if (th.joinable()) {
            th.join();
        }
    }
}

std::string join_preserving_order(const std::vector<std::string>& values, char separator = ',') {
    if (values.empty()) {
        return {};
    }
    std::ostringstream oss;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            oss << separator;
        }
        oss << values[i];
    }
    return oss.str();
}

} // namespace

Runtime::Runtime() {}

void Runtime::register_plugins(std::vector<PluginRecord> plugins) {
    pluginRecords_ = std::move(plugins);
}

void Runtime::initialize_environment(const Program& program) {
    Env rootEnv; // Declare rootEnv
    std::cerr << "[debug] Initializing environment and binding aliases...\n";
    bind_builtin_module_aliases(program, rootEnv.vars);
    std::cerr << "[debug] Aliases after binding: \n";
    for (const auto& [key, value] : rootEnv.vars) {
        std::cerr << key << " -> " << value << "\n";
    }
}

std::vector<std::string> Runtime::s_cliArgs;
void Runtime::set_cli_args(const std::vector<std::string>& args) { s_cliArgs = args; }

static int64_t to_int(const std::string& s) {
    try { return std::stoll(s); } catch (...) { return 0; }
}

static std::filesystem::path path_from_u8(const std::string& s) {
    const auto* first = reinterpret_cast<const char8_t*>(s.data());
    const auto* last = first + s.size();
    return std::filesystem::path(std::u8string(first, last));
}

static bool is_int_string(const std::string& s);
static bool is_truthy(const std::string& v);

static double to_double(const std::string& s) {
    try { return std::stod(s); } catch (...) { return 0.0; }
}

static bool is_float_string(const std::string& s) {
    if (s.empty()) return false;
    // If it contains a decimal point or exponent, and parses as a double, treat as float
    bool hasPoint = false;
    bool hasExp = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '.' ) hasPoint = true;
        if (c == 'e' || c == 'E') hasExp = true;
        if ((c == '+' || c == '-') && i != 0 && !(s[i-1]=='e' || s[i-1]=='E')) return false;
        if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == '+' || c == '-' || c=='e' || c=='E')) return false;
    }
    if (!hasPoint && !hasExp) return false;
    try { std::size_t idx; std::stod(s, &idx); return idx == s.size(); } catch (...) { return false; }
}

// Simple dynamic containers as built-ins: lists and dicts
int g_nextListId = 1;
std::unordered_map<int, std::vector<std::string>> g_lists;
int g_nextDictId = 1;
std::unordered_map<int, std::unordered_map<std::string, std::string>> g_dicts;

#ifdef _WIN32
// Portable integer rounding helper (avoids std::lround issues on some MinGW setups)
static inline int iround(double x) {
    return (int)((x >= 0.0) ? (x + 0.5) : (x - 0.5));
}

struct LayoutState {
    bool enabled = true;
    int padX = 16;
    int padY = 16;
    int spacingX = 8;
    int spacingY = 8;
    int lineH = 28;
    int defaultW = 140;
    int widthOverride = 0;
    int nextX = 16;
    int nextY = 16;
    bool sameLine = false;
    int lastX = 16;
    int lastY = 16;
    int lastW = 140;
    int lastH = 28;
};

struct LayoutSlot {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

struct NativeWin {
    HWND hwnd{};
    std::unordered_map<int, std::string> idToAction;
    std::unordered_map<int, std::string> idToName;
    std::unordered_map<std::string, int> nameToId;
    std::unordered_map<int, HWND> idToHwnd;
    int nextId = 1000;
    HFONT hFont{};
    double scale = 1.0;
    LayoutState layout;
};
static std::unordered_map<HWND, NativeWin*> g_hwndMap;
// Note: kObsWinClass is a base name; we build a unique name per-process using the HINSTANCE value
static const wchar_t* kObsWinClass = L"OBSK_GUI_CLS";
static std::unordered_map<int, NativeWin*> g_handleMap;
static int g_nextHandle = 1;
static std::string g_eventActionName; // optional centralized event handler action
static NativeWin* g_currentEventWin = nullptr; // fallback for built-ins
static HFONT g_segoeFont = nullptr;
static HBRUSH g_bgBrush = nullptr;

// Forward declarations for layout and window helpers defined later in this TU.
static void layout_initialize(NativeWin* nw);
static HFONT create_font_for_scale(double scale);
static void apply_font_for(HWND hwnd, HFONT f);
static void ensure_window_class_registered(HINSTANCE hInst);

// WNDCLASS state and helpers
static std::once_flag g_classOnce;        // guard against multi-threaded registration
static ATOM g_classAtom = 0;              // stores class atom on successful registration
static std::wstring g_obsWinClassName;    // unique class name for this module/process

// Simple logger for Win32 GUI subsystem
static void winlog(const char* msg) {
    // Keep logging lightweight; visible in console builds
    std::cerr << "[win] " << msg << std::endl;
}

// Forward declaration for window proc
static LRESULT CALLBACK ObsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

static NativeWin* parse_window_handle(const std::string& s) {
    if (s.rfind("win:", 0) == 0) {
        int id = to_int(s.substr(4));
        auto it = g_handleMap.find(id);
        if (it != g_handleMap.end()) {
            return it->second;
        }
    }
    if (g_currentEventWin) {
        return g_currentEventWin;
    }
    return nullptr;
}

static std::string create_native_window(const Runtime* selfConst, const std::string& title, int width, int height) {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    ensure_window_class_registered(hInst);
    std::ostringstream log;
    log << "win_window_create title='" << title << "' size=" << width << "x" << height;
    winlog(log.str().c_str());
    std::wstring wt(title.begin(), title.end());
    HWND hwnd = CreateWindowExW(0, g_obsWinClassName.c_str(), wt.c_str(), WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, width, height, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) {
        DWORD err = GetLastError();
        std::ostringstream es; es << "win_window_create FAILED error=" << err; winlog(es.str().c_str());
        return {};
    }
    auto* nw = new NativeWin();
    nw->hwnd = hwnd;
    layout_initialize(nw);
    UINT dpi = 96;
    HMODULE hUser = GetModuleHandleW(L"user32.dll");
    auto pGetDpiForWindow = reinterpret_cast<UINT (WINAPI*)(HWND)>(GetProcAddress(hUser, "GetDpiForWindow"));
    if (pGetDpiForWindow) {
        dpi = pGetDpiForWindow(hwnd);
    }
    nw->scale = dpi / 96.0;
    nw->hFont = create_font_for_scale(nw->scale);
    g_hwndMap[hwnd] = nw;
    int hid = g_nextHandle++;
    g_handleMap[hid] = nw;
    auto* self = const_cast<Runtime*>(selfConst);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    apply_font_for(hwnd, nw->hFont);
    int sw = iround(width * nw->scale);
    int sh = iround(height * nw->scale);
    SetWindowPos(hwnd, nullptr, 0, 0, sw, sh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    UpdateWindow(hwnd);
    std::ostringstream out; out << "win_window_create -> hid=" << hid << " scale=" << nw->scale << " size=" << sw << "x" << sh;
    winlog(out.str().c_str());
    return std::string("win:") + std::to_string(hid);
}

static void run_message_loop(const std::string& actionName) {
    g_eventActionName = actionName;
    winlog("enter win_loop");
    MSG msg;
    int gm;
    while ((gm = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (gm == 0) {
        winlog("win_loop: WM_QUIT received");
    } else if (gm == -1) {
        DWORD err = GetLastError();
        std::ostringstream em; em << "win_loop: GetMessage failed, error=" << err; winlog(em.str().c_str());
    }
    g_eventActionName.clear();
}

struct EuiCommand {
    std::string keyword;
    std::unordered_map<std::string, std::string> attrs;
    std::vector<std::string> positional;
    size_t line = 0;
};

static std::string ascii_lower_copy(std::string v) {
    for (char& ch : v) {
        ch = static_cast<char>(::tolower(static_cast<unsigned char>(ch)));
    }
    return v;
}

static std::string_view trim_view(std::string_view v) {
    while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front()))) {
        v.remove_prefix(1);
    }
    while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) {
        v.remove_suffix(1);
    }
    return v;
}

static std::vector<std::string> eui_tokenize_line(std::string_view line) {
    std::vector<std::string> tokens;
    std::string current;
    bool inQuote = false;
    char quoteChar = '\0';
    for (size_t i = 0; i < line.size(); ++i) {
        char c = static_cast<char>(line[i]);
        if (!inQuote && c == '#') {
            break;
        }
        if (inQuote) {
            if (c == quoteChar) {
                inQuote = false;
            } else if (c == '\\' && i + 1 < line.size()) {
                char next = static_cast<char>(line[++i]);
                switch (next) {
                    case 'n': current.push_back('\n'); break;
                    case 't': current.push_back('\t'); break;
                    case '\\': current.push_back('\\'); break;
                    case '"': current.push_back('"'); break;
                    case '\'': current.push_back('\''); break;
                    default: current.push_back(next); break;
                }
            } else {
                current.push_back(c);
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            inQuote = true;
            quoteChar = c;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }
    return tokens;
}

static bool parse_int_pair(const std::string& text, int& a, int& b) {
    auto pos = text.find(',');
    if (pos == std::string::npos) {
        int val = to_int(text);
        a = val;
        b = val;
        return true;
    }
    std::string first = text.substr(0, pos);
    std::string second = text.substr(pos + 1);
    a = to_int(first);
    b = to_int(second);
    return true;
}

static int register_named_control(NativeWin* nw, const std::string& idName) {
    if (!nw) {
        return 0;
    }
    int id = nw->nextId++;
    nw->idToName[id] = idName;
    nw->nameToId[idName] = id;
    return id;
}

static void assign_action_if_any(NativeWin* nw, int id, const std::string& actionName, const std::string& fallbackAction) {
    if (!nw || id == 0) {
        return;
    }
    if (!actionName.empty()) {
        nw->idToAction[id] = actionName;
    } else if (!fallbackAction.empty()) {
        nw->idToAction[id] = fallbackAction;
    }
}

static void redraw_native_window(NativeWin* nw) {
    if (!nw) {
        return;
    }
    RedrawWindow(nw->hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

static void log_control_creation(const char* kind, const std::string& idName, const LayoutSlot& slot) {
    std::ostringstream ss;
    ss << kind;
    if (!idName.empty()) {
        ss << " id='" << idName << "'";
    }
    ss << " at (" << slot.x << "," << slot.y << ") size=" << slot.w << "x" << slot.h;
    winlog(ss.str().c_str());
}

static void create_label_control(NativeWin* nw, const LayoutSlot& slot, const std::string& text) {
    if (!nw) {
        return;
    }
    log_control_creation("ui_label", text, slot);
    std::wstring wtxt(text.begin(), text.end());
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    auto scale = [&](int v) { return iround(v * nw->scale); };
    HWND ch = CreateWindowExW(0, L"STATIC", wtxt.c_str(), WS_VISIBLE | WS_CHILD | SS_LEFT | SS_NOPREFIX,
        scale(slot.x), scale(slot.y), scale(slot.w), scale(slot.h), nw->hwnd, nullptr, hInst, nullptr);
    apply_font_for(ch, nw->hFont);
    redraw_native_window(nw);
}

static int create_button_control(NativeWin* nw, const LayoutSlot& slot, const std::string& idName, const std::string& text, bool defaultButton) {
    if (!nw) {
        return 0;
    }
    log_control_creation("ui_button", idName, slot);
    std::wstring wtxt(text.begin(), text.end());
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    auto scale = [&](int v) { return iround(v * nw->scale); };
    int id = register_named_control(nw, idName);
    DWORD style = WS_TABSTOP | WS_VISIBLE | WS_CHILD | (defaultButton ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON);
    HWND ch = CreateWindowExW(0, L"BUTTON", wtxt.c_str(), style,
        scale(slot.x), scale(slot.y), scale(slot.w), scale(slot.h), nw->hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
    nw->idToHwnd[id] = ch;
    apply_font_for(ch, nw->hFont);
    redraw_native_window(nw);
    return id;
}

static int create_checkbox_control(NativeWin* nw, const LayoutSlot& slot, const std::string& idName, const std::string& text, bool checked) {
    if (!nw) {
        return 0;
    }
    log_control_creation("ui_checkbox", idName, slot);
    std::wstring wtxt(text.begin(), text.end());
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    auto scale = [&](int v) { return iround(v * nw->scale); };
    int id = register_named_control(nw, idName);
    HWND ch = CreateWindowExW(0, L"BUTTON", wtxt.c_str(), WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
        scale(slot.x), scale(slot.y), scale(slot.w), scale(slot.h), nw->hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
    if (checked) {
        SendMessageW(ch, BM_SETCHECK, BST_CHECKED, 0);
    }
    nw->idToHwnd[id] = ch;
    apply_font_for(ch, nw->hFont);
    redraw_native_window(nw);
    return id;
}

static int create_radio_control(NativeWin* nw, const LayoutSlot& slot, const std::string& idName, const std::string& text, bool groupStart, bool checked) {
    if (!nw) {
        return 0;
    }
    log_control_creation("ui_radio", idName, slot);
    std::wstring wtxt(text.begin(), text.end());
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    auto scale = [&](int v) { return iround(v * nw->scale); };
    int id = register_named_control(nw, idName);
    DWORD style = WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_AUTORADIOBUTTON;
    if (groupStart) {
        style |= WS_GROUP;
    }
    HWND ch = CreateWindowExW(0, L"BUTTON", wtxt.c_str(), style,
        scale(slot.x), scale(slot.y), scale(slot.w), scale(slot.h), nw->hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
    if (checked) {
        SendMessageW(ch, BM_SETCHECK, BST_CHECKED, 0);
    }
    nw->idToHwnd[id] = ch;
    apply_font_for(ch, nw->hFont);
    redraw_native_window(nw);
    return id;
}

static int create_slider_control(NativeWin* nw, const LayoutSlot& slot, const std::string& idName, int minv, int maxv, int value) {
    if (!nw) {
        return 0;
    }
    log_control_creation("ui_slider", idName, slot);
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    auto scale = [&](int v) { return iround(v * nw->scale); };
    int id = register_named_control(nw, idName);
    HWND ch = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_VISIBLE | WS_CHILD | TBS_AUTOTICKS,
        scale(slot.x), scale(slot.y), scale(slot.w), scale(slot.h), nw->hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
    SendMessageW(ch, TBM_SETRANGEMIN, TRUE, minv);
    SendMessageW(ch, TBM_SETRANGEMAX, TRUE, maxv);
    SendMessageW(ch, TBM_SETPOS, TRUE, value);
    nw->idToHwnd[id] = ch;
    apply_font_for(ch, nw->hFont);
    redraw_native_window(nw);
    return id;
}

static int create_textbox_control(NativeWin* nw, const LayoutSlot& slot, const std::string& idName, const std::string& text) {
    if (!nw) {
        return 0;
    }
    log_control_creation("ui_textbox", idName, slot);
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    auto scale = [&](int v) { return iround(v * nw->scale); };
    int id = register_named_control(nw, idName);
    HWND ch = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_VISIBLE | WS_CHILD | ES_LEFT | ES_AUTOHSCROLL,
        scale(slot.x), scale(slot.y), scale(slot.w), scale(slot.h), nw->hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
    if (!text.empty()) {
        std::wstring wtxt(text.begin(), text.end());
        SetWindowTextW(ch, wtxt.c_str());
    }
    nw->idToHwnd[id] = ch;
    apply_font_for(ch, nw->hFont);
    redraw_native_window(nw);
    return id;
}

static void create_separator_control(NativeWin* nw, const LayoutSlot& slot, int thickness) {
    if (!nw) {
        return;
    }
    log_control_creation("ui_separator", "", slot);
    if (thickness <= 0) {
        thickness = 2;
    }
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    auto scale = [&](int v) { return iround(v * nw->scale); };
    int scaledThickness = std::max(1, scale(thickness));
    HWND ch = CreateWindowExW(0, L"STATIC", L"", WS_VISIBLE | WS_CHILD | SS_ETCHEDHORZ,
        scale(slot.x), scale(slot.y), scale(slot.w), scaledThickness, nw->hwnd, nullptr, hInst, nullptr);
    apply_font_for(ch, nw->hFont);
    redraw_native_window(nw);
}

static std::optional<std::vector<EuiCommand>> parse_eui_document(const std::string& text, std::string& error) {
    std::vector<EuiCommand> commands;
    std::istringstream stream(text);
    std::string line;
    size_t lineNo = 0;
    while (std::getline(stream, line)) {
        ++lineNo;
        auto trimmed = trim_view(line);
        if (trimmed.empty()) {
            continue;
        }
        if (!trimmed.empty() && trimmed.front() == '#') {
            continue;
        }
        auto tokens = eui_tokenize_line(trimmed);
        if (tokens.empty()) {
            continue;
        }
        EuiCommand cmd;
        cmd.keyword = ascii_lower_copy(tokens.front());
        cmd.line = lineNo;
        for (size_t i = 1; i < tokens.size(); ++i) {
            const std::string& tok = tokens[i];
            auto eq = tok.find('=');
            if (eq == std::string::npos) {
                cmd.positional.push_back(tok);
                continue;
            }
            std::string key = ascii_lower_copy(tok.substr(0, eq));
            std::string value = tok.substr(eq + 1);
            cmd.attrs[std::move(key)] = value;
        }
        commands.push_back(std::move(cmd));
    }
    if (commands.empty()) {
        error = "erelang.ui document does not contain any commands";
        return std::nullopt;
    }
    return commands;
}

static HFONT create_font_for_scale(double scale) {
    if (!g_segoeFont) {
        LOGFONTW lf{}; lf.lfHeight = -12; lstrcpyW(lf.lfFaceName, L"Segoe UI");
        g_segoeFont = CreateFontIndirectW(&lf);
    }
    int base = 12;
    int h = -iround(base * scale);
    LOGFONTW lf{}; lf.lfHeight = h; lstrcpyW(lf.lfFaceName, L"Segoe UI");
    HFONT f = CreateFontIndirectW(&lf);
    if (!f) return g_segoeFont; // fallback
    return f;
}

static void apply_font_for(HWND hwnd, HFONT f) {
    if (f) SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
}

static void layout_initialize(NativeWin* nw) {
    if (!nw) return;
    LayoutState ls{};
    ls.enabled = true;
    ls.padX = 16;
    ls.padY = 16;
    ls.spacingX = 8;
    ls.spacingY = 8;
    ls.lineH = 28;
    ls.defaultW = 140;
    ls.widthOverride = 0;
    ls.nextX = ls.padX;
    ls.nextY = ls.padY;
    ls.sameLine = false;
    ls.lastX = ls.padX;
    ls.lastY = ls.padY;
    ls.lastW = ls.defaultW;
    ls.lastH = ls.lineH;
    nw->layout = ls;
}

static void layout_reset(NativeWin* nw) {
    if (!nw) return;
    auto& ls = nw->layout;
    ls.widthOverride = 0;
    ls.sameLine = false;
    ls.nextX = ls.padX;
    ls.nextY = ls.padY;
    ls.lastX = ls.padX;
    ls.lastY = ls.padY;
    ls.lastW = ls.defaultW;
    ls.lastH = ls.lineH;
}

static void layout_configure(NativeWin* nw, int padX, int padY, int lineH, int spacingX, int spacingY, int defaultW) {
    if (!nw) return;
    auto& ls = nw->layout;
    ls.enabled = true;
    if (padX >= 0) ls.padX = padX;
    if (padY >= 0) ls.padY = padY;
    if (lineH > 0) ls.lineH = lineH;
    if (spacingX >= 0) ls.spacingX = spacingX;
    if (spacingY >= 0) ls.spacingY = spacingY;
    if (defaultW > 0) ls.defaultW = defaultW;
    layout_reset(nw);
}

static LayoutSlot layout_take_slot(NativeWin* nw, int reqW, int reqH) {
    if (!nw) return {0,0,reqW,reqH};
    auto& ls = nw->layout;
    if (!ls.enabled) {
        int w = (reqW > 0) ? reqW : 140;
        int h = (reqH > 0) ? reqH : 28;
        return {0, 0, w, h};
    }
    int w = (reqW > 0) ? reqW : ls.defaultW;
    if (ls.widthOverride > 0) {
        w = ls.widthOverride;
        ls.widthOverride = 0;
    }
    int h = (reqH > 0) ? reqH : ls.lineH;

    int x = ls.sameLine ? (ls.lastX + ls.lastW + ls.spacingX) : ls.padX;
    int y = ls.sameLine ? ls.lastY : ls.nextY;
    ls.sameLine = false;

    ls.lastX = x;
    ls.lastY = y;
    ls.lastW = w;
    ls.lastH = h;

    ls.nextX = ls.padX;
    ls.nextY = y + h + ls.spacingY;

    return {x, y, w, h};
}

static void layout_signal_same_line(NativeWin* nw) {
    if (!nw) return;
    nw->layout.sameLine = true;
}

static void layout_force_newline(NativeWin* nw) {
    if (!nw) return;
    auto& ls = nw->layout;
    ls.sameLine = false;
    ls.nextX = ls.padX;
    ls.nextY = ls.lastY + ls.lastH + ls.spacingY;
}

static void layout_push_width(NativeWin* nw, int width) {
    if (!nw) return;
    nw->layout.widthOverride = (width > 0) ? width : 0;
}

static void layout_pop_width(NativeWin* nw) {
    if (!nw) return;
    nw->layout.widthOverride = 0;
}

static void layout_notify_manual(NativeWin* nw, int x, int y, int w, int h) {
    if (!nw) return;
    auto& ls = nw->layout;
    if (!ls.enabled) return;
    ls.sameLine = false;
    ls.widthOverride = 0;
    ls.lastX = x;
    ls.lastY = y;
    ls.lastW = (w > 0) ? w : ls.defaultW;
    ls.lastH = (h > 0) ? h : ls.lineH;
    ls.nextX = ls.padX;
    ls.nextY = ls.lastY + ls.lastH + ls.spacingY;
}

static void ensure_window_class_registered(HINSTANCE hInst) {
    // Ensure registration happens exactly once across threads
    std::call_once(g_classOnce, [hInst]() {
        // Build a unique class name per module by appending the HINSTANCE value
        std::wostringstream oss;
        oss << kObsWinClass << L"_" << std::hex << reinterpret_cast<uintptr_t>(hInst);
        g_obsWinClassName = oss.str();

        // Initialize only the common controls we use:
        // - ICC_STANDARD_CLASSES: button, static, edit
        // - ICC_BAR_CLASSES: trackbar (slider)
        INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
        if (!InitCommonControlsEx(&icc)) {
            winlog("InitCommonControlsEx failed (continuing; controls may still work).");
        }

        // Fully zero-initialize and use WNDCLASSEXW for extra fields.
        WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW; // redraw on horizontal/vertical resize; adjust as needed
        wc.lpfnWndProc = ObsWndProc;        // dedicated window proc
        wc.cbClsExtra = 0; wc.cbWndExtra = 0; wc.hInstance = hInst;
        wc.hIcon = nullptr; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszMenuName = nullptr; wc.lpszClassName = g_obsWinClassName.c_str();
        wc.hIconSm = nullptr;

        g_classAtom = RegisterClassExW(&wc);
        if (!g_classAtom) {
            DWORD err = GetLastError();
            if (err == ERROR_CLASS_ALREADY_EXISTS) {
                // Class exists (possibly registered by another module); treat as success
                winlog("RegisterClassExW: class already exists; using existing class.");
            } else {
                // Hard failure: log and avoid silent behaviour
                std::ostringstream em; em << "RegisterClassExW failed, error=" << err;
                winlog(em.str().c_str());
            }
        } else {
            winlog("Window class registered successfully.");
        }
    });
}

// Dedicated window procedure with consistent LRESULT returns and guarded casts.
static LRESULT CALLBACK ObsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CTLCOLORSTATIC: {
            // Transparent background for static controls; stock brushes must not be deleted
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetBkMode(hdc, TRANSPARENT);
            if (!g_bgBrush) g_bgBrush = reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
            return reinterpret_cast<LRESULT>(g_bgBrush);
        }
        case WM_ERASEBKGND:
            // Reduce flicker by handling background erase ourselves
            return 1;
        case WM_DPICHANGED: {
            auto it = g_hwndMap.find(hwnd);
            if (it != g_hwndMap.end()) {
                NativeWin* nw = it->second;
                UINT dpiX = LOWORD(wParam);
                nw->scale = dpiX / 96.0;
                if (nw->hFont) { DeleteObject(nw->hFont); nw->hFont = nullptr; }
                nw->hFont = create_font_for_scale(nw->scale);
                for (const auto& kv : nw->idToHwnd) apply_font_for(kv.second, nw->hFont);
                if (lParam) {
                    RECT* prcNew = reinterpret_cast<RECT*>(lParam);
                    SetWindowPos(hwnd, nullptr, prcNew->left, prcNew->top, prcNew->right - prcNew->left, prcNew->bottom - prcNew->top,
                                 SWP_NOZORDER | SWP_NOACTIVATE);
                }
            }
            return 0;
        }
        case WM_HSCROLL: {
            auto* self = reinterpret_cast<Runtime*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            HWND ctrl = reinterpret_cast<HWND>(lParam);
            if (self && ctrl) {
                int id = GetDlgCtrlID(ctrl);
                auto it = g_hwndMap.find(hwnd);
                if (it != g_hwndMap.end()) self->handle_gui_click(id, it->second);
            }
            return 0;
        }
        case WM_COMMAND: {
            if (HIWORD(wParam) == BN_CLICKED) {
                auto* self = reinterpret_cast<Runtime*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                int id = LOWORD(wParam);
                auto it = g_hwndMap.find(hwnd);
                if (self && it != g_hwndMap.end()) self->handle_gui_click(id, it->second);
                return 0;
            }
            break;
        }
        case WM_DESTROY: {
            // Clean up and remove from maps; do not delete stock brushes
            auto it = g_hwndMap.find(hwnd);
            if (it != g_hwndMap.end()) {
                NativeWin* nw = it->second;
                if (nw) {
                    if (nw->hFont) { DeleteObject(nw->hFont); nw->hFont = nullptr; }
                    // remove reverse handle map entries pointing to this NativeWin
                    for (auto it2 = g_handleMap.begin(); it2 != g_handleMap.end(); ) {
                        if (it2->second == nw) it2 = g_handleMap.erase(it2); else ++it2;
                    }
                    delete nw;
                }
                g_hwndMap.erase(it);
            }
            // Only post quit if this was the last window we manage
            if (g_hwndMap.empty()) PostQuitMessage(0);
            return 0;
        }
        default:
            // Default: delegate to DefWindowProcW for unhandled messages to ensure expected behavior
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    // Fallback return; should never reach here due to returns in each case/default.
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
#endif
// Forward declarations for built-in modules defined in separate translation units
std::string __erelang_builtin_math_dispatch(const std::string& name, const std::vector<std::string>& argv);
std::string __erelang_builtin_network_dispatch(const std::string& name, const std::vector<std::string>& argv);
std::string __erelang_builtin_system_dispatch(const std::string& name, const std::vector<std::string>& argv);
std::string __erelang_builtin_crypto_dispatch(const std::string& name, const std::vector<std::string>& argv);
std::string __erelang_builtin_data_dispatch(const std::string& name, const std::vector<std::string>& argv);
std::string __erelang_builtin_regex_dispatch(const std::string& name, const std::vector<std::string>& argv);
std::string __erelang_builtin_perm_dispatch(const std::string& name, const std::vector<std::string>& argv);
std::string __erelang_builtin_binary_dispatch(const std::string& name, const std::vector<std::string>& argv);
std::string __erelang_builtin_threads_dispatch(Runtime* rt, const std::string& name, const std::vector<std::string>& argv);
// Forward declare monitor dispatch (implemented in new builtins/monitor.cpp)
std::string __erelang_builtin_monitor_dispatch(Runtime* rt, const std::string& name, const std::vector<std::string>& argv);

std::string Runtime::eval_builtin_call(std::string_view name, const std::vector<ExprPtr>& args, const Runtime::Env& env) const {
    // Local owning string for APIs expecting std::string
    std::string nameStr(name);
    // Check both local and global vars for alias binding
    if (auto aliasIt = env.vars.find(nameStr); aliasIt != env.vars.end()) {
        const std::string& aliasTarget = aliasIt->second;
        if (aliasTarget.rfind(kBuiltinAliasPrefix.data(), 0) == 0) {
            nameStr = aliasTarget.substr(kBuiltinAliasPrefix.size());
        }
    } else if (auto aliasIt = globalVars_.find(nameStr); aliasIt != globalVars_.end()) {
        const std::string& aliasTarget = aliasIt->second;
        if (aliasTarget.rfind(kBuiltinAliasPrefix.data(), 0) == 0) {
            nameStr = aliasTarget.substr(kBuiltinAliasPrefix.size());
        }
    }

    // Script module alias resolution: alias.action(...) -> action(...)
    // Example: #include <modules/math.elan> as math ; math.sum(2,3) -> sum(2,3)
    if (currentProgram_) {
        const auto dot = nameStr.find('.');
        if (dot != std::string::npos && dot > 0 && dot + 1 < nameStr.size()) {
            const std::string alias = nameStr.substr(0, dot);
            const std::string method = nameStr.substr(dot + 1);
            for (const auto& importDecl : currentProgram_->imports) {
                if (!importDecl.alias || *importDecl.alias != alias || importDecl.path.empty()) continue;
                std::string normalizedPath = importDecl.path;
                for (auto& ch : normalizedPath) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                const bool isBuiltin = (normalizedPath.rfind("builtin/", 0) == 0 || normalizedPath.rfind("builtin:", 0) == 0);
                if (isBuiltin) continue;
                if (find_action(*currentProgram_, method)) {
                    nameStr = method;
                }
                break;
            }
        }
    }

    auto argS = [&](size_t i){ return i < args.size() ? eval_string(*args[i], env) : std::string(); };
    // If the name refers to a scripted action in the running program, execute it and return any ctx.returnValue
    if (currentProgram_) {
        if (const Action* sa = find_action(*currentProgram_, nameStr)) {
            if (currentProgram_->strict && sa->visibility != Visibility::Public) {
                // respect strict mode
            }
            ExecContext actCtx;
            Env calleeEnv;
            for (const auto& kv : globalVars_) calleeEnv.vars[kv.first] = kv.second;
            // bind args
            for (size_t i=0;i<sa->params.size() && i<args.size(); ++i) calleeEnv.vars[sa->params[i].name] = eval_string(*args[i], env);
            exec_block(sa->body, *currentProgram_, actCtx, calleeEnv);
            // propagate any changed globals back
            for (const auto& kv : calleeEnv.vars) if (globalNames_.count(kv.first)) globalVars_[kv.first] = kv.second;
            return actCtx.returned ? actCtx.returnValue : std::string();
        }
    }
    // Policy gate: deny if not allowed
    if (!PolicyManager::instance().is_allowed(nameStr)) {
        // Silent deny returns empty; later we can emit a diagnostic variable or throw
        return {};
    }
    // Deterministic scaffolding
    static bool s_deterministic = false;
    static uint64_t s_seed = 0;
    static bool s_seedInit = false;
    static uint64_t s_timeVirtual = 0; // milliseconds
    if (!s_seedInit) {
        // Acquire seed from CLI args if provided: --seed <n> and --deterministic flags
        for (size_t i=0;i<s_cliArgs.size();++i) {
            if (s_cliArgs[i] == "--deterministic") { s_deterministic = true; }
            if (s_cliArgs[i] == "--seed" && i+1 < s_cliArgs.size()) { s_seed = (uint64_t)std::stoull(s_cliArgs[i+1]); }
        }
        if (s_seed == 0) s_seed = 0xC0FFEEULL; // default constant seed if not provided
        s_seedInit = true;
    }
    auto deterministic_rng = [&]()->uint64_t {
        // simple xorshift64*
        static uint64_t x = 0;
        if (x == 0) x = (s_seed? s_seed : 0xBAD5EEDULL);
        x ^= x >> 12; x ^= x << 25; x ^= x >> 27; return x * 2685821657736338717ULL;
    };
    
    // Builtins to control deterministic virtual time
    if (nameStr == "advance_time" && s_deterministic) {
        uint64_t delta = (uint64_t)to_int(eval_string(*args[0], env));
        s_timeVirtual += delta; return std::to_string((long long)s_timeVirtual);
    }
    if (nameStr == "now_ms" && s_deterministic) {
        return std::to_string((long long)s_timeVirtual);
    }
    if (nameStr == "dev_meta") {
        std::string key = args.empty()?std::string():eval_string(*args[0], env);
        for (auto & c : key) c = (char)tolower((unsigned char)c);
        if (key == "version") return ERELANG_VERSION_STRING;
        if (key == "deterministic") return s_deterministic?"true":"false";
        if (key == "seed") return std::to_string((long long)s_seed);
        if (key == "features") return "window,data,regex,perm,binary,threads,crypto";
        return {};
    }
    if (nameStr == "plugin_core") {
        if (args.size() < 2) {
            return {};
        }
        const std::string slug = eval_string(*args[0], env);
        const std::string query = eval_string(*args[1], env);
        if (slug.empty() || query.empty()) {
            return {};
        }
        const PluginRecord* record = nullptr;
        for (const auto& plugin : pluginRecords_) {
            if (plugin.slug == slug) {
                record = &plugin;
                break;
            }
        }
        if (!record) {
            return {};
        }
        const auto [fileName, keyName] = split_core_query(query);
        if (!fileName.empty() && keyName.empty()) {
            return {};
        }
        auto lookup = [&](const std::string& file, const std::string& key) -> std::string {
            if (key.empty()) {
                return {};
            }
            if (!file.empty()) {
                auto itFile = record->coreProperties.find(file);
                if (itFile == record->coreProperties.end()) {
                    return {};
                }
                auto itKey = itFile->second.find(key);
                if (itKey == itFile->second.end()) {
                    return {};
                }
                return itKey->second;
            }
            for (const auto& entry : record->coreProperties) {
                auto itKey = entry.second.find(key);
                if (itKey != entry.second.end()) {
                    return itKey->second;
                }
            }
            return {};
        };
        std::string targetKey;
        if (keyName.empty()) {
            targetKey = trim_copy(query);
        } else {
            targetKey = keyName;
        }
        return lookup(fileName, targetKey);
    }
    if (nameStr == "plugin_core_files") {
        if (args.empty()) {
            return {};
        }
        const std::string slug = eval_string(*args[0], env);
        if (slug.empty()) {
            return {};
        }
        for (const auto& plugin : pluginRecords_) {
            if (plugin.slug == slug) {
                std::vector<std::string> files;
                files.reserve(plugin.coreProperties.size());
                for (const auto& kv : plugin.coreProperties) {
                    files.push_back(kv.first);
                }
                return join_strings(std::move(files));
            }
        }
        return {};
    }
    if (nameStr == "plugin_core_keys") {
        if (args.size() < 2) {
            return {};
        }
        const std::string slug = eval_string(*args[0], env);
        const std::string fileName = eval_string(*args[1], env);
        if (slug.empty() || fileName.empty()) {
            return {};
        }
        for (const auto& plugin : pluginRecords_) {
            if (plugin.slug == slug) {
                auto itFile = plugin.coreProperties.find(fileName);
                if (itFile == plugin.coreProperties.end()) {
                    return {};
                }
                std::vector<std::string> keys;
                keys.reserve(itFile->second.size());
                for (const auto& kv : itFile->second) {
                    keys.push_back(kv.first);
                }
                return join_strings(std::move(keys));
            }
        }
        return {};
    }
    if (nameStr == "audit") {
        // audit(event, detail?) -> currently prints structured line; later integrate real JSON logger
        std::string ev = args.size()>0? eval_string(*args[0], env):std::string();
        std::string dt = args.size()>1? eval_string(*args[1], env):std::string();
        std::cerr << "{\"audit\":true,\"event\":\"" << ev << "\",\"detail\":\"" << dt << "\"}" << std::endl;
        return {};
    }
    // List/Dict built-ins (cross-platform)
    if (nameStr == "language_name") {
        return std::string("erelang / Erelang");
    }
    if (nameStr == "language_version") {
        return std::string(erelang::BuildInfo::version());
    }
    if (nameStr == "language_about") {
        return std::string(
            "Erelang (erelang) is a small, batteries-included DSL for desktop scripting.\n"
            "It focuses on quick GUIs, simple entities, and practical I/O built-ins.\n"
        );
    }
    if (nameStr == "language_limitations") {
        return std::string(
            "Reasons it leans DSL-like / special-purpose:\n\n"
            "- Scope is narrow: Bakes in GUI, filesystem, UUIDs, debugging, entities.\n"
            "  No broad ecosystems (networking libs, math/science packages, or richer\n"
            "  concurrency beyond parallel{} + wait all).\n"
            "- Small type system: Only str, int, bool; no floats, generics, or advanced\n"
            "  memory management.\n"
            "- Built-in focus: Opinionated first-class concepts (Window, Gui, entity)\n"
            "  over an open-ended, extensible library ecosystem like Python/C++.\n"
        );
    }
    // Conversion and type-check helpers
    if (nameStr == "toint") {
        return std::to_string((long long)to_int(argS(0)));
    }
    if (nameStr == "toInt") {
        return std::to_string((long long)to_int(argS(0)));
    }
    if (nameStr == "tofloat") {
        double v = to_double(argS(0));
        std::ostringstream ss; ss << v; return ss.str();
    }
    if (nameStr == "tostr") {
        return argS(0);
    }
    if (nameStr == "toString") {
        return argS(0);
    }
    if (nameStr == "tobool") {
        return is_truthy(argS(0)) ? std::string("true") : std::string("false");
    }
    if (nameStr == "string.lstrip") {
        std::string s = argS(0);
        size_t i = 0;
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])) != 0) ++i;
        return s.substr(i);
    }
    if (nameStr == "string.rstrip") {
        std::string s = argS(0);
        if (s.empty()) return s;
        size_t j = s.size();
        while (j > 0 && std::isspace(static_cast<unsigned char>(s[j - 1])) != 0) --j;
        return s.substr(0, j);
    }
    if (nameStr == "string.strip") {
        std::string s = argS(0);
        size_t i = 0;
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])) != 0) ++i;
        size_t j = s.size();
        while (j > i && std::isspace(static_cast<unsigned char>(s[j - 1])) != 0) --j;
        return s.substr(i, j - i);
    }
    if (nameStr == "string.lower") {
        std::string s = argS(0);
        for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return s;
    }
    if (nameStr == "string.upper") {
        std::string s = argS(0);
        for (auto& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        return s;
    }
    if (nameStr == "is_int") {
        return is_int_string(argS(0)) ? std::string("true") : std::string("false");
    }
    if (nameStr == "is_float") {
        return is_float_string(argS(0)) ? std::string("true") : std::string("false");
    }
    if (nameStr == "is_str") {
        // Everything is a string at runtime; consider non-empty as true
        return argS(0).empty() ? std::string("false") : std::string("true");
    }
    if (nameStr == "args_count") {
    return std::to_string((int)s_cliArgs.size());
    }
    if (nameStr == "args_get") {
    int idx = to_int(argS(0));
    if (idx >= 0 && idx < (int)s_cliArgs.size()) return s_cliArgs[idx];
    return {};
    }
    if (nameStr == "exec") {
    // Execute arbitrary command (shell). Return exit code as string.
    std::string cmd = argS(0);
#ifdef _WIN32
    std::string full = std::string("cmd /c ") + cmd;
#else
    std::string full = cmd;
#endif
    int code = std::system(full.c_str());
    return std::to_string(code);
    }
    if (nameStr == "run_file") {
    // Run a file with default OS association (like double-click). No output capture; returns empty.
    std::string fp = argS(0);
#ifdef _WIN32
    std::wstring w(fp.begin(), fp.end()); ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
    return {};
    }
    if (nameStr == "run_bat") {
    std::string bat = argS(0);
#ifdef _WIN32
    std::string full = std::string("cmd /c \"") + bat + "\"";
    std::system(full.c_str());
#endif
    return {};
    }
    if (nameStr == "read_line") {
        std::string s; std::getline(std::cin, s); return s;
    }
    if (nameStr == "input") {
        if (!args.empty()) {
            std::string msg = argS(0);
            std::cout << msg;
            std::cout.flush();
        }
        std::string s;
        std::getline(std::cin, s);
        return s;
    }
    if (nameStr == "prompt") {
        std::string msg = argS(0); std::cout << msg; std::cout.flush(); std::string s; std::getline(std::cin, s); return s;
    }
    // Filesystem utilities
    if (nameStr == "read_text") {
        std::ifstream in(argS(0), std::ios::binary);
        if (!in) return {};
        std::ostringstream ss; ss << in.rdbuf();
        return ss.str();
    }
    if (nameStr == "write_text") {
        std::ofstream out(argS(0), std::ios::binary);
        if (!out) return {};
        out << argS(1);
        return {};
    }
    if (nameStr == "append_text") {
        std::ofstream out(argS(0), std::ios::binary | std::ios::app);
        if (!out) return {};
        out << argS(1);
        return {};
    }
    if (nameStr == "file_exists") {
        return std::filesystem::exists(argS(0)) ? std::string("true") : std::string("false");
    }
    if (nameStr == "mkdirs") {
        std::error_code ec; std::filesystem::create_directories(argS(0), ec); return {};
    }
    if (nameStr == "copy_file") {
        std::error_code ec; bool ok = std::filesystem::copy_file(argS(0), argS(1), std::filesystem::copy_options::overwrite_existing, ec);
        return ok && !ec ? std::string("true") : std::string("false");
    }
    if (nameStr == "move_file") {
        std::error_code ec; std::filesystem::rename(argS(0), argS(1), ec);
        return !ec ? std::string("true") : std::string("false");
    }
    if (nameStr == "delete_file") {
        std::error_code ec; std::filesystem::remove(argS(0), ec);
        return !ec ? std::string("true") : std::string("false");
    }
    if (nameStr == "list_files") {
        int id = g_nextListId++;
        g_lists[id] = {};
        std::error_code ec; for (auto& e : std::filesystem::directory_iterator(argS(0), ec)) {
            g_lists[id].push_back(e.path().string());
        }
        return std::string("list:") + std::to_string(id);
    }
    if (nameStr == "cwd") {
        return std::filesystem::current_path().string();
    }
    if (nameStr == "chdir") {
        std::error_code ec; std::filesystem::current_path(argS(0), ec); return !ec ? std::string("true") : std::string("false");
    }
    if (nameStr == "path_join") {
        if (args.empty()) return {};
        std::filesystem::path p = argS(0);
        for (std::size_t i = 1; i < args.size(); ++i) {
            p /= argS(i);
        }
        return p.string();
    }
    if (nameStr == "path_dirname") {
        return std::filesystem::path(argS(0)).parent_path().string();
    }
    if (nameStr == "path_basename") {
        return std::filesystem::path(argS(0)).filename().string();
    }
    if (nameStr == "path_ext") {
        return std::filesystem::path(argS(0)).extension().string();
    }
    // Time utilities
    if (nameStr == "now_iso") {
        using namespace std::chrono;
        auto t = system_clock::now(); auto tt = system_clock::to_time_t(t);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &tt);
#else
        localtime_r(&tt, &tm);
#endif
        char buf[32]; std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        return std::string(buf);
    }
    if (nameStr == "rand_int") {
        int a = (int)to_int(argS(0)); int b = (int)to_int(argS(1)); if (a > b) std::swap(a,b);
        if (s_deterministic) {
            uint64_t r = deterministic_rng();
            int span = (b - a) + 1; if (span <= 0) span = 1;
            return std::to_string(a + (int)(r % (uint64_t)span));
        } else {
            static std::mt19937 rng{std::random_device{}()};
            std::uniform_int_distribution<int> dist(a, b);
            return std::to_string(dist(rng));
        }
    }
    if (nameStr == "uuid") {
#ifdef _WIN32
        UUID u; if (UuidCreate(&u) == RPC_S_OK) {
            RPC_CSTR s = nullptr; if (UuidToStringA(&u, &s) == RPC_S_OK && s) {
                std::string out(reinterpret_cast<char*>(s)); RpcStringFreeA(&s); return out;
            }
        }
        return {};
#else
        // Fallback: pseudo UUID
        static std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution<int> d(0, 15);
        const char* hex = "0123456789abcdef"; std::string r(36, '-');
        int idxs[] = {8,13,18,23}; int p = 0;
        for (int i=0;i<36;++i) { if (p < 4 && i == idxs[p]) { ++p; continue; } r[i] = hex[d(rng)]; }
        return r;
#endif
    }
    if (nameStr == "list_new") {
        int id = g_nextListId++;
        g_lists[id] = {};
        for (const auto& a : args) {
            g_lists[id].push_back(eval_string(*a, env));
        }
        return std::string("list:") + std::to_string(id);
    }
    if (nameStr == "list_push") {
        std::string h = argS(0); std::string v = argS(1);
        if (h.rfind("list:", 0) == 0) {
            int id = to_int(h.substr(5)); g_lists[id].push_back(v);
        }
        return {};
    }
    if (nameStr == "list_get") {
        std::string h = argS(0); int idx = to_int(argS(1));
        if (h.rfind("list:", 0) == 0) {
            int id = to_int(h.substr(5)); auto& vec = g_lists[id];
            if (idx >=0 && idx < (int)vec.size()) return vec[idx];
        }
        return {};
    }
    if (nameStr == "list_len") {
        std::string h = argS(0);
        if (h.rfind("list:", 0) == 0) { int id = to_int(h.substr(5)); return std::to_string((int)g_lists[id].size()); }
        return "0";
    }
    if (nameStr == "list_join") {
        std::string h = argS(0); std::string sep = argS(1);
        if (h.rfind("list:", 0) == 0) { int id = to_int(h.substr(5));
            std::ostringstream ss; const auto& v = g_lists[id];
            for (size_t i=0;i<v.size();++i) { if (i) ss << sep; ss << v[i]; }
            return ss.str();
        }
        return {};
    }
    if (nameStr == "list_clear") {
        std::string h = argS(0); if (h.rfind("list:", 0) == 0) { int id = to_int(h.substr(5)); g_lists[id].clear(); }
        return {};
    }
    if (nameStr == "list_remove_at") {
        std::string h = argS(0); int idx = (int)to_int(argS(1));
        if (h.rfind("list:", 0) == 0) { int id = to_int(h.substr(5)); auto& v = g_lists[id]; if (idx>=0 && idx<(int)v.size()) v.erase(v.begin()+idx); }
        return {};
    }
    if (nameStr == "dict_new") {
        int id = g_nextDictId++;
        g_dicts[id] = {};
        // Optional key/value varargs: dict_new("k1", v1, "k2", v2, ...)
        for (std::size_t i = 0; i + 1 < args.size(); i += 2) {
            std::string key = eval_string(*args[i], env);
            std::string value = eval_string(*args[i + 1], env);
            g_dicts[id][key] = value;
        }
        return std::string("dict:") + std::to_string(id);
    }
    auto join_builtin_path = [&](size_t from, size_t toExclusive) -> std::string {
        if (toExclusive <= from) return {};
        std::ostringstream oss;
        for (size_t i = from; i < toExclusive; ++i) {
            if (i > from) oss << '.';
            oss << argS(i);
        }
        return oss.str();
    };
    if (nameStr == "dict_set") {
        std::string h = argS(0);
        if (args.size() < 3) return {};
        std::string k = join_builtin_path(1, args.size() - 1);
        std::string v = argS(args.size() - 1);
        if (h.rfind("dict:", 0) == 0) { int id = to_int(h.substr(5)); g_dicts[id][k] = v; }
        return {};
    }
    if (nameStr == "dict_get") {
        std::string h = argS(0);
        if (args.size() < 2) return {};
        std::string k = join_builtin_path(1, args.size());
        if (h.rfind("dict:", 0) == 0) { int id = to_int(h.substr(5)); auto it = g_dicts[id].find(k); if (it!=g_dicts[id].end()) return it->second; }
        return {};
    }
    if (nameStr == "dict_has") {
        std::string h = argS(0);
        if (args.size() < 2) return "false";
        std::string k = join_builtin_path(1, args.size());
        if (h.rfind("dict:", 0) == 0) { int id = to_int(h.substr(5)); return (g_dicts[id].count(k)?"true":"false"); }
        return "false";
    }
    if (nameStr == "dict_keys") {
        std::string h = argS(0); if (h.rfind("dict:", 0) != 0) return {};
        int id = to_int(h.substr(5)); int lid = g_nextListId++; g_lists[lid] = {};
        for (const auto& kv : g_dicts[id]) g_lists[lid].push_back(kv.first);
        return std::string("list:") + std::to_string(lid);
    }
    if (nameStr == "dict_values") {
        std::string h = argS(0); if (h.rfind("dict:", 0) != 0) return {};
        int id = to_int(h.substr(5)); int lid = g_nextListId++; g_lists[lid] = {};
        for (const auto& kv : g_dicts[id]) g_lists[lid].push_back(kv.second);
        return std::string("list:") + std::to_string(lid);
    }
    if (nameStr == "dict_get_or") {
        std::string h = argS(0);
        if (args.size() < 3) return {};
        std::string k = join_builtin_path(1, args.size() - 1);
        std::string def = argS(args.size() - 1);
        if (h.rfind("dict:", 0) == 0) {
            int id = to_int(h.substr(5));
            auto it = g_dicts[id].find(k);
            if (it != g_dicts[id].end()) return it->second;
        }
        return def;
    }
    if (nameStr == "dict_remove") {
        std::string h = argS(0);
        if (args.size() < 2) return "false";
        std::string k = join_builtin_path(1, args.size());
        if (h.rfind("dict:", 0) == 0) {
            int id = to_int(h.substr(5));
            return g_dicts[id].erase(k) ? "true" : "false";
        }
        return "false";
    }
    if (nameStr == "dict_clear") {
        std::string h = argS(0);
        if (h.rfind("dict:", 0) == 0) {
            int id = to_int(h.substr(5));
            g_dicts[id].clear();
        }
        return {};
    }
    if (nameStr == "dict_size") {
        std::string h = argS(0);
        if (h.rfind("dict:", 0) == 0) {
            int id = to_int(h.substr(5));
            return std::to_string((int)g_dicts[id].size());
        }
        return "0";
    }
    if (nameStr == "dict_merge") {
        std::string target = argS(0);
        std::string source = argS(1);
        if (target.rfind("dict:", 0) == 0 && source.rfind("dict:", 0) == 0) {
            int targetId = to_int(target.substr(5));
            int sourceId = to_int(source.substr(5));
            for (const auto& kv : g_dicts[sourceId]) {
                g_dicts[targetId][kv.first] = kv.second;
            }
        }
        return {};
    }
    if (nameStr == "dict_clone") {
        std::string source = argS(0);
        int newId = g_nextDictId++;
        g_dicts[newId] = {};
        if (source.rfind("dict:", 0) == 0) {
            int sourceId = to_int(source.substr(5));
            g_dicts[newId] = g_dicts[sourceId];
        }
        return std::string("dict:") + std::to_string(newId);
    }
    if (nameStr == "dict_items" || nameStr == "dict_entries") {
        std::string h = argS(0);
        if (h.rfind("dict:", 0) != 0) return {};
        int id = to_int(h.substr(5));
        int lid = g_nextListId++;
        g_lists[lid] = {};
        for (const auto& kv : g_dicts[id]) {
            g_lists[lid].push_back(kv.first + "=" + kv.second);
        }
        return std::string("list:") + std::to_string(lid);
    }
    if (nameStr == "dict_set_path") {
        std::string h = argS(0); std::string path = argS(1); std::string v = argS(2);
        if (h.rfind("dict:", 0) == 0) {
            int id = to_int(h.substr(5));
            g_dicts[id][path] = v;
        }
        return {};
    }
    if (nameStr == "dict_get_path") {
        std::string h = argS(0); std::string path = argS(1);
        std::string def = (args.size() > 2) ? argS(2) : std::string();
        if (h.rfind("dict:", 0) == 0) {
            int id = to_int(h.substr(5));
            auto it = g_dicts[id].find(path);
            if (it != g_dicts[id].end()) return it->second;
        }
        return def;
    }
    if (nameStr == "dict_has_path") {
        std::string h = argS(0); std::string path = argS(1);
        if (h.rfind("dict:", 0) == 0) {
            int id = to_int(h.substr(5));
            return g_dicts[id].count(path) ? "true" : "false";
        }
        return "false";
    }
    if (nameStr == "dict_remove_path") {
        std::string h = argS(0); std::string path = argS(1);
        if (h.rfind("dict:", 0) == 0) {
            int id = to_int(h.substr(5));
            return g_dicts[id].erase(path) ? "true" : "false";
        }
        return "false";
    }
    if (nameStr == "table_new") {
        int id = g_nextDictId++;
        g_dicts[id] = {};
        return std::string("dict:") + std::to_string(id);
    }
    if (nameStr == "table_put") {
        std::string h = argS(0);
        std::string row = argS(1);
        std::string col = argS(2);
        std::string value = argS(3);
        if (h.rfind("dict:", 0) == 0) {
            int id = to_int(h.substr(5));
            g_dicts[id][row + "." + col] = value;
        }
        return {};
    }
    if (nameStr == "table_get") {
        std::string h = argS(0);
        std::string row = argS(1);
        std::string col = argS(2);
        std::string def = (args.size() > 3) ? argS(3) : std::string();
        if (h.rfind("dict:", 0) == 0) {
            int id = to_int(h.substr(5));
            auto key = row + "." + col;
            auto it = g_dicts[id].find(key);
            if (it != g_dicts[id].end()) return it->second;
        }
        return def;
    }
    if (nameStr == "table_has") {
        std::string h = argS(0);
        std::string row = argS(1);
        std::string col = argS(2);
        if (h.rfind("dict:", 0) == 0) {
            int id = to_int(h.substr(5));
            return g_dicts[id].count(row + "." + col) ? "true" : "false";
        }
        return "false";
    }
    if (nameStr == "table_remove") {
        std::string h = argS(0);
        std::string row = argS(1);
        std::string col = argS(2);
        if (h.rfind("dict:", 0) == 0) {
            int id = to_int(h.substr(5));
            return g_dicts[id].erase(row + "." + col) ? "true" : "false";
        }
        return "false";
    }
    if (nameStr == "table_rows") {
        std::string h = argS(0);
        if (h.rfind("dict:", 0) != 0) return {};
        int id = to_int(h.substr(5));
        std::unordered_set<std::string> rows;
        for (const auto& kv : g_dicts[id]) {
            auto pos = kv.first.find('.');
            if (pos != std::string::npos) {
                rows.insert(kv.first.substr(0, pos));
            }
        }
        int lid = g_nextListId++;
        g_lists[lid] = {};
        for (const auto& row : rows) g_lists[lid].push_back(row);
        return std::string("list:") + std::to_string(lid);
    }
    if (nameStr == "table_columns") {
        std::string h = argS(0);
        if (h.rfind("dict:", 0) != 0) return {};
        int id = to_int(h.substr(5));
        std::unordered_set<std::string> cols;
        for (const auto& kv : g_dicts[id]) {
            auto pos = kv.first.find('.');
            if (pos != std::string::npos && pos + 1 < kv.first.size()) {
                cols.insert(kv.first.substr(pos + 1));
            }
        }
        int lid = g_nextListId++;
        g_lists[lid] = {};
        for (const auto& col : cols) g_lists[lid].push_back(col);
        return std::string("list:") + std::to_string(lid);
    }
    if (nameStr == "table_row_keys") {
        std::string h = argS(0);
        std::string row = argS(1);
        if (h.rfind("dict:", 0) != 0) return {};
        int id = to_int(h.substr(5));
        std::string prefix = row + ".";
        int lid = g_nextListId++;
        g_lists[lid] = {};
        for (const auto& kv : g_dicts[id]) {
            if (kv.first.rfind(prefix, 0) == 0 && kv.first.size() > prefix.size()) {
                g_lists[lid].push_back(kv.first.substr(prefix.size()));
            }
        }
        return std::string("list:") + std::to_string(lid);
    }
    if (nameStr == "table_clear_row") {
        std::string h = argS(0);
        std::string row = argS(1);
        if (h.rfind("dict:", 0) == 0) {
            int id = to_int(h.substr(5));
            std::string prefix = row + ".";
            std::vector<std::string> toErase;
            for (const auto& kv : g_dicts[id]) {
                if (kv.first.rfind(prefix, 0) == 0) toErase.push_back(kv.first);
            }
            for (const auto& key : toErase) g_dicts[id].erase(key);
        }
        return {};
    }
    if (nameStr == "table_count_row") {
        std::string h = argS(0);
        std::string row = argS(1);
        if (h.rfind("dict:", 0) == 0) {
            int id = to_int(h.substr(5));
            std::string prefix = row + ".";
            int count = 0;
            for (const auto& kv : g_dicts[id]) {
                if (kv.first.rfind(prefix, 0) == 0) ++count;
            }
            return std::to_string(count);
        }
        return "0";
    }
    if (nameStr == "now_ms") {
        using namespace std::chrono;
        return std::to_string(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
    }
    if (nameStr == "env") {
        std::string key = argS(0);
#ifdef _WIN32
        std::wstring wkey(key.begin(), key.end());
        DWORD len = GetEnvironmentVariableW(wkey.c_str(), nullptr, 0);
        if (len) {
            std::wstring buf; buf.resize(len);
            GetEnvironmentVariableW(wkey.c_str(), buf.data(), len);
            std::string out(buf.begin(), buf.end());
            if (!out.empty() && out.back()=='\0') out.pop_back();
            return out;
        }
        return {};
#else
        const char* v = std::getenv(key.c_str());
        return v ? std::string(v) : std::string();
#endif
    }
#ifdef _WIN32
    // Low-level windowing built-ins for modular GUI handlers
    if (nameStr == "win_window_create") {
        std::string t = argS(0);
        int w = to_int(argS(1));
        int h = to_int(argS(2));
        return create_native_window(this, t, w, h);
    }
    if (nameStr == "ui_window_create") {
        std::string title = argS(0);
        int w = to_int(argS(1));
        int h = to_int(argS(2));
        std::string handle = create_native_window(this, title, w, h);
        if (handle.empty()) {
            return {};
        }
        NativeWin* nw = parse_window_handle(handle);
        if (nw) {
            int padX = (args.size() > 3) ? to_int(argS(3)) : -1;
            int padY = (args.size() > 4) ? to_int(argS(4)) : -1;
            int lineH = (args.size() > 5) ? to_int(argS(5)) : -1;
            int spacing = (args.size() > 6) ? to_int(argS(6)) : -1;
            int defaultW = (args.size() > 7) ? to_int(argS(7)) : -1;
            if (padX >= 0 || padY >= 0 || lineH > 0 || spacing >= 0 || defaultW > 0) {
                int spacingX = spacing;
                int spacingY = spacing;
                layout_configure(nw, padX, padY, lineH, spacingX, spacingY, defaultW);
            }
        }
        return handle;
    }
    auto parse_handle = [&](const std::string& s)->NativeWin*{
        return parse_window_handle(s);
    };
    if (nameStr == "ui_label") {
        NativeWin* nw = parse_handle(argS(0));
        if (!nw) return {};
        std::string text = argS(1);
        int width = (args.size() > 2) ? to_int(argS(2)) : 0;
        int height = (args.size() > 3) ? to_int(argS(3)) : 0;
        LayoutSlot slot = layout_take_slot(nw, width, height);
        create_label_control(nw, slot, text);
        return {};
    }
    if (nameStr == "ui_same_line") {
        NativeWin* nw = parse_handle(argS(0));
        if (!nw) return {};
        layout_signal_same_line(nw);
        return {};
    }
    if (nameStr == "ui_newline") {
        NativeWin* nw = parse_handle(argS(0));
        if (!nw) return {};
        layout_force_newline(nw);
        return {};
    }
    if (nameStr == "ui_spacer") {
        NativeWin* nw = parse_handle(argS(0));
        if (!nw) return {};
        int width = (args.size() > 1) ? to_int(argS(1)) : 0;
        int height = (args.size() > 2) ? to_int(argS(2)) : 0;
        layout_take_slot(nw, width, height);
        return {};
    }
    if (nameStr == "ui_separator") {
        NativeWin* nw = parse_handle(argS(0));
        if (!nw) return {};
        int width = (args.size() > 1) ? to_int(argS(1)) : 0;
        int thickness = (args.size() > 2) ? to_int(argS(2)) : 2;
        LayoutSlot slot = layout_take_slot(nw, width, thickness);
        slot.h = (thickness > 0) ? thickness : slot.h;
        create_separator_control(nw, slot, thickness);
        return {};
    }
    if (nameStr == "ui_button") {
        NativeWin* nw = parse_handle(argS(0));
        if (!nw) return {};
        std::string idName = argS(1);
        std::string text = argS(2);
        std::string actionName = (args.size() > 3) ? argS(3) : std::string();
        int width = (args.size() > 4) ? to_int(argS(4)) : 0;
        int height = (args.size() > 5) ? to_int(argS(5)) : 0;
        LayoutSlot slot = layout_take_slot(nw, width, height);
        int id = create_button_control(nw, slot, idName, text, false);
        assign_action_if_any(nw, id, actionName, {});
        return {};
    }
    if (nameStr == "ui_checkbox") {
        NativeWin* nw = parse_handle(argS(0));
        if (!nw) return {};
        std::string idName = argS(1);
        std::string text = argS(2);
        std::string actionName = (args.size() > 3) ? argS(3) : std::string();
        int width = (args.size() > 4) ? to_int(argS(4)) : 0;
        int height = (args.size() > 5) ? to_int(argS(5)) : 0;
        bool checked = (args.size() > 6) ? is_truthy(argS(6)) : false;
        LayoutSlot slot = layout_take_slot(nw, width, height);
        int id = create_checkbox_control(nw, slot, idName, text, checked);
        assign_action_if_any(nw, id, actionName, {});
        return {};
    }
    if (nameStr == "ui_radio") {
        NativeWin* nw = parse_handle(argS(0));
        if (!nw) return {};
        std::string idName = argS(1);
        std::string text = argS(2);
        bool groupStart = (args.size() > 3) ? is_truthy(argS(3)) : false;
        std::string actionName = (args.size() > 4) ? argS(4) : std::string();
        int width = (args.size() > 5) ? to_int(argS(5)) : 0;
        int height = (args.size() > 6) ? to_int(argS(6)) : 0;
        bool checked = (args.size() > 7) ? is_truthy(argS(7)) : false;
        LayoutSlot slot = layout_take_slot(nw, width, height);
        int id = create_radio_control(nw, slot, idName, text, groupStart, checked);
        assign_action_if_any(nw, id, actionName, {});
        return {};
    }
    if (nameStr == "ui_slider") {
        NativeWin* nw = parse_handle(argS(0));
        if (!nw) return {};
        std::string idName = argS(1);
        int minv = to_int(argS(2));
        int maxv = to_int(argS(3));
        int value = to_int(argS(4));
        std::string actionName = (args.size() > 5) ? argS(5) : std::string();
        int width = (args.size() > 6) ? to_int(argS(6)) : 0;
        int height = (args.size() > 7) ? to_int(argS(7)) : 0;
        LayoutSlot slot = layout_take_slot(nw, width, height);
        int id = create_slider_control(nw, slot, idName, minv, maxv, value);
        assign_action_if_any(nw, id, actionName, {});
        return {};
    }
    if (nameStr == "ui_textbox") {
        NativeWin* nw = parse_handle(argS(0));
        if (!nw) return {};
        std::string idName = argS(1);
        std::string initial = (args.size() > 2) ? argS(2) : std::string();
        std::string actionName = (args.size() > 3) ? argS(3) : std::string();
        int width = (args.size() > 4) ? to_int(argS(4)) : 0;
        int height = (args.size() > 5) ? to_int(argS(5)) : 0;
        LayoutSlot slot = layout_take_slot(nw, width, height);
        int id = create_textbox_control(nw, slot, idName, initial);
        assign_action_if_any(nw, id, actionName, {});
        return {};
    }
    if (nameStr == "ui_load") {
    fs::path input = path_from_u8(argS(0));
        std::string fallbackAction = (args.size() > 1) ? argS(1) : std::string();
        bool autoShow = (args.size() > 2) ? is_truthy(argS(2)) : true;
        bool autoLoop = (args.size() > 3) ? is_truthy(argS(3)) : false;
        if (!input.is_absolute()) {
            input = fs::current_path() / input;
        }
        std::error_code ec;
        if (!fs::exists(input, ec)) {
            std::ostringstream es; es << "ui_load: file not found '" << input.string() << "'";
            winlog(es.str().c_str());
            return {};
        }
        std::string data = slurp_text(input);
        std::string parseError;
        auto parsed = parse_eui_document(data, parseError);
        if (!parsed) {
            std::ostringstream es; es << "ui_load: " << parseError;
            winlog(es.str().c_str());
            return {};
        }
        NativeWin* nw = nullptr;
        std::string handle;
        bool localAutoShow = autoShow;
        bool localAutoLoop = autoLoop;
        std::string localFallback = fallbackAction;
        auto attr_lookup = [](const EuiCommand& cmd, std::initializer_list<const char*> keys) -> const std::string* {
            for (const char* k : keys) {
                auto it = cmd.attrs.find(k);
                if (it != cmd.attrs.end()) {
                    return &it->second;
                }
            }
            return nullptr;
        };
        for (const auto& cmd : *parsed) {
            const std::string& kw = cmd.keyword;
            if (kw == "window") {
                std::string title;
                if (const auto* v = attr_lookup(cmd, {"title", "text", "caption"})) {
                    title = *v;
                } else if (!cmd.positional.empty()) {
                    title = cmd.positional[0];
                }
                if (title.empty()) {
                    title = "Erelang UI";
                }
                int width = 420;
                if (const auto* v = attr_lookup(cmd, {"width", "w"})) {
                    width = to_int(*v);
                } else if (cmd.positional.size() > 1) {
                    width = to_int(cmd.positional[1]);
                }
                int height = 320;
                if (const auto* v = attr_lookup(cmd, {"height", "h"})) {
                    height = to_int(*v);
                } else if (cmd.positional.size() > 2) {
                    height = to_int(cmd.positional[2]);
                }
                handle = create_native_window(this, title, width, height);
                if (handle.empty()) {
                    return {};
                }
                nw = parse_window_handle(handle);
                if (!nw) {
                    return handle;
                }
                int padX = -1;
                int padY = -1;
                int lineH = -1;
                int spacingX = -1;
                int spacingY = -1;
                int defaultW = -1;
                if (const auto* pad = attr_lookup(cmd, {"pad", "padding"})) {
                    parse_int_pair(*pad, padX, padY);
                }
                if (const auto* padXv = attr_lookup(cmd, {"padx"})) padX = to_int(*padXv);
                if (const auto* padYv = attr_lookup(cmd, {"pady"})) padY = to_int(*padYv);
                if (const auto* line = attr_lookup(cmd, {"line", "lineheight"})) lineH = to_int(*line);
                if (const auto* spacing = attr_lookup(cmd, {"spacing"})) parse_int_pair(*spacing, spacingX, spacingY);
                if (const auto* spacingXV = attr_lookup(cmd, {"spacingx"})) spacingX = to_int(*spacingXV);
                if (const auto* spacingYV = attr_lookup(cmd, {"spacingy"})) spacingY = to_int(*spacingYV);
                if (const auto* defW = attr_lookup(cmd, {"defaultwidth", "controlwidth"})) defaultW = to_int(*defW);
                layout_configure(nw, padX, padY, lineH, spacingX, spacingY, defaultW);
                if (const auto* handler = attr_lookup(cmd, {"handler", "action"})) {
                    if (!handler->empty()) {
                        localFallback = *handler;
                    }
                }
                if (const auto* showAttr = attr_lookup(cmd, {"show"})) {
                    localAutoShow = is_truthy(*showAttr);
                }
                if (const auto* loopAttr = attr_lookup(cmd, {"loop", "autoloop"})) {
                    localAutoLoop = is_truthy(*loopAttr);
                }
                if (const auto* scaleAttr = attr_lookup(cmd, {"scale"})) {
                    int pct = to_int(*scaleAttr);
                    if (pct <= 0) pct = 100;
                    nw->scale = pct / 100.0;
                    if (nw->hFont) { DeleteObject(nw->hFont); nw->hFont = nullptr; }
                    nw->hFont = create_font_for_scale(nw->scale);
                    for (const auto& kv : nw->idToHwnd) apply_font_for(kv.second, nw->hFont);
                }
                if (const auto* autoScaleAttr = attr_lookup(cmd, {"autoscale"})) {
                    if (is_truthy(*autoScaleAttr)) {
                        UINT dpi = 96;
                        HMODULE hUser = GetModuleHandleW(L"user32.dll");
                        auto pGetDpiForWindow = reinterpret_cast<UINT (WINAPI*)(HWND)>(GetProcAddress(hUser, "GetDpiForWindow"));
                        if (pGetDpiForWindow) dpi = pGetDpiForWindow(nw->hwnd);
                        nw->scale = dpi / 96.0;
                        if (nw->hFont) { DeleteObject(nw->hFont); nw->hFont = nullptr; }
                        nw->hFont = create_font_for_scale(nw->scale);
                        for (const auto& kv : nw->idToHwnd) apply_font_for(kv.second, nw->hFont);
                    }
                }
                continue;
            }
            if (!nw) {
                continue;
            }
            auto control_text = [&](const EuiCommand& c) {
                if (const auto* v = attr_lookup(c, {"text", "label", "caption"})) {
                    return *v;
                }
                if (!c.positional.empty()) {
                    return c.positional[0];
                }
                return std::string();
            };
            auto control_id = [&](const EuiCommand& c) {
                if (const auto* v = attr_lookup(c, {"id", "name"})) {
                    return *v;
                }
                if (!c.positional.empty()) {
                    return c.positional[0];
                }
                return std::string();
            };
            auto control_width = [&](const EuiCommand& c) -> int {
                if (const auto* v = attr_lookup(c, {"width", "w"})) {
                    return static_cast<int>(to_int(*v));
                }
                return 0;
            };
            auto control_height = [&](const EuiCommand& c) -> int {
                if (const auto* v = attr_lookup(c, {"height", "h"})) {
                    return static_cast<int>(to_int(*v));
                }
                return 0;
            };
            auto control_action = [&](const EuiCommand& c) {
                if (const auto* v = attr_lookup(c, {"on", "action", "callback"})) {
                    return *v;
                }
                return std::string();
            };
            if (kw == "layout") {
                int padX = -1;
                int padY = -1;
                int lineH = -1;
                int spacingX = -1;
                int spacingY = -1;
                int defaultW = -1;
                if (const auto* pad = attr_lookup(cmd, {"pad", "padding"})) parse_int_pair(*pad, padX, padY);
                if (const auto* padXv = attr_lookup(cmd, {"padx"})) padX = to_int(*padXv);
                if (const auto* padYv = attr_lookup(cmd, {"pady"})) padY = to_int(*padYv);
                if (const auto* line = attr_lookup(cmd, {"line", "lineheight"})) lineH = to_int(*line);
                if (const auto* spacing = attr_lookup(cmd, {"spacing"})) parse_int_pair(*spacing, spacingX, spacingY);
                if (const auto* spacingXV = attr_lookup(cmd, {"spacingx"})) spacingX = to_int(*spacingXV);
                if (const auto* spacingYV = attr_lookup(cmd, {"spacingy"})) spacingY = to_int(*spacingYV);
                if (const auto* defW = attr_lookup(cmd, {"defaultwidth", "controlwidth"})) defaultW = to_int(*defW);
                layout_configure(nw, padX, padY, lineH, spacingX, spacingY, defaultW);
                continue;
            }
            if (kw == "label") {
                std::string text = control_text(cmd);
                int width = control_width(cmd);
                int height = control_height(cmd);
                LayoutSlot slot = layout_take_slot(nw, width, height);
                create_label_control(nw, slot, text);
                continue;
            }
            if (kw == "button") {
                std::string id = control_id(cmd);
                std::string text = control_text(cmd);
                std::string action = control_action(cmd);
                int width = control_width(cmd);
                int height = control_height(cmd);
                LayoutSlot slot = layout_take_slot(nw, width, height);
                int idNum = create_button_control(nw, slot, id, text, false);
                assign_action_if_any(nw, idNum, action, localFallback);
                continue;
            }
            if (kw == "checkbox") {
                std::string id = control_id(cmd);
                std::string text = control_text(cmd);
                std::string action = control_action(cmd);
                bool checked = false;
                if (const auto* v = attr_lookup(cmd, {"checked", "value"})) {
                    checked = is_truthy(*v);
                }
                int width = control_width(cmd);
                int height = control_height(cmd);
                LayoutSlot slot = layout_take_slot(nw, width, height);
                int idNum = create_checkbox_control(nw, slot, id, text, checked);
                assign_action_if_any(nw, idNum, action, localFallback);
                continue;
            }
            if (kw == "radio") {
                std::string id = control_id(cmd);
                std::string text = control_text(cmd);
                std::string action = control_action(cmd);
                bool groupStart = false;
                if (const auto* g = attr_lookup(cmd, {"group", "groupstart"})) {
                    groupStart = is_truthy(*g);
                }
                bool checked = false;
                if (const auto* v = attr_lookup(cmd, {"checked", "value"})) {
                    checked = is_truthy(*v);
                }
                int width = control_width(cmd);
                int height = control_height(cmd);
                LayoutSlot slot = layout_take_slot(nw, width, height);
                int idNum = create_radio_control(nw, slot, id, text, groupStart, checked);
                assign_action_if_any(nw, idNum, action, localFallback);
                continue;
            }
            if (kw == "slider") {
                std::string id = control_id(cmd);
                std::string action = control_action(cmd);
                int minv = 0;
                if (const auto* v = attr_lookup(cmd, {"min"})) minv = to_int(*v);
                int maxv = 100;
                if (const auto* v = attr_lookup(cmd, {"max"})) maxv = to_int(*v);
                int value = minv;
                if (const auto* v = attr_lookup(cmd, {"value", "initial"})) value = to_int(*v);
                int width = control_width(cmd);
                int height = control_height(cmd);
                LayoutSlot slot = layout_take_slot(nw, width, height);
                int idNum = create_slider_control(nw, slot, id, minv, maxv, value);
                assign_action_if_any(nw, idNum, action, localFallback);
                continue;
            }
            if (kw == "textbox" || kw == "input") {
                std::string id = control_id(cmd);
                std::string text = control_text(cmd);
                std::string action = control_action(cmd);
                int width = control_width(cmd);
                int height = control_height(cmd);
                LayoutSlot slot = layout_take_slot(nw, width, height);
                int idNum = create_textbox_control(nw, slot, id, text);
                assign_action_if_any(nw, idNum, action, localFallback);
                continue;
            }
            if (kw == "sameline" || kw == "same") {
                layout_signal_same_line(nw);
                continue;
            }
            if (kw == "newline" || kw == "linebreak") {
                layout_force_newline(nw);
                continue;
            }
            if (kw == "spacer") {
                int width = control_width(cmd);
                int height = control_height(cmd);
                layout_take_slot(nw, width, height);
                continue;
            }
            if (kw == "separator" || kw == "rule") {
                int width = control_width(cmd);
                int thickness = 2;
                if (const auto* t = attr_lookup(cmd, {"height", "thickness"})) thickness = to_int(*t);
                LayoutSlot slot = layout_take_slot(nw, width, thickness);
                slot.h = (thickness > 0) ? thickness : slot.h;
                create_separator_control(nw, slot, thickness);
                continue;
            }
        }
        if (nw && localAutoShow) {
            ShowWindow(nw->hwnd, SW_SHOWNORMAL);
            ShowWindow(nw->hwnd, SW_RESTORE);
            UpdateWindow(nw->hwnd);
            SetForegroundWindow(nw->hwnd);
            RedrawWindow(nw->hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        }
        if (nw && localAutoLoop) {
            run_message_loop(localFallback);
        }
        return handle;
    }
    if (nameStr == "win_layout_config") {
        std::string hs = argS(0); NativeWin* nw = parse_handle(hs); if (!nw) return {};
        int padX = to_int(argS(1));
        int padY = to_int(argS(2));
        int lineH = to_int(argS(3));
        int spacingX = to_int(argS(4));
        int spacingY = to_int(argS(5));
        int defaultW = to_int(argS(6));
        layout_configure(nw, padX, padY, lineH, spacingX, spacingY, defaultW);
        return {};
    }
    if (nameStr == "win_layout_stack") {
        std::string hs = argS(0); NativeWin* nw = parse_handle(hs); if (!nw) return {};
        int padX = to_int(argS(1));
        int padY = to_int(argS(2));
        int lineH = to_int(argS(3));
        int spacing = to_int(argS(4));
        int defaultW = to_int(argS(5));
        layout_configure(nw, padX, padY, lineH, spacing, spacing, defaultW);
        return {};
    }
    if (nameStr == "win_layout_reset") {
        std::string hs = argS(0); NativeWin* nw = parse_handle(hs); if (!nw) return {};
        layout_reset(nw);
        return {};
    }
    if (nameStr == "win_layout_same") {
        std::string hs = argS(0); NativeWin* nw = parse_handle(hs); if (!nw) return {};
        layout_signal_same_line(nw);
        return {};
    }
    if (nameStr == "win_layout_newline") {
        std::string hs = argS(0); NativeWin* nw = parse_handle(hs); if (!nw) return {};
        layout_force_newline(nw);
        return {};
    }
    if (nameStr == "win_layout_push_width") {
        std::string hs = argS(0); NativeWin* nw = parse_handle(hs); if (!nw) return {};
        int width = to_int(argS(1));
        layout_push_width(nw, width);
        return {};
    }
    if (nameStr == "win_layout_pop_width") {
        std::string hs = argS(0); NativeWin* nw = parse_handle(hs); if (!nw) return {};
        layout_pop_width(nw);
        return {};
    }
    if (nameStr == "win_button_create") {
        std::string hs = argS(0); NativeWin* nw = parse_handle(hs); if (!nw) return {};
        std::string idName = argS(1); std::string txt = argS(2);
        int x = to_int(argS(3)), y = to_int(argS(4)), w = to_int(argS(5)), h = to_int(argS(6));
        bool autoPlace = (x < 0 && y < 0);
        if (autoPlace) {
            LayoutSlot slot = layout_take_slot(nw, w, h);
            x = slot.x; y = slot.y; w = slot.w; h = slot.h;
        } else {
            layout_notify_manual(nw, x, y, w, h);
        }
        { std::ostringstream ss; ss << "win_button_create id='" << idName << "' text='" << txt << "' at (" << x << "," << y << ") size=" << w << "x" << h; if (autoPlace) ss << " [auto]"; winlog(ss.str().c_str()); }
        auto S = [&](int v){ return iround(v * nw->scale); };
        std::wstring wtxt(txt.begin(), txt.end()); int id = nw->nextId++;
        nw->idToName[id] = idName; nw->nameToId[idName] = id;
        HINSTANCE hInst = GetModuleHandleW(nullptr);
        HWND ch = CreateWindowExW(0, L"BUTTON", wtxt.c_str(), WS_TABSTOP|WS_VISIBLE|WS_CHILD|BS_DEFPUSHBUTTON,
            S(x), S(y), S(w), S(h), nw->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
        nw->idToHwnd[id] = ch;
        apply_font_for(ch, nw->hFont);
        RedrawWindow(nw->hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        return {};
    }
    if (nameStr == "win_checkbox_create") {
        std::string hs = argS(0); NativeWin* nw = parse_handle(hs); if (!nw) return {};
        std::string idName = argS(1); std::string txt = argS(2);
        int x = to_int(argS(3)), y = to_int(argS(4)), w = to_int(argS(5)), h = to_int(argS(6));
        bool autoPlace = (x < 0 && y < 0);
        if (autoPlace) {
            LayoutSlot slot = layout_take_slot(nw, w, h);
            x = slot.x; y = slot.y; w = slot.w; h = slot.h;
        } else {
            layout_notify_manual(nw, x, y, w, h);
        }
        { std::ostringstream ss; ss << "win_checkbox_create id='" << idName << "' text='" << txt << "' at (" << x << "," << y << ") size=" << w << "x" << h; if (autoPlace) ss << " [auto]"; winlog(ss.str().c_str()); }
        std::wstring wtxt(txt.begin(), txt.end()); int id = nw->nextId++;
        nw->idToName[id] = idName; nw->nameToId[idName] = id;
        HINSTANCE hInst = GetModuleHandleW(nullptr);
        auto S = [&](int v){ return iround(v * nw->scale); };
        HWND ch = CreateWindowExW(0, L"BUTTON", wtxt.c_str(), WS_TABSTOP|WS_VISIBLE|WS_CHILD|BS_AUTOCHECKBOX,
            S(x), S(y), S(w), S(h), nw->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
        nw->idToHwnd[id] = ch; apply_font_for(ch, nw->hFont);
        RedrawWindow(nw->hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        return {};
    }
    if (nameStr == "win_radiobutton_create") {
        std::string hs = argS(0); NativeWin* nw = parse_handle(hs); if (!nw) return {};
        std::string idName = argS(1); std::string txt = argS(2);
        int x = to_int(argS(3)), y = to_int(argS(4)), w = to_int(argS(5)), h = to_int(argS(6));
        bool groupStart = false; if (args.size() > 7) { std::string g = argS(7); groupStart = (g == "true" || to_int(g) != 0); }
        bool autoPlace = (x < 0 && y < 0);
        if (autoPlace) {
            LayoutSlot slot = layout_take_slot(nw, w, h);
            x = slot.x; y = slot.y; w = slot.w; h = slot.h;
        } else {
            layout_notify_manual(nw, x, y, w, h);
        }
        std::wstring wtxt(txt.begin(), txt.end()); int id = nw->nextId++;
        nw->idToName[id] = idName; nw->nameToId[idName] = id;
        HINSTANCE hInst = GetModuleHandleW(nullptr);
    DWORD style = WS_TABSTOP|WS_VISIBLE|WS_CHILD|BS_AUTORADIOBUTTON; if (groupStart) style |= WS_GROUP;
        auto S = [&](int v){ return iround(v * nw->scale); };
        HWND ch = CreateWindowExW(0, L"BUTTON", wtxt.c_str(), style,
            S(x), S(y), S(w), S(h), nw->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
        nw->idToHwnd[id] = ch; apply_font_for(ch, nw->hFont);
        RedrawWindow(nw->hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        return {};
    }
    if (nameStr == "win_slider_create") {
        std::string hs = argS(0); NativeWin* nw = parse_handle(hs); if (!nw) return {};
        std::string idName = argS(1);
        int x = to_int(argS(2)), y = to_int(argS(3)), w = to_int(argS(4)), h = to_int(argS(5));
        int minv = to_int(argS(6)), maxv = to_int(argS(7)), val = to_int(argS(8));
        bool autoPlace = (x < 0 && y < 0);
        if (autoPlace) {
            LayoutSlot slot = layout_take_slot(nw, w, h);
            x = slot.x; y = slot.y; w = slot.w; h = slot.h;
        } else {
            layout_notify_manual(nw, x, y, w, h);
        }
        HINSTANCE hInst = GetModuleHandleW(nullptr); int id = nw->nextId++;
        nw->idToName[id] = idName; nw->nameToId[idName] = id;
                auto S = [&](int v){ return iround(v * nw->scale); };
                HWND ch = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_VISIBLE|WS_CHILD|TBS_AUTOTICKS,
                    S(x), S(y), S(w), S(h), nw->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
                SendMessageW(ch, TBM_SETRANGEMIN, TRUE, minv);
                SendMessageW(ch, TBM_SETRANGEMAX, TRUE, maxv);
                SendMessageW(ch, TBM_SETPOS, TRUE, val);
                nw->idToHwnd[id] = ch; apply_font_for(ch, nw->hFont);
                RedrawWindow(nw->hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
                return {};
    }
    if (nameStr == "win_label_create") {
        std::string hs = argS(0); NativeWin* nw = parse_handle(hs); if (!nw) return {};
        std::string txt = argS(1); int x = to_int(argS(2)), y = to_int(argS(3)), w = to_int(argS(4)), h = to_int(argS(5));
        bool autoPlace = (x < 0 && y < 0);
        if (autoPlace) {
            LayoutSlot slot = layout_take_slot(nw, w, h);
            x = slot.x; y = slot.y; w = slot.w; h = slot.h;
        } else {
            layout_notify_manual(nw, x, y, w, h);
        }
        { std::ostringstream ss; ss << "win_label_create text='" << txt << "' at (" << x << "," << y << ") size=" << w << "x" << h; if (autoPlace) ss << " [auto]"; winlog(ss.str().c_str()); }
        std::wstring wtxt(txt.begin(), txt.end()); HINSTANCE hInst = GetModuleHandleW(nullptr);
        auto S = [&](int v){ return iround(v * nw->scale); };
        HWND ch = CreateWindowExW(0, L"STATIC", wtxt.c_str(), WS_VISIBLE|WS_CHILD|SS_LEFT|SS_NOPREFIX, S(x), S(y), S(w), S(h), nw->hwnd, nullptr, hInst, nullptr);
        apply_font_for(ch, nw->hFont);
        RedrawWindow(nw->hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        return {};
    }
    if (nameStr == "win_textbox_create") {
        std::string hs = argS(0); NativeWin* nw = parse_handle(hs); if (!nw) return {};
        std::string idName = argS(1); int x = to_int(argS(2)), y = to_int(argS(3)), w = to_int(argS(4)), h = to_int(argS(5));
        bool autoPlace = (x < 0 && y < 0);
        if (autoPlace) {
            LayoutSlot slot = layout_take_slot(nw, w, h);
            x = slot.x; y = slot.y; w = slot.w; h = slot.h;
        } else {
            layout_notify_manual(nw, x, y, w, h);
        }
        HINSTANCE hInst = GetModuleHandleW(nullptr); int id = nw->nextId++;
        nw->idToName[id] = idName; nw->nameToId[idName] = id;
                auto S = [&](int v){ return iround(v * nw->scale); };
                HWND ch = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_VISIBLE|WS_CHILD|ES_LEFT|ES_AUTOHSCROLL,
                    S(x), S(y), S(w), S(h), nw->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
                nw->idToHwnd[id] = ch; apply_font_for(ch, nw->hFont);
                RedrawWindow(nw->hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
                return {};
    }
    if (nameStr == "win_set_title") {
        std::string hs = argS(0); NativeWin* nw = parse_handle(hs); if (!nw) return {};
        std::string t = argS(1); std::wstring wt(t.begin(), t.end()); SetWindowTextW(nw->hwnd, wt.c_str()); return {};
    }
    if (nameStr == "win_move") {
        std::string hs = argS(0); NativeWin* nw = parse_handle(hs); if (!nw) return {};
        int x = to_int(argS(1)), y = to_int(argS(2)); RECT rc{}; GetWindowRect(nw->hwnd, &rc);
        MoveWindow(nw->hwnd, x, y, rc.right-rc.left, rc.bottom-rc.top, TRUE); return {};
    }
    if (nameStr == "win_resize") {
        std::string hs = argS(0); NativeWin* nw = parse_handle(hs); if (!nw) return {};
        int w = to_int(argS(1)), h = to_int(argS(2)); RECT rc{}; GetWindowRect(nw->hwnd, &rc);
        MoveWindow(nw->hwnd, rc.left, rc.top, w, h, TRUE); return {};
    }
    if (nameStr == "win_set_text") {
        std::string hs = argS(0); std::string idName = argS(1); std::string txt = argS(2);
        NativeWin* nw = parse_handle(hs); if (!nw) return {};
        auto it = nw->nameToId.find(idName); if (it == nw->nameToId.end()) return {};
        auto hw = nw->idToHwnd[it->second]; std::wstring wtxt(txt.begin(), txt.end()); SetWindowTextW(hw, wtxt.c_str()); return {};
    }
    if (nameStr == "win_get_text") {
        std::string hs = argS(0); std::string idName = argS(1);
        NativeWin* nw = parse_handle(hs); if (!nw) return {};
        auto it = nw->nameToId.find(idName); if (it == nw->nameToId.end()) return {};
        auto hw = nw->idToHwnd[it->second]; int len = GetWindowTextLengthW(hw); std::wstring buf; buf.resize(len+1);
        GetWindowTextW(hw, buf.data(), len+1); std::string s(buf.begin(), buf.end()); if (!s.empty() && s.back()=='\0') s.pop_back(); return s;
    }
    if (nameStr == "win_get_check") {
        std::string hs = argS(0); std::string idName = argS(1);
        NativeWin* nw = parse_handle(hs); if (!nw) return {};
        auto it = nw->nameToId.find(idName); if (it == nw->nameToId.end()) return {};
        auto hw = nw->idToHwnd[it->second]; LRESULT st = SendMessageW(hw, BM_GETCHECK, 0, 0);
        return (st == BST_CHECKED || st == BST_INDETERMINATE) ? std::string("true") : std::string("false");
    }
    if (nameStr == "win_set_check") {
        std::string hs = argS(0); std::string idName = argS(1); std::string v = argS(2);
        NativeWin* nw = parse_handle(hs); if (!nw) return {};
        auto it = nw->nameToId.find(idName); if (it == nw->nameToId.end()) return {};
        auto hw = nw->idToHwnd[it->second]; BOOL chk = (v == "true" || to_int(v) != 0);
        SendMessageW(hw, BM_SETCHECK, chk ? BST_CHECKED : BST_UNCHECKED, 0); return {};
    }
    if (nameStr == "win_get_slider") {
        std::string hs = argS(0); std::string idName = argS(1);
        NativeWin* nw = parse_handle(hs); if (!nw) return {};
        auto it = nw->nameToId.find(idName); if (it == nw->nameToId.end()) return {};
        auto hw = nw->idToHwnd[it->second]; LRESULT pos = SendMessageW(hw, TBM_GETPOS, 0, 0);
        return std::to_string(static_cast<int>(pos));
    }
    if (nameStr == "win_set_slider") {
        std::string hs = argS(0); std::string idName = argS(1); int val = to_int(argS(2));
        NativeWin* nw = parse_handle(hs); if (!nw) return {};
        auto it = nw->nameToId.find(idName); if (it == nw->nameToId.end()) return {};
        auto hw = nw->idToHwnd[it->second]; SendMessageW(hw, TBM_SETPOS, TRUE, val); return {};
    }
    if (nameStr == "win_on") {
        std::string hs = argS(0); std::string idName = argS(1); std::string action = argS(2);
        NativeWin* nw = parse_handle(hs); if (!nw) return {};
        auto it = nw->nameToId.find(idName); if (it == nw->nameToId.end()) return {};
        nw->idToAction[it->second] = action; return {};
    }
    if (nameStr == "win_show") {
        std::string hs = argS(0); NativeWin* nw = parse_handle(hs); if (!nw) return {};
        // Force a normal, restored show state to avoid starting minimized from shell nCmdShow
        ShowWindow(nw->hwnd, SW_SHOWNORMAL);
        ShowWindow(nw->hwnd, SW_RESTORE);
        UpdateWindow(nw->hwnd);
        SetForegroundWindow(nw->hwnd);
        RedrawWindow(nw->hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        return {};
    }
    if (nameStr == "win_close") {
        std::string hs = argS(0); NativeWin* nw = parse_handle(hs); if (!nw) return {};
        PostMessageW(nw->hwnd, WM_CLOSE, 0, 0);
        return {};
    }
    if (nameStr == "win_set_scale") {
        std::string hs = argS(0); NativeWin* nw = parse_handle(hs); if (!nw) return {};
        int pct = to_int(argS(1)); if (pct <= 0) pct = 100; nw->scale = pct / 100.0;
        if (nw->hFont) { DeleteObject(nw->hFont); nw->hFont = nullptr; }
        nw->hFont = create_font_for_scale(nw->scale);
        // Apply new font to all known children
        for (const auto& kv : nw->idToHwnd) apply_font_for(kv.second, nw->hFont);
        return {};
    }
    if (nameStr == "win_auto_scale") {
        std::string hs = argS(0); NativeWin* nw = parse_handle(hs); if (!nw) return {};
        UINT dpi = 96; HMODULE hUser = GetModuleHandleW(L"user32.dll");
        auto pGetDpiForWindow = reinterpret_cast<UINT (WINAPI*)(HWND)>(GetProcAddress(hUser, "GetDpiForWindow"));
        if (pGetDpiForWindow) dpi = pGetDpiForWindow(nw->hwnd);
        nw->scale = dpi / 96.0;
        if (nw->hFont) { DeleteObject(nw->hFont); nw->hFont = nullptr; }
        nw->hFont = create_font_for_scale(nw->scale);
        for (const auto& kv : nw->idToHwnd) apply_font_for(kv.second, nw->hFont);
        return {};
    }
    if (nameStr == "win_message_box") {
        std::string hs = argS(0); NativeWin* nw = parse_handle(hs);
        std::string t = argS(1); std::string m = argS(2);
        UINT icon = MB_ICONINFORMATION;
        // Optional 4th argument: icon kind
        if (args.size() >= 4) {
            std::string kind = argS(3);
            for (auto & c : kind) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
            if (kind == "warn" || kind == "warning") icon = MB_ICONWARNING; // alias MB_ICONEXCLAMATION
            else if (kind == "error" || kind == "err" || kind == "fatal") icon = MB_ICONERROR;
            else if (kind == "question" || kind == "ask" || kind == "qry") icon = MB_ICONQUESTION;
            else if (kind == "info" || kind == "information" || kind == "i") icon = MB_ICONINFORMATION; // default
        }
        std::wstring wt(t.begin(), t.end()), wm(m.begin(), m.end());
        MessageBoxW(nw ? nw->hwnd : nullptr, wm.c_str(), wt.c_str(), MB_OK | icon);
        return {};
    }
    if (nameStr == "win_loop") {
        run_message_loop(argS(0));
        return {};
    }
    if (nameStr == "username") {
        wchar_t buf[256]; DWORD sz = 256;
        if (GetUserNameW(buf, &sz)) { std::wstring ws(buf, sz ? sz-1 : 0); return std::string(ws.begin(), ws.end()); }
        wchar_t vbuf[256]; DWORD vlen = GetEnvironmentVariableW(L"USERNAME", vbuf, 256);
        if (vlen > 0 && vlen < 256) { std::wstring ws(vbuf, vlen); return std::string(ws.begin(), ws.end()); }
        return {};
    }
    if (nameStr == "machine_guid") {
        HKEY hKey;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", 0, KEY_READ | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS) {
            wchar_t data[256]; DWORD len = sizeof(data);
            if (RegQueryValueExW(hKey, L"MachineGuid", nullptr, nullptr, reinterpret_cast<LPBYTE>(data), &len) == ERROR_SUCCESS) {
                std::wstring ws(data);
                RegCloseKey(hKey);
                return std::string(ws.begin(), ws.end());
            }
            RegCloseKey(hKey);
        }
        return {};
    }
    if (nameStr == "computer_name") {
        wchar_t buf[256]; DWORD sz = 256;
        if (GetComputerNameW(buf, &sz)) { std::wstring ws(buf, sz); return std::string(ws.begin(), ws.end()); }
        return {};
    }
    if (nameStr == "volume_serial") {
        wchar_t root[] = L"C:\\";
        DWORD serial = 0, maxComp, fsFlags; wchar_t fsName[128]; wchar_t volName[128];
        if (GetVolumeInformationW(root, volName, 128, &serial, &maxComp, &fsFlags, fsName, 128)) {
            return std::to_string(static_cast<unsigned long>(serial));
        }
        return {};
    }
    if (nameStr == "hwid") {
        std::string mg;
        HKEY hKey;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", 0, KEY_READ | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS) {
            wchar_t data[256]; DWORD len = sizeof(data);
            if (RegQueryValueExW(hKey, L"MachineGuid", nullptr, nullptr, reinterpret_cast<LPBYTE>(data), &len) == ERROR_SUCCESS) {
                std::wstring ws(data); mg.assign(ws.begin(), ws.end());
            }
            RegCloseKey(hKey);
        }
        wchar_t root[] = L"C:\\";
        DWORD serial = 0, maxComp, fsFlags; wchar_t fsName[128]; wchar_t volName[128];
        std::string vs;
        if (GetVolumeInformationW(root, volName, 128, &serial, &maxComp, &fsFlags, fsName, 128)) {
            vs = std::to_string(static_cast<unsigned long>(serial));
        }
        return mg + ":" + vs;
    }
    // removed mbox built-in: implement in 0bs via Window entity
#endif
    // Unified external built-in module dispatch (evaluate args only once if present)
    std::vector<std::string> argv;
    if (!args.empty()) {
        argv.reserve(args.size());
        for (const auto& a : args) argv.push_back(eval_string(*a, env));
    }
    const std::string& n = nameStr; // alias for dispatchers expecting std::string
    if (auto r = __erelang_builtin_math_dispatch(n, argv); !r.empty()) return r;
    if (auto r = __erelang_builtin_network_dispatch(n, argv); !r.empty()) return r;
    if (auto r = __erelang_builtin_system_dispatch(n, argv); !r.empty()) return r;
    if (auto r = __erelang_builtin_crypto_dispatch(n, argv); !r.empty()) return r;
    if (auto r = __erelang_builtin_monitor_dispatch(const_cast<Runtime*>(this), n, argv); !r.empty()) return r;
    if (auto r = __erelang_builtin_data_dispatch(n, argv); !r.empty()) return r;
    if (auto r = __erelang_builtin_regex_dispatch(n, argv); !r.empty()) return r;
    if (auto r = __erelang_builtin_perm_dispatch(n, argv); !r.empty()) return r;
    if (auto r = __erelang_builtin_binary_dispatch(n, argv); !r.empty()) return r;
    if (auto r = __erelang_builtin_threads_dispatch(const_cast<Runtime*>(this), n, argv); !r.empty()) return r;
    return {};
}

static bool is_int_string(const std::string& s) {
    if (s.empty()) return false;
    size_t i = 0; if (s[0] == '-' || s[0] == '+') i = 1; if (i >= s.size()) return false;
    for (; i < s.size(); ++i) if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    return true;
}

static bool is_truthy(const std::string& v) {
    if (v == "true") return true;
    if (v == "false") return false;
    return to_int(v) != 0 || !v.empty();
}

static bool is_identifier_text(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    auto is_alpha_or_underscore = [](char ch) {
        return std::isalpha(static_cast<unsigned char>(ch)) || ch == '_';
    };
    auto is_alnum_or_underscore = [](char ch) {
        return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
    };
    if (!is_alpha_or_underscore(text.front())) {
        return false;
    }
    for (size_t i = 1; i < text.size(); ++i) {
        if (!is_alnum_or_underscore(text[i])) {
            return false;
        }
    }
    return true;
}

std::optional<ExprPtr> Runtime::parse_interpolation_expr(std::string_view exprText) const {
    const std::string key = trim_copy(exprText);
    if (key.empty()) {
        return std::nullopt;
    }

    {
        std::lock_guard<std::mutex> lock(interpolationExprCacheMutex_);
        auto it = interpolationExprCache_.find(key);
        if (it != interpolationExprCache_.end()) {
            return it->second;
        }
    }

    try {
        std::string script;
        script.reserve(key.size() + 64);
        script += "@erelang\n";
        script += "public action __fmt {\n";
        script += "  return ";
        script += key;
        script += "\n}";

        LexerOptions lxopts;
        lxopts.enableDurations = true;
        lxopts.enableUnits = true;
        lxopts.enablePolyIdentifiers = true;
        lxopts.emitDocComments = false;
        lxopts.emitComments = false;
        Lexer lexer(script, lxopts);
        Parser parser(lexer.lex());
        Program program = parser.parse();
        for (const auto& action : program.actions) {
            if (action.name != "__fmt") {
                continue;
            }
            for (const auto& stmt : action.body.stmts) {
                if (!std::holds_alternative<ReturnStmt>(stmt)) {
                    continue;
                }
                const auto& ret = std::get<ReturnStmt>(stmt);
                if (!ret.value.has_value() || !(*ret.value)) {
                    return std::nullopt;
                }
                ExprPtr parsed = *ret.value;
                {
                    std::lock_guard<std::mutex> lock(interpolationExprCacheMutex_);
                    interpolationExprCache_[key] = parsed;
                }
                return parsed;
            }
        }
    } catch (...) {
        return std::nullopt;
    }

    return std::nullopt;
}

std::optional<std::string> Runtime::eval_interpolation_expr(std::string_view exprText, const Env& env) const {
    auto parsed = parse_interpolation_expr(exprText);
    if (!parsed.has_value() || !(*parsed)) {
        std::string raw = trim_copy(exprText);
        if (raw.size() > 2 && raw.back() == ')' && raw[raw.size() - 2] == '(') {
            const std::string fn = raw.substr(0, raw.size() - 2);
            if (is_identifier_text(fn)) {
                try {
                    return eval_builtin_call(fn, {}, env);
                } catch (...) {
                    return std::nullopt;
                }
            }
        }
        return std::nullopt;
    }
    try {
        return eval_string(*(*parsed), env);
    } catch (...) {
        return std::nullopt;
    }
}

std::string Runtime::eval_string(const Expr& e, const Env& env) const {
    if (std::holds_alternative<ExprString>(e.node)) {
        const auto& n = std::get<ExprString>(e.node);
        std::string out; out.reserve(n.v.size());
        const std::string& s = n.v;
        for (size_t i=0;i<s.size();){
            if (s[i]=='{') {
                size_t j = s.find('}', i+1);
                if (j!=std::string::npos) {
                    std::string key = s.substr(i+1, j-(i+1));
                    std::string trimmedKey = trim_copy(key);
                    auto it = env.vars.find(trimmedKey);
                    if (it != env.vars.end()) {
                        out += it->second;
                    } else {
                        auto git = globalVars_.find(trimmedKey);
                        if (git != globalVars_.end()) {
                            out += git->second;
                        } else if (auto exprValue = eval_interpolation_expr(trimmedKey, env); exprValue.has_value()) {
                            out += *exprValue;
                        } else {
                            out += '{';
                            out += key;
                            out += '}';
                        }
                    }
                    i = j+1; continue;
                }
            }
            out.push_back(s[i++]);
        }
        return out;
    }
    if (std::holds_alternative<ExprNumber>(e.node)) return std::to_string(std::get<ExprNumber>(e.node).v);
    if (std::holds_alternative<ExprBool>(e.node)) return std::get<ExprBool>(e.node).v ? "true" : "false";
    if (std::holds_alternative<ExprIdent>(e.node)) {
        const auto& n = std::get<ExprIdent>(e.node);
        auto it = env.vars.find(n.name);
        if (it!=env.vars.end()) return it->second;
        auto git = globalVars_.find(n.name);
        if (git != globalVars_.end()) return git->second;
        if (env.objects.find(n.name) != env.objects.end()) return n.name;
        return n.name;
    }
    if (std::holds_alternative<UnaryExpr>(e.node)) {
        const auto& u = std::get<UnaryExpr>(e.node);
        std::string v = eval_string(*u.expr, env);
        switch (u.op) {
            case UnOp::Neg: return std::to_string(-to_int(v));
            case UnOp::Not: return (v == "true" || to_int(v) != 0) ? "false" : "true";
        }
    }
    if (std::holds_alternative<BinaryExpr>(e.node)) {
        const auto& b = std::get<BinaryExpr>(e.node);
        std::string ls = eval_string(*b.left, env);
        std::string rs = eval_string(*b.right, env);
        int64_t li = to_int(ls), ri = to_int(rs);
        auto is_explicit_string_expr = [](const ExprPtr& expr) -> bool {
            if (!expr) return false;
            if (std::holds_alternative<ExprString>(expr->node)) return true;
            if (std::holds_alternative<FunctionCallExpr>(expr->node)) {
                const auto& fc = std::get<FunctionCallExpr>(expr->node);
                if (fc.name == "tostr" || fc.name == "toString") return true;
                if (fc.name.rfind("string.", 0) == 0) return true;
            }
            return false;
        };
        // Lightweight unit arithmetic: pattern <int><unit>, same unit on both sides
        auto parseUnit = [](const std::string& s) -> std::optional<std::pair<long long,std::string>> {
            if (s.empty() || !std::isdigit(static_cast<unsigned char>(s[0]))) return std::nullopt;
            size_t i = 0; while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
            if (i == 0 || i >= s.size()) return std::nullopt; // must have unit suffix
            // Unit must start with a letter
            if (!std::isalpha(static_cast<unsigned char>(s[i]))) return std::nullopt;
            std::string unit = s.substr(i);
            // Basic validation: disallow whitespace
            for (char ch : unit) { if (std::isspace(static_cast<unsigned char>(ch))) return std::nullopt; }
            long long value = 0; try { value = std::stoll(s.substr(0,i)); } catch (...) { return std::nullopt; }
            return std::make_pair(value, unit);
        };
        auto lu = parseUnit(ls);
        auto ru = parseUnit(rs);
        auto unitAddSub = [&](BinOp op)->std::string {
            if (lu && ru && lu->second == ru->second) {
                if (op == BinOp::Add) return std::to_string(lu->first + ru->first) + lu->second;
                if (op == BinOp::Sub) return std::to_string(lu->first - ru->first) + lu->second;
            }
            return std::string();
        };
        auto is_char_like = [](const std::string& s) -> bool {
            return s.size() == 1 && std::isalpha(static_cast<unsigned char>(s[0])) != 0;
        };
        auto require_int_operands = [&](const char* opName) {
            if (!is_int_string(ls) || !is_int_string(rs)) {
                throw std::runtime_error(std::string("Illegal operation: ") + opName + " requires int operands");
            }
        };
        switch (b.op) {
            case BinOp::Add: {
                if (auto r = unitAddSub(BinOp::Add); !r.empty()) return r;
                const bool leftIsInt = is_int_string(ls);
                const bool rightIsInt = is_int_string(rs);
                if ((is_char_like(ls) && rightIsInt) || (leftIsInt && is_char_like(rs))) {
                    throw std::runtime_error("Illegal operation: char + int");
                }
                if (leftIsInt && rightIsInt) {
                    if (is_explicit_string_expr(b.left) || is_explicit_string_expr(b.right)) return ls + rs;
                    return std::to_string(li + ri);
                }
                if (!leftIsInt && !rightIsInt) return ls + rs;
                if (!leftIsInt && rightIsInt && is_explicit_string_expr(b.right)) return ls + rs;
                if (leftIsInt && !rightIsInt && is_explicit_string_expr(b.left)) return ls + rs;
                throw std::runtime_error("Illegal operation: string + int");
            }
            case BinOp::Sub: {
                if (auto r = unitAddSub(BinOp::Sub); !r.empty()) return r;
                require_int_operands("-");
                return std::to_string(li - ri);
            }
            case BinOp::Mul:
                require_int_operands("*");
                return std::to_string(li * ri);
            case BinOp::Div:
                require_int_operands("/");
                return std::to_string(ri==0?0:li / ri);
            case BinOp::Mod:
                require_int_operands("%");
                return std::to_string(ri==0?0:li % ri);
            case BinOp::Pow: {
                require_int_operands("^");
                if (ri < 0) return "0";
                int64_t value = 1;
                for (int64_t i = 0; i < ri; ++i) value *= li;
                return std::to_string(value);
            }
            case BinOp::EQ: return (ls == rs) ? "true" : "false";
            case BinOp::NE: return (ls != rs) ? "true" : "false";
            case BinOp::LT: return (li < ri) ? "true" : "false";
            case BinOp::LE: return (li <= ri) ? "true" : "false";
            case BinOp::GT: return (li > ri) ? "true" : "false";
            case BinOp::GE: return (li >= ri) ? "true" : "false";
            case BinOp::And: return ((ls=="true" || to_int(ls)!=0) && (rs=="true" || to_int(rs)!=0)) ? "true" : "false";
            case BinOp::Or: return ((ls=="true" || to_int(ls)!=0) || (rs=="true" || to_int(rs)!=0)) ? "true" : "false";
            case BinOp::Coalesce: {
                // treat empty string as nullish; if ls is non-empty, return ls; else rs
                return (!ls.empty() ? ls : rs);
            }
        }
    }
    if (std::holds_alternative<MemberExpr>(e.node)) {
        const auto& m = std::get<MemberExpr>(e.node);
        auto oit = env.objects.find(m.objectName);
        if (oit != env.objects.end()) {
            auto fit = oit->second->fields.find(m.field);
            if (fit != oit->second->fields.end()) return fit->second;
        }
        auto sv = env.vars.find(m.objectName);
        if (sv != env.vars.end() && sv->second.rfind("struct:", 0) == 0) {
            auto fit = env.vars.find(m.objectName + "." + m.field);
            if (fit != env.vars.end()) return fit->second;
        }
        auto it = env.vars.find(m.objectName + "." + m.field);
        if (it != env.vars.end()) return it->second;
        return {};
    }
    if (std::holds_alternative<FunctionCallExpr>(e.node)) {
        const auto& fc = std::get<FunctionCallExpr>(e.node);
        return eval_builtin_call(fc.name, fc.args, env);
    }
    if (std::holds_alternative<NewExpr>(e.node)) {
        const auto& ne = std::get<NewExpr>(e.node);
        return std::string{"<new:"} + ne.typeName + ">";
    }
    return {};
}

const Action* Runtime::find_action(const Program& program, std::string_view name) const {
    for (const auto& a : program.actions) if (a.name == name) return &a;
    return nullptr;
}

const Hook* Runtime::find_hook(const Program& program, std::string_view name) const {
    for (const auto& h : program.hooks) if (h.name == name) return &h;
    return nullptr;
}

const Entity* Runtime::find_entity(const Program& program, std::string_view name) const {
    for (const auto& e : program.entities) if (e.name == name) return &e;
    return nullptr;
}

static const StructDecl* find_struct_decl(const Program& program, std::string_view name) {
    for (const auto& s : program.structs) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

const Action* Runtime::find_entity_method(const Entity& e, std::string_view name) const {
    for (const auto& a : e.methods) if (a.name == name) return &a;
    return nullptr;
}

void Runtime::exec_stmt(const Statement& s, const Program& program, ExecContext& ctx, Env& env) const {
    if (std::holds_alternative<PrintStmt>(s)) {
        const auto& st = std::get<PrintStmt>(s);
        std::cout << eval_string(*st.value, env) << std::endl;
        return;
    }
    if (std::holds_alternative<SleepStmt>(s)) {
        const auto& st = std::get<SleepStmt>(s);
        std::this_thread::sleep_for(std::chrono::milliseconds(st.ms));
        return;
    }
    if (std::holds_alternative<std::shared_ptr<ParallelStmt>>(s)) {
        const auto& p = std::get<std::shared_ptr<ParallelStmt>>(s);
        ctx.threads.emplace_back([this, &program, p, env]() mutable {
            ExecContext child;
            Env childEnv = env;
            exec_block(p->body, program, child, childEnv);
            for (auto& th : child.threads) if (th.joinable()) th.join();
        });
        return;
    }
    if (std::holds_alternative<WaitAllStmt>(s)) {
        for (auto& th : ctx.threads) if (th.joinable()) th.join();
        ctx.threads.clear();
        return;
    }
    if (std::holds_alternative<PauseStmt>(s)) {
        std::string dummy; std::getline(std::cin, dummy);
        return;
    }
    if (std::holds_alternative<InputStmt>(s)) {
        const auto& is = std::get<InputStmt>(s);
        std::string line; std::getline(std::cin, line);
        env.vars[is.name] = line;
        if (globalNames_.count(is.name)) globalVars_[is.name] = line;
        return;
    }
    if (std::holds_alternative<LetStmt>(s)) {
        const auto& st = std::get<LetStmt>(s);
        if (!st.declaredType.empty()) {
            if (const StructDecl* sd = find_struct_decl(program, st.declaredType)) {
                env.vars[st.name] = std::string("struct:") + sd->name;
                for (const auto& f : sd->fields) {
                    env.vars[st.name + "." + f.name] = std::string{};
                }

                const std::string initValue = eval_string(*st.value, env);
                if (initValue.rfind("dict:", 0) == 0) {
                    const int id = to_int(initValue.substr(5));
                    auto dit = g_dicts.find(id);
                    if (dit != g_dicts.end()) {
                        for (const auto& f : sd->fields) {
                            auto fit = dit->second.find(f.name);
                            if (fit != dit->second.end()) {
                                env.vars[st.name + "." + f.name] = fit->second;
                            }
                        }
                    }
                } else if (std::holds_alternative<ExprIdent>(st.value->node)) {
                    const auto& sourceName = std::get<ExprIdent>(st.value->node).name;
                    auto sit = env.vars.find(sourceName);
                    if (sit != env.vars.end() && sit->second == (std::string("struct:") + sd->name)) {
                        for (const auto& f : sd->fields) {
                            auto srcField = env.vars.find(sourceName + "." + f.name);
                            if (srcField != env.vars.end()) {
                                env.vars[st.name + "." + f.name] = srcField->second;
                            }
                        }
                    }
                }

                if (globalNames_.count(st.name)) globalVars_[st.name] = env.vars[st.name];
                return;
            }
        }
        // Support object construction on right-hand side: let x = new Type(args)
        if (std::holds_alternative<NewExpr>(st.value->node)) {
            const auto& ne = std::get<NewExpr>(st.value->node);
            const Entity* ent = find_entity(program, ne.typeName);
            if (!ent) throw std::runtime_error("Unknown entity: " + ne.typeName);
            if (program.strict && ent->visibility != Visibility::Public) {
                throw std::runtime_error("Entity not public: " + ne.typeName);
            }
            auto obj = std::make_shared<Object>();
            obj->typeName = ne.typeName;
            // initialize fields to empty
            for (const auto& f : ent->fields) obj->fields[f.name] = {};
#ifdef _WIN32
            if (ne.typeName == "Window") {
                HINSTANCE hInst = GetModuleHandleW(nullptr);
                ensure_window_class_registered(hInst);
                std::string t0 = eval_string(*ne.args[0], env);
                std::wstring title(t0.begin(), t0.end());
                int w = to_int(eval_string(*ne.args[1], env));
                int h = to_int(eval_string(*ne.args[2], env));
                HWND hwnd = CreateWindowExW(0, g_obsWinClassName.c_str(), title.c_str(), WS_OVERLAPPEDWINDOW,
                    CW_USEDEFAULT, CW_USEDEFAULT, w, h, nullptr, nullptr, hInst, nullptr);
                auto* native = new NativeWin(); native->hwnd = hwnd;
                // Set DPI-based scale and font
                UINT dpi = 96; HMODULE hUser = GetModuleHandleW(L"user32.dll");
                auto pGetDpiForWindow = reinterpret_cast<UINT (WINAPI*)(HWND)>(GetProcAddress(hUser, "GetDpiForWindow"));
                if (pGetDpiForWindow) dpi = pGetDpiForWindow(hwnd);
                native->scale = dpi / 96.0;
                // Apply @scale if provided (percentage)
                for (const auto& ad : program.directives) {
                    if (ad.name == "scale" && ad.value) {
                        int pct = to_int(*ad.value);
                        if (pct > 0) native->scale = pct / 100.0;
                        break;
                    }
                }
                native->hFont = create_font_for_scale(native->scale);
                obj->native = native;
                g_hwndMap[hwnd] = native;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
                apply_font_for(hwnd, native->hFont);
                // scale initial size
                int sw = iround(w * native->scale);
                int sh = iround(h * native->scale);
                SetWindowPos(hwnd, nullptr, 0, 0, sw, sh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
#endif
            // bind and run optional init(name, ...) only if 'new' provided arguments
            if (ne.typeName != "Window") if (const Action* init = find_entity_method(*ent, "init")) {
                if (!ne.args.empty()) {
                    Env selfEnv;
                    // bind params from args
                    for (size_t i=0; i<init->params.size() && i<ne.args.size(); ++i) {
                        selfEnv.vars[init->params[i].name] = eval_string(*ne.args[i], env);
                    }
                    // expose 'self' object by name
                    selfEnv.objects["self"] = obj;
                    ExecContext child;
                    exec_block(init->body, program, child, selfEnv);
                    for (auto& th : child.threads) if (th.joinable()) th.join();
                }
            }
            env.objects[st.name] = obj;
            // also store a string representation
            env.vars[st.name] = st.name;
        } else {
            std::string v = eval_string(*st.value, env);
            env.vars[st.name] = v;
            if (globalNames_.count(st.name)) globalVars_[st.name] = v;
        }
        return;
    }
    if (std::holds_alternative<ReturnStmt>(s)) {
        const auto& rs = std::get<ReturnStmt>(s);
        // Evaluate return expression if provided
        try {
            std::string rv;
            if (rs.value && *rs.value) rv = eval_string(**rs.value, env); else rv.clear();
            ctx.returned = true;
            ctx.returnValue = rv;
        } catch (...) { }
        return;
    }
    if (std::holds_alternative<FireStmt>(s)) {
        const auto& st = std::get<FireStmt>(s);
        if (const Hook* h = find_hook(program, st.name)) exec_block(h->body, program, ctx, env);
        return;
    }
    if (std::holds_alternative<IfStmt>(s)) {
        const auto& st = std::get<IfStmt>(s);
        std::string c = eval_string(*st.cond, env);
        bool truthy = is_truthy(c);
        if (truthy) exec_block(*st.thenBlk, program, ctx, env);
        else if (st.elseBlk) exec_block(*st.elseBlk, program, ctx, env);
        return;
    }
    if (std::holds_alternative<WhileStmt>(s)) {
        const auto& st = std::get<WhileStmt>(s);
        while (is_truthy(eval_string(*st.cond, env))) {
            exec_block(*st.body, program, ctx, env);
        }
        return;
    }
    if (std::holds_alternative<ForStmt>(s)) {
        const auto& st = std::get<ForStmt>(s);
        // init
        if (st.init) exec_block(*st.init, program, ctx, env);
        while (true) {
            if (st.cond) {
                std::string c = eval_string(**st.cond, env);
                if (!is_truthy(c)) break;
            }
            exec_block(*st.body, program, ctx, env);
            if (st.step) exec_block(*st.step, program, ctx, env);
        }
        return;
    }
    if (std::holds_alternative<ForInStmt>(s)) {
        const auto& st = std::get<ForInStmt>(s);
        if (!st.usedColon) {
            throw std::runtime_error("For-each iteration must use ':' syntax");
        }
        std::string iter = eval_string(*st.iterable, env);
        if (iter.rfind("list:", 0) == 0) {
            if (st.valueVar) {
                throw std::runtime_error("List iteration supports only one variable: for (item : list)");
            }
            int id = to_int(iter.substr(5));
            auto it = g_lists.find(id);
            if (it != g_lists.end()) {
                for (const auto& item : it->second) {
                    env.vars[st.var] = item;
                    exec_block(*st.body, program, ctx, env);
                }
            }
        } else if (iter.rfind("dict:", 0) == 0) {
            int id = to_int(iter.substr(5));
            auto it = g_dicts.find(id);
            if (it != g_dicts.end()) {
                for (const auto& kv : it->second) {
                    if (st.valueVar) {
                        env.vars[st.var] = kv.first;
                        env.vars[*st.valueVar] = kv.second;
                    } else {
                        env.vars[st.var] = kv.first;
                    }
                    exec_block(*st.body, program, ctx, env);
                }
            }
        }
        return;
    }
    if (std::holds_alternative<TryCatchStmt>(s)) {
        const auto& st = std::get<TryCatchStmt>(s);
        try {
            exec_block(*st.tryBlk, program, ctx, env);
        } catch (const std::exception& ex) {
            env.vars[st.catchVar] = ex.what();
            exec_block(*st.catchBlk, program, ctx, env);
        }
        return;
    }
    if (std::holds_alternative<SwitchStmt>(s)) {
        const auto& sw = std::get<SwitchStmt>(s);
        std::string sel = eval_string(*sw.selector, env);
        bool matched = false;
        for (const auto& c : sw.cases) {
            if (c.value == sel) { exec_block(*c.body, program, ctx, env); matched = true; break; }
        }
        if (!matched && sw.defaultBlk) exec_block(*sw.defaultBlk, program, ctx, env);
        return;
    }
    if (std::holds_alternative<SetStmt>(s)) {
        const auto& st = std::get<SetStmt>(s);
        if (st.isMember) {
            if (resolve_builtin_module_method(program, st.objectName, st.varOrField).has_value()) {
                throw std::runtime_error("Cannot assign to builtin module alias: " + st.objectName + "." + st.varOrField);
            }
            auto it = env.objects.find(st.objectName);
            if (it == env.objects.end()) {
                auto structIt = env.vars.find(st.objectName);
                if (structIt != env.vars.end() && structIt->second.rfind("struct:", 0) == 0) {
                    std::string val = eval_string(*st.value, env);
                    env.vars[st.objectName + "." + st.varOrField] = val;
                    return;
                }
                throw std::runtime_error("Unknown object: " + st.objectName);
            }
            std::string val = eval_string(*st.value, env);
            it->second->fields[st.varOrField] = val;
            if (st.objectName == "self") {
                env.vars[st.varOrField] = val;
            }
        } else {
            std::string v = eval_string(*st.value, env);
            env.vars[st.varOrField] = v;
            if (globalNames_.count(st.varOrField)) globalVars_[st.varOrField] = v;
        }
        return;
    }
    if (std::holds_alternative<MethodCallStmt>(s)) {
        const auto& mc = std::get<MethodCallStmt>(s);
        if (auto moduleBuiltin = env.vars.find(mc.objectName + "." + mc.method); moduleBuiltin != env.vars.end()) {
            if (moduleBuiltin->second.rfind(kBuiltinAliasPrefix.data(), 0) == 0) {
                env.vars["_"] = eval_builtin_call(mc.objectName + "." + mc.method, mc.args, env);
                return;
            }
        }
        if (auto builtinTarget = resolve_builtin_module_method(program, mc.objectName, mc.method); builtinTarget.has_value()) {
            env.vars["_"] = eval_builtin_call(*builtinTarget, mc.args, env);
            return;
        }
        // Support dynamic list/dict method calls using handles stored in variables
        auto vhit = env.vars.find(mc.objectName);
        if (vhit != env.vars.end()) {
            const std::string& handle = vhit->second;
            if (handle.rfind("list:", 0) == 0 && mc.method == "forEach") {
                int id = to_int(handle.substr(5));
                // forEach(actionName)
                std::string actionName = eval_string(*mc.args[0], env);
                for (const auto& item : g_lists[id]) {
                    if (const Action* a = find_action(program, actionName)) {
                        Env callee; callee.vars["item"] = item; exec_block(a->body, program, ctx, callee);
                    }
                }
                return;
            }
            if (handle.rfind("list:", 0) == 0 && mc.method == "push") {

                int id = to_int(handle.substr(5));
                if (!mc.args.empty()) {
                    std::string v = eval_string(*mc.args[0], env);
                    g_lists[id].push_back(v);
                }
                return;
            }
            if (handle.rfind("list:", 0) == 0 && mc.method == "get") {
                int id = to_int(handle.substr(5));
                if (!mc.args.empty()) {
                    int idx = to_int(eval_string(*mc.args[0], env));
                    auto& vec = g_lists[id];
                    if (idx >= 0 && idx < (int)vec.size()) {
                        // Write into a special var `_` to return a value (printing via print `_`)
                        env.vars["_"] = vec[idx];
                    } else {
                        env.vars["_"] = std::string();
                    }
                }
                return;
            }
            if (handle.rfind("list:", 0) == 0 && mc.method == "len") {
                int id = to_int(handle.substr(5));
                env.vars["_"] = std::to_string((int)g_lists[id].size());
                return;
            }
            if (handle.rfind("dict:", 0) == 0 && mc.method == "forEach") {
                int id = to_int(handle.substr(5));
                std::string actionName = eval_string(*mc.args[0], env);
                for (const auto& kv : g_dicts[id]) {
                    if (const Action* a = find_action(program, actionName)) {
                        Env callee; callee.vars["key"] = kv.first; callee.vars["value"] = kv.second; exec_block(a->body, program, ctx, callee);
                    }
                }
                return;
            }
            if (handle.rfind("dict:", 0) == 0 && mc.method == "set") {
                int id = to_int(handle.substr(5));
                if (mc.args.size() >= 2) {
                    std::ostringstream key;
                    for (size_t i = 0; i + 1 < mc.args.size(); ++i) {
                        if (i) key << '.';
                        key << eval_string(*mc.args[i], env);
                    }
                    std::string k = key.str();
                    std::string v = eval_string(*mc.args[mc.args.size() - 1], env);
                    g_dicts[id][k] = v;
                }
                return;
            }
            if (handle.rfind("dict:", 0) == 0 && mc.method == "get") {
                int id = to_int(handle.substr(5));
                if (!mc.args.empty()) {
                    std::ostringstream key;
                    for (size_t i = 0; i < mc.args.size(); ++i) {
                        if (i) key << '.';
                        key << eval_string(*mc.args[i], env);
                    }
                    std::string k = key.str();
                    auto it = g_dicts[id].find(k);
                    env.vars["_"] = (it != g_dicts[id].end()) ? it->second : std::string();
                }
                return;
            }
            if (handle.rfind("dict:", 0) == 0 && mc.method == "has") {
                int id = to_int(handle.substr(5));
                if (!mc.args.empty()) {
                    std::ostringstream key;
                    for (size_t i = 0; i < mc.args.size(); ++i) {
                        if (i) key << '.';
                        key << eval_string(*mc.args[i], env);
                    }
                    std::string k = key.str();
                    env.vars["_"] = (g_dicts[id].count(k) ? "true" : "false");
                }
                return;
            }
            if (handle.rfind("dict:", 0) == 0 && mc.method == "getOr") {
                int id = to_int(handle.substr(5));
                if (mc.args.size() >= 2) {
                    std::ostringstream key;
                    for (size_t i = 0; i + 1 < mc.args.size(); ++i) {
                        if (i) key << '.';
                        key << eval_string(*mc.args[i], env);
                    }
                    std::string k = key.str();
                    std::string def = eval_string(*mc.args[mc.args.size() - 1], env);
                    auto it = g_dicts[id].find(k);
                    env.vars["_"] = (it != g_dicts[id].end()) ? it->second : def;
                }
                return;
            }
            if (handle.rfind("dict:", 0) == 0 && mc.method == "remove") {
                int id = to_int(handle.substr(5));
                if (!mc.args.empty()) {
                    std::ostringstream key;
                    for (size_t i = 0; i < mc.args.size(); ++i) {
                        if (i) key << '.';
                        key << eval_string(*mc.args[i], env);
                    }
                    std::string k = key.str();
                    env.vars["_"] = g_dicts[id].erase(k) ? "true" : "false";
                }
                return;
            }
            if (handle.rfind("dict:", 0) == 0 && mc.method == "clear") {
                int id = to_int(handle.substr(5));
                g_dicts[id].clear();
                return;
            }
            if (handle.rfind("dict:", 0) == 0 && mc.method == "size") {
                int id = to_int(handle.substr(5));
                env.vars["_"] = std::to_string((int)g_dicts[id].size());
                return;
            }
            if (handle.rfind("dict:", 0) == 0 && mc.method == "keys") {
                int id = to_int(handle.substr(5));
                int lid = g_nextListId++;
                g_lists[lid] = {};
                for (const auto& kv : g_dicts[id]) g_lists[lid].push_back(kv.first);
                env.vars["_"] = std::string("list:") + std::to_string(lid);
                return;
            }
            if (handle.rfind("dict:", 0) == 0 && mc.method == "values") {
                int id = to_int(handle.substr(5));
                int lid = g_nextListId++;
                g_lists[lid] = {};
                for (const auto& kv : g_dicts[id]) g_lists[lid].push_back(kv.second);
                env.vars["_"] = std::string("list:") + std::to_string(lid);
                return;
            }
            if (handle.rfind("dict:", 0) == 0 && mc.method == "merge") {
                int id = to_int(handle.substr(5));
                if (!mc.args.empty()) {
                    std::string otherHandle = eval_string(*mc.args[0], env);
                    if (otherHandle.rfind("dict:", 0) == 0) {
                        int sourceId = to_int(otherHandle.substr(5));
                        for (const auto& kv : g_dicts[sourceId]) {
                            g_dicts[id][kv.first] = kv.second;
                        }
                    }
                }
                return;
            }
        }
        auto it = env.objects.find(mc.objectName);
        if (it == env.objects.end()) throw std::runtime_error("Unknown object: " + mc.objectName);
        ObjPtr obj = it->second;
        // native-backed Window entity methods
#ifdef _WIN32
        if (obj->typeName == "Window") {
            HINSTANCE hInst = GetModuleHandleW(nullptr);
            auto* native = static_cast<NativeWin*>(obj->native);
            if (mc.method == "button") {
                if (!native) { native = new NativeWin(); obj->native = native; }
                std::string idName = eval_string(*mc.args[0], env);
                std::string s1 = eval_string(*mc.args[1], env);
                std::wstring text(s1.begin(), s1.end());
                int x = to_int(eval_string(*mc.args[2], env));
                int y = to_int(eval_string(*mc.args[3], env));
                int w = to_int(eval_string(*mc.args[4], env));
                int h = to_int(eval_string(*mc.args[5], env));
                int id = native->nextId++;
                native->idToName[id] = idName;
                auto S = [&](int v){ return iround(v * native->scale); };
                HWND ch = CreateWindowExW(0, L"BUTTON", text.c_str(), WS_TABSTOP|WS_VISIBLE|WS_CHILD|BS_DEFPUSHBUTTON,
                    S(x), S(y), S(w), S(h), native->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
                apply_font_for(ch, native->hFont);
                RedrawWindow(native->hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
                return;
            }
            if (mc.method == "checkbox") {
                if (!native) { native = new NativeWin(); obj->native = native; }
                std::string idName = eval_string(*mc.args[0], env);
                std::string s1 = eval_string(*mc.args[1], env);
                std::wstring text(s1.begin(), s1.end());
                int x = to_int(eval_string(*mc.args[2], env));
                int y = to_int(eval_string(*mc.args[3], env));
                int w = to_int(eval_string(*mc.args[4], env));
                int h = to_int(eval_string(*mc.args[5], env));
                int id = native->nextId++;
                native->idToName[id] = idName; native->nameToId[idName] = id;
                auto S = [&](int v){ return iround(v * native->scale); };
                HWND ch = CreateWindowExW(0, L"BUTTON", text.c_str(), WS_TABSTOP|WS_VISIBLE|WS_CHILD|BS_AUTOCHECKBOX,
                    S(x), S(y), S(w), S(h), native->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
                native->idToHwnd[id] = ch; apply_font_for(ch, native->hFont);
                RedrawWindow(native->hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
                return;
            }
            if (mc.method == "radio") {
                if (!native) { native = new NativeWin(); obj->native = native; }
                std::string idName = eval_string(*mc.args[0], env);
                std::string s1 = eval_string(*mc.args[1], env);
                std::wstring text(s1.begin(), s1.end());
                int x = to_int(eval_string(*mc.args[2], env));
                int y = to_int(eval_string(*mc.args[3], env));
                int w = to_int(eval_string(*mc.args[4], env));
                int h = to_int(eval_string(*mc.args[5], env));
                bool groupStart = false; if (mc.args.size() > 6) { std::string g = eval_string(*mc.args[6], env); groupStart = (g == "true" || to_int(g) != 0); }
                int id = native->nextId++;
                native->idToName[id] = idName; native->nameToId[idName] = id;
                DWORD style = WS_TABSTOP|WS_VISIBLE|WS_CHILD|BS_AUTORADIOBUTTON; if (groupStart) style |= WS_GROUP;
                auto S = [&](int v){ return iround(v * native->scale); };
                HWND ch = CreateWindowExW(0, L"BUTTON", text.c_str(), style,
                    S(x), S(y), S(w), S(h), native->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
                native->idToHwnd[id] = ch; apply_font_for(ch, native->hFont);
                RedrawWindow(native->hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
                return;
            }
            if (mc.method == "slider") {
                if (!native) { native = new NativeWin(); obj->native = native; }
                std::string idName = eval_string(*mc.args[0], env);
                int x = to_int(eval_string(*mc.args[1], env));
                int y = to_int(eval_string(*mc.args[2], env));
                int w = to_int(eval_string(*mc.args[3], env));
                int h = to_int(eval_string(*mc.args[4], env));
                int minv = to_int(eval_string(*mc.args[5], env));
                int maxv = to_int(eval_string(*mc.args[6], env));
                int val = to_int(eval_string(*mc.args[7], env));
                int id = native->nextId++;
                native->idToName[id] = idName; native->nameToId[idName] = id;
                auto S = [&](int v){ return iround(v * native->scale); };
                HWND ch = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_VISIBLE|WS_CHILD|TBS_AUTOTICKS,
                    S(x), S(y), S(w), S(h), native->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
                SendMessageW(ch, TBM_SETRANGEMIN, TRUE, minv);
                SendMessageW(ch, TBM_SETRANGEMAX, TRUE, maxv);
                SendMessageW(ch, TBM_SETPOS, TRUE, val);
                native->idToHwnd[id] = ch; apply_font_for(ch, native->hFont);
                RedrawWindow(native->hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
                return;
            }
            if (mc.method == "label") {
                if (!native) { native = new NativeWin(); obj->native = native; }
                std::string s0 = eval_string(*mc.args[0], env);
                std::wstring text(s0.begin(), s0.end());
                int x = to_int(eval_string(*mc.args[1], env));
                int y = to_int(eval_string(*mc.args[2], env));
                int w = to_int(eval_string(*mc.args[3], env));
                int h = to_int(eval_string(*mc.args[4], env));
                auto S = [&](int v){ return iround(v * native->scale); };
                HWND ch = CreateWindowExW(0, L"STATIC", text.c_str(), WS_VISIBLE|WS_CHILD,
                    S(x), S(y), S(w), S(h), native->hwnd, nullptr, hInst, nullptr);
                apply_font_for(ch, native->hFont);
                RedrawWindow(native->hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
                return;
            }
            if (mc.method == "setScale") {
                if (!native) { native = new NativeWin(); obj->native = native; }
                int pct = to_int(eval_string(*mc.args[0], env)); if (pct <= 0) pct = 100;
                native->scale = pct / 100.0;
                if (native->hFont) { DeleteObject(native->hFont); native->hFont = nullptr; }
                native->hFont = create_font_for_scale(native->scale);
                for (const auto& kv : native->idToHwnd) apply_font_for(kv.second, native->hFont);
                return;
            }
            if (mc.method == "nativeMessageBox") {
                if (!native) return;
                if (mc.args.size() < 2) return; // need at least title + message
                std::string t = eval_string(*mc.args[0], env);
                std::string m = eval_string(*mc.args[1], env);
                UINT icon = MB_ICONINFORMATION;
                if (mc.args.size() >= 3) {
                    std::string kind = eval_string(*mc.args[2], env);
                    for (auto & c : kind) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
                    if (kind == "warn" || kind == "warning") icon = MB_ICONWARNING; // alias MB_ICONEXCLAMATION
                    else if (kind == "error" || kind == "err" || kind == "fatal") icon = MB_ICONERROR;
                    else if (kind == "question" || kind == "ask" || kind == "qry") icon = MB_ICONQUESTION;
                    else if (kind == "info" || kind == "information" || kind == "i") icon = MB_ICONINFORMATION; // default
                }
                std::wstring title(t.begin(), t.end());
                std::wstring msg(m.begin(), m.end());
                HWND hwnd = native->hwnd;
                MessageBoxW((hwnd ? hwnd : nullptr), msg.c_str(), title.c_str(), MB_OK | icon);
                return;
            }
            if (mc.method == "onClick") {
                if (!native) { native = new NativeWin(); obj->native = native; }
                std::string idName = eval_string(*mc.args[0], env);
                std::string action = eval_string(*mc.args[1], env);
                for (const auto& kv : native->idToName) if (kv.second == idName) native->idToAction[kv.first] = action;
                return;
            }
            if (mc.method == "onChange") {
                if (!native) { native = new NativeWin(); obj->native = native; }
                std::string idName = eval_string(*mc.args[0], env);
                std::string action = eval_string(*mc.args[1], env);
                for (const auto& kv : native->idToName) if (kv.second == idName) native->idToAction[kv.first] = action;
                return;
            }
            if (mc.method == "show") {
                HWND hwnd = static_cast<NativeWin*>(obj->native)->hwnd;
                ShowWindow(hwnd, SW_SHOWNORMAL);
                ShowWindow(hwnd, SW_RESTORE);
                UpdateWindow(hwnd);
                SetForegroundWindow(hwnd);
                RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
                MSG msg; while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
                return;
            }
            if (mc.method == "close") {
                auto* native = static_cast<NativeWin*>(obj->native);
                if (native && native->hwnd) PostMessageW(native->hwnd, WM_CLOSE, 0, 0);
                return;
            }
        }
#endif
    // fallback to scripted entity methods
        const Entity* ent = find_entity(program, obj->typeName);
        if (!ent) throw std::runtime_error("Entity type not found: " + obj->typeName);
        const Action* meth = find_entity_method(*ent, mc.method);
        if (!meth) throw std::runtime_error("Unknown method: " + mc.method);
        // Enforce visibility in strict mode for scripted entity methods
        if (program.strict && meth->visibility != Visibility::Public && mc.objectName != "self") {
            throw std::runtime_error("Method not visible: " + mc.method);
        }
        // Hidden enforcement: if method has @hidden, allow only when caller is self
        bool isHidden = false;
        for (const auto& at : meth->attributes) if (at.name == "hidden") { isHidden = true; break; }
        if (isHidden && mc.objectName != "self") {
            throw std::runtime_error("Method hidden: " + mc.method);
        }
        Env callEnv;
        // Seed with current shared globals
        for (const auto& kv : globalVars_) callEnv.vars[kv.first] = kv.second;
        // bind positional args into method params
        for (size_t i=0; i<meth->params.size() && i<mc.args.size(); ++i) {
            callEnv.vars[meth->params[i].name] = eval_string(*mc.args[i], env);
        }
        // expose 'self' with fields accessible as variables
        callEnv.objects["self"] = obj;
        for (const auto& kv : obj->fields) callEnv.vars[kv.first] = kv.second;
        ExecContext child;
        exec_block(meth->body, program, child, callEnv);
        for (auto& th : child.threads) if (th.joinable()) th.join();
        // propagate any changed fields back to object
        for (auto& f : obj->fields) {
            auto vit = callEnv.vars.find(f.first);
            if (vit != callEnv.vars.end()) f.second = vit->second;
        }
        return;
    }
    if (std::holds_alternative<ActionCallStmt>(s)) {
        const auto& call = std::get<ActionCallStmt>(s);
        if (const Action* a = find_action(program, call.name)) {
            if (program.strict && a->visibility != Visibility::Public) {
                throw std::runtime_error("Action not public: " + a->name);
            }
            Env calleeEnv;
            // Seed with current shared globals
            for (const auto& kv : globalVars_) calleeEnv.vars[kv.first] = kv.second;
            // Bind positional args into parameter names
            for (size_t i=0; i<a->params.size() && i<call.args.size(); ++i) {
                calleeEnv.vars[a->params[i].name] = eval_string(*call.args[i], env);
            }
            exec_block(a->body, program, ctx, calleeEnv);
        }
        else {
            // Fallback: treat as built-in call with side effects
            (void)eval_builtin_call(call.name, call.args, env);
        }
        return;
    }
}

void Runtime::exec_block(const Block& b, const Program& program, ExecContext& ctx, Env& env) const {
    for (const auto& st : b.stmts) exec_stmt(st, program, ctx, env);
}

int Runtime::run(const Program& program) const {
    currentProgram_ = &program;
    std::string entry = program.runTarget.value_or("main");
    const Action* a = find_action(program, entry);
    if (!a) throw std::runtime_error("Action not found: " + entry);
    if (program.strict && a->visibility != Visibility::Public) {
        throw std::runtime_error("Entry action not public: " + a->name);
    }
    ExecContext ctx;
    Env rootEnv;
    // Seed program-level globals into shared store and initial root env
    globalNames_.clear();
    for (const auto& g : program.globals) globalNames_.insert(g.name);
    for (const auto& g : program.globals) {
        if (g.value) globalVars_[g.name] = eval_string(*g.value, rootEnv);
        rootEnv.vars[g.name] = globalVars_[g.name];
    }
    // Seed file-level directives into env (simple string values); apply special ones
    for (const auto& d : program.directives) {
        if (d.value) rootEnv.vars[d.name] = *d.value; else rootEnv.vars[d.name] = "true";
    }
    auto seedPluginAliases = [&](Env& targetEnv) {
        if (program.pluginAliases.empty() || pluginRecords_.empty()) return;
        auto dedupe_sort = [](std::vector<std::string>& items) {
            std::sort(items.begin(), items.end());
            items.erase(std::unique(items.begin(), items.end()), items.end());
        };
        for (const auto& alias : program.pluginAliases) {
            std::vector<std::string> slugs;
            slugs.reserve(pluginRecords_.size());
            std::vector<std::string> allCoreFiles;
            std::unordered_map<std::string, std::vector<std::string>> coreKeysByFile;
            for (const auto& plugin : pluginRecords_) {
                slugs.push_back(plugin.slug);
                std::string key = alias + ":" + plugin.slug;
                auto obj = std::make_shared<Object>();
                obj->typeName = "Plugin";
                obj->fields["id"] = plugin.id;
                obj->fields["slug"] = plugin.slug;
                obj->fields["name"] = plugin.name;
                obj->fields["version"] = plugin.version;
                obj->fields["author"] = plugin.author;
                obj->fields["target"] = plugin.target;
                obj->fields["description"] = plugin.description;
                obj->fields["dependencies"] = join_strings(plugin.dependencies);
                obj->fields["base_directory"] = plugin.baseDirectory.string();
                obj->fields["manifest_path"] = plugin.manifestPath.string();
                obj->fields["on_load"] = plugin.onLoad;
                obj->fields["on_unload"] = plugin.onUnload;
                obj->fields["data_hook"] = plugin.dataHook;
                if (!plugin.hookBindings.empty()) {
                    std::vector<std::string> hookNames;
                    hookNames.reserve(plugin.hookBindings.size());
                    for (const auto& [hookName, actions] : plugin.hookBindings) {
                        hookNames.push_back(hookName);
                        obj->fields["hook." + hookName] = join_preserving_order(actions);
                        obj->fields["hook." + hookName + ".count"] = std::to_string(actions.size());
                    }
                    std::sort(hookNames.begin(), hookNames.end());
                    obj->fields["hooks"] = join_preserving_order(hookNames);
                    obj->fields["hooks.count"] = std::to_string(hookNames.size());
                }
                if (plugin.dslSpec) {
                    const auto& lang = *plugin.dslSpec;
                    obj->fields["language.id"] = lang.id;
                    obj->fields["language.name"] = lang.name;
                    obj->fields["language.version"] = lang.version;
                    if (!lang.extensions.empty()) {
                        obj->fields["language.extensions"] = join_preserving_order(lang.extensions);
                        obj->fields["language.extensions.count"] = std::to_string(lang.extensions.size());
                    }
                    if (!lang.keywordAliases.empty()) {
                        std::vector<std::string> aliasKeys;
                        aliasKeys.reserve(lang.keywordAliases.size());
                        for (const auto& [alias, canonical] : lang.keywordAliases) {
                            aliasKeys.push_back(alias);
                            obj->fields["language.alias." + alias] = canonical;
                        }
                        std::sort(aliasKeys.begin(), aliasKeys.end());
                        obj->fields["language.aliases"] = join_preserving_order(aliasKeys);
                        obj->fields["language.aliases.count"] = std::to_string(aliasKeys.size());
                    }
                }
                for (const auto& [fileName, entries] : plugin.coreProperties) {
                    allCoreFiles.push_back(fileName);
                    auto& bucket = coreKeysByFile[fileName];
                    bucket.reserve(bucket.size() + entries.size());
                    for (const auto& [entryKey, entryValue] : entries) {
                        bucket.push_back(entryKey);
                        obj->fields["core:" + fileName + ":" + entryKey] = entryValue;
                    }
                }
                targetEnv.objects[key] = std::move(obj);
            }
            targetEnv.vars[alias + ".slugs"] = join_strings(slugs);
            targetEnv.vars[alias + ".count"] = std::to_string(slugs.size());
            if (!allCoreFiles.empty()) {
                dedupe_sort(allCoreFiles);
                targetEnv.vars[alias + ".core.files"] = join_strings(allCoreFiles);
                for (auto& kv : coreKeysByFile) {
                    dedupe_sort(kv.second);
                    targetEnv.vars[alias + ".core." + kv.first + ".keys"] = join_strings(kv.second);
                }
            }
        }
    };
    seedPluginAliases(rootEnv);
    bind_builtin_module_aliases(program, rootEnv.vars);
#ifdef _WIN32
    // Apply default GUI event handler from @event(name) if present
    for (const auto& d : program.directives) {
        if ((d.name == "event") && d.value) { g_eventActionName = *d.value; break; }
    }
#endif
    auto dispatchPluginHooks = [&](std::string_view hookName, bool reverseOrder) {
        if (pluginRecords_.empty()) {
            return;
        }
        const std::string canonical = lowercase_ascii_copy(hookName);
        auto invoke = [&](const PluginRecord& plugin) {
            const auto& actions = hook_actions_for(plugin, canonical);
            if (actions.empty()) {
                return;
            }
            Env envCopy;
            for (const auto& kv : globalVars_) {
                envCopy.vars[kv.first] = kv.second;
            }
            seedPluginAliases(envCopy);
            bind_builtin_module_aliases(program, envCopy.vars);
            for (const auto& actionName : actions) {
                if (actionName.empty()) {
                    continue;
                }
                if (const Action* action = find_action(program, actionName)) {
                    ExecContext hookCtx;
                    exec_block(action->body, program, hookCtx, envCopy);
                    join_threads(hookCtx.threads);
                } else {
                    std::cerr << "[plugins] hook '" << canonical << "' action '" << actionName << "' not found for plugin '" << plugin.id << "'\n";
                }
            }
            for (auto& kv : envCopy.vars) {
                if (globalNames_.count(kv.first)) {
                    globalVars_[kv.first] = kv.second;
                }
            }
        };
        if (reverseOrder) {
            for (auto it = pluginRecords_.rbegin(); it != pluginRecords_.rend(); ++it) {
                invoke(*it);
            }
        } else {
            for (const auto& plugin : pluginRecords_) {
                invoke(plugin);
            }
        }
    };

    dispatchPluginHooks("datahook", false);
    dispatchPluginHooks("onload", false);
    dispatchPluginHooks("onstart", false);

    if (const Hook* s = find_hook(program, "onStart")) {
        exec_block(s->body, program, ctx, rootEnv);
    }

    exec_block(a->body, program, ctx, rootEnv);
    join_threads(ctx.threads);

    // Support both onEnd (preferred) and onExit (alias). Use the same rootEnv so variables persist.
    if (const Hook* e = find_hook(program, "onEnd"); e || (e = find_hook(program, "onExit"))) {
        ExecContext after;
        exec_block(e->body, program, after, rootEnv);
        join_threads(after.threads);
    }

    dispatchPluginHooks("onend", true);
    dispatchPluginHooks("onexit", true);
    dispatchPluginHooks("onunload", true);

    currentProgram_ = nullptr;
    return 0;
}

int Runtime::run_single_action(const Program& program, std::string_view actionName) const {
    currentProgram_ = &program;
    const Action* a = find_action(program, actionName);
    if (!a) { currentProgram_ = nullptr; return 1; }
    if (program.strict && a->visibility != Visibility::Public) { currentProgram_ = nullptr; return 2; }
    ExecContext ctx; Env env;
    auto seedPluginAliases = [&](Env& targetEnv) {
        if (program.pluginAliases.empty() || pluginRecords_.empty()) return;
        auto dedupe_sort = [](std::vector<std::string>& items) {
            std::sort(items.begin(), items.end());
            items.erase(std::unique(items.begin(), items.end()), items.end());
        };
        for (const auto& alias : program.pluginAliases) {
            std::vector<std::string> slugs;
            slugs.reserve(pluginRecords_.size());
            std::vector<std::string> allCoreFiles;
            std::unordered_map<std::string, std::vector<std::string>> coreKeysByFile;
            for (const auto& plugin : pluginRecords_) {
                slugs.push_back(plugin.slug);
                std::string key = alias + ":" + plugin.slug;
                auto obj = std::make_shared<Object>();
                obj->typeName = "Plugin";
                obj->fields["id"] = plugin.id;
                obj->fields["slug"] = plugin.slug;
                obj->fields["name"] = plugin.name;
                obj->fields["version"] = plugin.version;
                obj->fields["author"] = plugin.author;
                obj->fields["target"] = plugin.target;
                obj->fields["description"] = plugin.description;
                obj->fields["dependencies"] = join_strings(plugin.dependencies);
                obj->fields["base_directory"] = plugin.baseDirectory.string();
                obj->fields["manifest_path"] = plugin.manifestPath.string();
                obj->fields["on_load"] = plugin.onLoad;
                obj->fields["on_unload"] = plugin.onUnload;
                obj->fields["data_hook"] = plugin.dataHook;
                if (!plugin.hookBindings.empty()) {
                    std::vector<std::string> hookNames;
                    hookNames.reserve(plugin.hookBindings.size());
                    for (const auto& [hookName, actions] : plugin.hookBindings) {
                        hookNames.push_back(hookName);
                        obj->fields["hook." + hookName] = join_preserving_order(actions);
                        obj->fields["hook." + hookName + ".count"] = std::to_string(actions.size());
                    }
                    std::sort(hookNames.begin(), hookNames.end());
                    obj->fields["hooks"] = join_preserving_order(hookNames);
                    obj->fields["hooks.count"] = std::to_string(hookNames.size());
                }
                if (plugin.dslSpec) {
                    const auto& lang = *plugin.dslSpec;
                    obj->fields["language.id"] = lang.id;
                    obj->fields["language.name"] = lang.name;
                    obj->fields["language.version"] = lang.version;
                    if (!lang.extensions.empty()) {
                        obj->fields["language.extensions"] = join_preserving_order(lang.extensions);
                        obj->fields["language.extensions.count"] = std::to_string(lang.extensions.size());
                    }
                    if (!lang.keywordAliases.empty()) {
                        std::vector<std::string> aliasKeys;
                        aliasKeys.reserve(lang.keywordAliases.size());
                        for (const auto& [alias, canonical] : lang.keywordAliases) {
                            aliasKeys.push_back(alias);
                            obj->fields["language.alias." + alias] = canonical;
                        }
                        std::sort(aliasKeys.begin(), aliasKeys.end());
                        obj->fields["language.aliases"] = join_preserving_order(aliasKeys);
                        obj->fields["language.aliases.count"] = std::to_string(aliasKeys.size());
                    }
                }
                for (const auto& [fileName, entries] : plugin.coreProperties) {
                    allCoreFiles.push_back(fileName);
                    auto& bucket = coreKeysByFile[fileName];
                    bucket.reserve(bucket.size() + entries.size());
                    for (const auto& [entryKey, entryValue] : entries) {
                        bucket.push_back(entryKey);
                        obj->fields["core:" + fileName + ":" + entryKey] = entryValue;
                    }
                }
                targetEnv.objects[key] = std::move(obj);
            }
            targetEnv.vars[alias + ".slugs"] = join_strings(slugs);
            targetEnv.vars[alias + ".count"] = std::to_string(slugs.size());
            if (!allCoreFiles.empty()) {
                dedupe_sort(allCoreFiles);
                targetEnv.vars[alias + ".core.files"] = join_strings(allCoreFiles);
                for (auto& kv : coreKeysByFile) {
                    dedupe_sort(kv.second);
                    targetEnv.vars[alias + ".core." + kv.first + ".keys"] = join_strings(kv.second);
                }
            }
        }
    };
    seedPluginAliases(env);
    bind_builtin_module_aliases(program, env.vars);
    auto dispatchPluginHooks = [&](std::string_view hookName, bool reverseOrder) {
        if (pluginRecords_.empty()) {
            return;
        }
        const std::string canonical = lowercase_ascii_copy(hookName);
        auto invoke = [&](const PluginRecord& plugin) {
            const auto& actions = hook_actions_for(plugin, canonical);
            if (actions.empty()) {
                return;
            }
            Env envCopy;
            for (const auto& kv : globalVars_) {
                envCopy.vars[kv.first] = kv.second;
            }
            seedPluginAliases(envCopy);
            bind_builtin_module_aliases(program, envCopy.vars);
            for (const auto& actionName : actions) {
                if (actionName.empty()) {
                    continue;
                }
                if (const Action* action = find_action(program, actionName)) {
                    ExecContext hookCtx;
                    exec_block(action->body, program, hookCtx, envCopy);
                    join_threads(hookCtx.threads);
                } else {
                    std::cerr << "[plugins] hook '" << canonical << "' action '" << actionName << "' not found for plugin '" << plugin.id << "'\n";
                }
            }
            for (auto& kv : envCopy.vars) {
                if (globalNames_.count(kv.first)) {
                    globalVars_[kv.first] = kv.second;
                }
            }
        };
        if (reverseOrder) {
            for (auto it = pluginRecords_.rbegin(); it != pluginRecords_.rend(); ++it) {
                invoke(*it);
            }
        } else {
            for (const auto& plugin : pluginRecords_) {
                invoke(plugin);
            }
        }
    };
    // Lazy one-time global initializer evaluation (if we haven't populated names or vars yet)
    if (globalNames_.empty()) {
        for (const auto& g : program.globals) globalNames_.insert(g.name);
        for (const auto& g : program.globals) {
            if (g.value) {
                try { globalVars_[g.name] = eval_string(*g.value, env); }
                catch (...) { globalVars_[g.name] = ""; }
            } else {
                globalVars_[g.name] = "";
            }
        }
        dispatchPluginHooks("datahook", false);
        dispatchPluginHooks("onload", false);
        dispatchPluginHooks("onstart", false);
        // Run onStart hook once if present
        if (const Hook* s = find_hook(program, "onStart")) {
            ExecContext sc; Env se; for (const auto& kv : globalVars_) se.vars[kv.first] = kv.second; exec_block(s->body, program, sc, se); for (auto& th : sc.threads) if (th.joinable()) th.join();
            // propagate any mutated globals back
            for (auto& kv : se.vars) if (globalNames_.count(kv.first)) globalVars_[kv.first] = kv.second;
        }
    }
    for (const auto& kv : globalVars_) env.vars[kv.first] = kv.second;
    seedPluginAliases(env);
    bind_builtin_module_aliases(program, env.vars);
    exec_block(a->body, program, ctx, env);
    for (auto& th : ctx.threads) if (th.joinable()) th.join();
    // Persist mutated globals
    for (auto& kv : env.vars) if (globalNames_.count(kv.first)) globalVars_[kv.first] = kv.second;
    currentProgram_ = nullptr;
    return 0;
}

#ifdef _WIN32
void Runtime::handle_gui_click(int id, void* nativeWinPtr) const {
    if (!currentProgram_ || !nativeWinPtr) return;
    auto* nw = static_cast<NativeWin*>(nativeWinPtr);
    const Action* a = nullptr;
    auto it = nw->idToAction.find(id);
    if (it != nw->idToAction.end()) {
        a = find_action(*currentProgram_, it->second);
    } else if (!g_eventActionName.empty()) {
        a = find_action(*currentProgram_, g_eventActionName);
    }
    if (!a) return;
    ExecContext ctx; Env env; env.vars["id"] = nw->idToName[id];
    // Load current globals snapshot for event
    for (const auto& kv : globalVars_) env.vars[kv.first] = kv.second;
    g_currentEventWin = nw;
    // Try to provide the current window handle as a string for scripts
    // by scanning the global handle map.
    for (const auto& kv : g_handleMap) {
        if (kv.second == nw) { env.vars["win"] = std::string("win:") + std::to_string(kv.first); break; }
    }
    exec_block(a->body, *currentProgram_, ctx, env);
    for (auto& th : ctx.threads) if (th.joinable()) th.join();
    g_currentEventWin = nullptr;
}
#endif

} // namespace erelang

#ifdef _WIN32
// Optional explicit shutdown to release global resources
extern "C" __declspec(dllexport) void __erelang_win_shutdown() {
    using namespace erelang;
    // Free global brush if we ever replace with a non-stock brush in future
    // (Currently g_bgBrush uses stock white brush and must not be deleted.)
    if (g_classAtom && !g_obsWinClassName.empty()) {
        HINSTANCE hInst = GetModuleHandleW(nullptr);
        UnregisterClassW(g_obsWinClassName.c_str(), hInst);
    }
}
#endif
