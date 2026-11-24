// Basic regex utilities
// Include <regex> before runtime/windows headers to avoid macro interference.
#include <regex>
#include <string>
#include <string_view>
#include <vector>
#include <sstream>
#include <unordered_map>
#include <memory>
#include <mutex>
#include "erelang/runtime.hpp"

namespace erelang {

// Static storage for compiled patterns and capture groups
static std::unordered_map<int, std::regex> g_compiledPatterns;
static int g_nextPatternId = 1;
static std::vector<std::string> g_lastCapturedGroups;
static std::mutex g_regexMutex;

// Compiling a std::regex costs far more than matching; scripts call these in loops with the
// same few patterns. Bounded so generated patterns cannot grow it without limit.
static std::shared_ptr<const std::regex> cached_regex(const std::string& pattern) {
    static std::unordered_map<std::string, std::shared_ptr<const std::regex>> cache;
    static std::mutex cacheMutex;
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        if (auto it = cache.find(pattern); it != cache.end()) return it->second;
    }
    auto compiled = std::make_shared<const std::regex>(pattern); // throws std::regex_error
    std::lock_guard<std::mutex> lock(cacheMutex);
    if (cache.size() >= 128) cache.clear();
    cache.emplace(pattern, compiled);
    return compiled;
}

// Expect patterns: a readable pattern language that expands to a regex, so common checks need
// no regex syntax. x = one digit, X = one letter, ? = any one character, * = any run of
// characters (possibly empty), backslash + c = the literal character c. Everything else is literal.
static std::string expect_to_regex(const std::string& pattern) {
    std::string out;
    out.reserve(pattern.size() * 2);
    auto literal = [&](char c) {
        if (std::string_view("\\^$.|?*+()[]{}").find(c) != std::string_view::npos) out += '\\';
        out += c;
    };
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        if (c == '\\' && i + 1 < pattern.size()) { literal(pattern[++i]); }
        else if (c == 'x') out += "[0-9]";
        else if (c == 'X') out += "[A-Za-z]";
        else if (c == '?') out += ".";
        else if (c == '*') out += ".*";
        else literal(c);
    }
    return out;
}

std::string __erelang_intel_regex(const std::string& sample);

static std::string regex_dispatch(const std::string& name, const std::vector<std::string>& argv) {
    auto argS = [&](size_t i){ return i<argv.size()?argv[i]:std::string(); };
    // API: re.match(text, pattern), re.find(text, pattern), re.replace(text, pattern, repl)
    // argv = {text, pattern} or {text, pattern, repl}
    if (name == "regex_match" || name == "re.match") {
        const std::string text = argS(0), pat = argS(1);
        // Full match: the pattern must cover the entire input, not a substring.
        try { const auto re = cached_regex(pat); return std::regex_match(text, *re) ? "true" : "false"; }
        catch (...) { return "false"; }
    }
    // re.expect(pattern, text): whole-text match using the readable expect pattern language.
    // Note the argument order is (pattern, text), the reverse of re.match.
    if (name == "regex_expect" || name == "re.expect") {
        const std::string pat = argS(0), text = argS(1);
        try { const auto re = cached_regex(expect_to_regex(pat)); return std::regex_match(text, *re) ? "true" : "false"; }
        catch (...) { return "false"; }
    }
    // re.infer(sample): compact regex that matches texts shaped like the sample.
    if (name == "regex_infer" || name == "re.infer") {
        return __erelang_intel_regex(argS(0));
    }
    // re.explain(pattern): the regex an expect pattern expands to.
    if (name == "regex_explain" || name == "re.explain") {
        return expect_to_regex(argS(0));
    }
    if (name == "regex_find" || name == "re.find") {
        const std::string text = argS(0), pat = argS(1);
        try {
            const auto re = cached_regex(pat); std::smatch m;
            if (std::regex_search(text, m, *re)) { return m.size() > 1 ? m[1].str() : m[0].str(); }
        } catch (...) {}
        return {};
    }
    if (name == "regex_replace" || name == "re.replace") {
        const std::string text = argS(0), pat = argS(1), repl = argS(2);
        try { const auto re = cached_regex(pat); return std::regex_replace(text, *re, repl); }
        catch (...) { return text; }
    }
    // Find all matches → returns a list handle
    if (name == "regex_find_all" || name == "re.find_all") {
        const std::string text = argS(0), pat = argS(1);
        try {
            const auto re = cached_regex(pat);
            auto begin = std::sregex_iterator(text.begin(), text.end(), *re);
            auto end = std::sregex_iterator();
            int listId = g_nextListId++;
            g_lists[listId] = {};
            for (auto it = begin; it != end; ++it) {
                g_lists[listId].push_back(value_from_legacy_string((*it)[0].str()));
            }
            return std::string("list:") + std::to_string(listId);
        } catch (...) { return {}; }
    }
    // Split by regex → returns a list handle
    if (name == "regex_split" || name == "re.split") {
        const std::string text = argS(0), pat = argS(1);
        try {
            const auto re = cached_regex(pat);
            auto begin = std::sregex_token_iterator(text.begin(), text.end(), *re, -1);
            auto end = std::sregex_token_iterator();
            int listId = g_nextListId++;
            g_lists[listId] = {};
            for (auto it = begin; it != end; ++it) {
                g_lists[listId].push_back(value_from_legacy_string(it->str()));
            }
            return std::string("list:") + std::to_string(listId);
        } catch (...) { return {}; }
    }
    // Capture groups from a regex match
    if (name == "regex_capture" || name == "re.capture") {
        const std::string text = argS(0), pat = argS(1);
        std::lock_guard<std::mutex> lg(g_regexMutex);
        g_lastCapturedGroups.clear();
        try {
            const auto re = cached_regex(pat); std::smatch m;
            if (std::regex_search(text, m, *re)) {
                for (size_t i = 0; i < m.size(); ++i) {
                    g_lastCapturedGroups.push_back(m[i].str());
                }
                return "true";
            }
        } catch (...) {}
        return "false";
    }
    // Get captured group by index
    if (name == "regex_group" || name == "re.group") {
        int idx = std::stoi(argS(0));
        std::lock_guard<std::mutex> lg(g_regexMutex);
        if (idx >= 0 && static_cast<size_t>(idx) < g_lastCapturedGroups.size()) {
            return g_lastCapturedGroups[idx];
        }
        return {};
    }
    // Compile a regex pattern → returns pattern id
    if (name == "regex_compile" || name == "re.compile") {
        const std::string pat = argS(0);
        try {
            std::regex re(pat);
            int id = g_nextPatternId++;
            g_compiledPatterns[id] = std::move(re);
            return std::to_string(id);
        } catch (...) { return {}; }
    }
    // Free a compiled pattern
    if (name == "regex_free" || name == "re.free") {
        int id = std::stoi(argS(0));
        g_compiledPatterns.erase(id);
        return {};
    }
    return {};
}

std::string __erelang_builtin_regex_dispatch(const std::string& name, const std::vector<std::string>& argv) {
    return regex_dispatch(name, argv);
}

} // namespace erelang

// Module registration: see include/erelang/module_registry.hpp.
#include "erelang/module_registry.hpp"
namespace {
const bool kModuleRegistered = erelang::modules::register_dispatch("regex",
    [](erelang::Runtime*, const std::string& name, const std::vector<std::string>& argv) {
        return erelang::__erelang_builtin_regex_dispatch(name, argv);
    });
}  // namespace
ERELANG_MODULE_ANCHOR(regex)
