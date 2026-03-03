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
    if (name == "regex_match") {
        std::regex re(argS(0)); return std::regex_match(argS(1), re)?"true":"false";
    }
    if (name == "regex_find") {
        std::string subject = argS(1);
        std::regex re(argS(0)); std::smatch m; if(std::regex_search(subject, m, re)){ if(m.size()>1) return m[1].str(); return m[0].str(); } return {};
    }
    if (name == "regex_replace") {
        std::regex re(argS(0)); return std::regex_replace(argS(2), re, argS(1));
    }
    return {};
}

std::string __erelang_builtin_regex_dispatch(const std::string& name, const std::vector<std::string>& argv) {
    return regex_dispatch(name, argv);
}

} // namespace erelang
