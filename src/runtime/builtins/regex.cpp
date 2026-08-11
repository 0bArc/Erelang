// Basic regex utilities
// Include <regex> before runtime/windows headers to avoid macro interference.
#include <regex>
#include <string>
#include <vector>
#include <sstream>
#include "erelang/runtime.hpp"

namespace erelang {

static std::string regex_dispatch(const std::string& name, const std::vector<std::string>& argv) {
    auto argS = [&](size_t i){ return i<argv.size()?argv[i]:std::string(); };
    // API: re.match(text, pattern), re.find(text, pattern), re.replace(text, pattern, repl)
    // argv = {text, pattern} or {text, pattern, repl}
    if (name == "regex_match") {
        const std::string text = argS(0), pat = argS(1);
        // Full match: the pattern must cover the entire input, not a substring.
        try { std::regex re(pat); return std::regex_match(text, re) ? "true" : "false"; }
        catch (...) { return "false"; }
    }
    if (name == "regex_find") {
        const std::string text = argS(0), pat = argS(1);
        try {
            std::regex re(pat); std::smatch m;
            if (std::regex_search(text, m, re)) { return m.size() > 1 ? m[1].str() : m[0].str(); }
        } catch (...) {}
        return {};
    }
    if (name == "regex_replace") {
        const std::string text = argS(0), pat = argS(1), repl = argS(2);
        try { std::regex re(pat); return std::regex_replace(text, re, repl); }
        catch (...) { return text; }
    }
    return {};
}

std::string __erelang_builtin_regex_dispatch(const std::string& name, const std::vector<std::string>& argv) {
    return regex_dispatch(name, argv);
}

} // namespace erelang
