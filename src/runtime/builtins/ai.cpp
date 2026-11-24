// builtin/ai: dispatch. See docs/pages/modules/ai.md.
//
// Script names map to flat builtins in imports.cpp: ai.regex.generate(...) is ai_regex_generate.
#include <algorithm>
#include <string>
#include <vector>
#include "ai_internal.hpp"

namespace erelang {

// Compact regex for one sample, shared with builtin/regex (re.infer).
std::string __erelang_intel_regex(const std::string& sample) { return ai::compact_regex(sample); }

std::string __erelang_builtin_ai_dispatch(const std::string& name, const std::vector<std::string>& argv) {
    if (name.rfind("ai_", 0) != 0) return {};
    const auto argS = [&](std::size_t i) { return i < argv.size() ? argv[i] : std::string(); };
    const auto yes = [](bool v) { return std::string(v ? "true" : "false"); };
    // A list handle, or a single string treated as one sample.
    const auto samples = [&](std::size_t i) {
        bool isList = false;
        std::vector<std::string> out = ai::read_list(argS(i), &isList);
        if (!isList) out = {argS(i)};
        return out;
    };

    if (name == "ai_auto") return ai::auto_regex(samples(0));
    if (name == "ai_infer_regex") return ai::infer_regex(samples(0));
    if (name == "ai_infer_pattern") return ai::pattern_of_all(samples(0));
    if (name == "ai_pattern") return ai::pattern_of(argS(0));
    if (name == "ai_kind") return ai::kind_of(argS(0));
    if (name == "ai_kind_all") return ai::majority_kind(samples(0));
    if (name == "ai_names") {
        std::vector<std::string> names;
        for (const auto& k : ai::kinds()) names.push_back(k.name);
        return ai::make_list(names);
    }
    if (name == "ai_describe") { const ai::Kind* k = ai::find_kind(argS(0)); return k ? k->description : ""; }
    if (name == "ai_example") { const ai::Kind* k = ai::find_kind(argS(0)); return k ? k->example : ""; }

    if (name == "ai_regex_generate") return ai::generate(argS(0));
    if (name == "ai_regex_can_generate") return yes(!ai::generate(argS(0)).empty());
    if (name == "ai_regex_explain") return ai::explain(argS(0));
    if (name == "ai_regex_optimize") return ai::optimize(argS(0));
    if (name == "ai_regex_test") return yes(ai::full_match(argS(0), argS(1)));
    if (name == "ai_regex_valid") return yes(ai::regex_valid(argS(0)));
    if (name == "ai_regex_of") { const ai::Kind* k = ai::find_kind(argS(0)); return k ? k->regex : ""; }
    if (name == "ai_regex_examples") {
        int n = 5;
        try { if (!argS(1).empty()) n = std::stoi(argS(1)); } catch (...) {}
        return ai::make_list(ai::examples(argS(0), std::min(std::max(n, 0), 200)));
    }

    if (name == "ai_extract") return ai::make_list(ai::extract(argS(0), argS(1)));
    if (name == "ai_count") return std::to_string(ai::extract(argS(0), argS(1)).size());
    if (name == "ai_validate") {
        const ai::Kind* k = ai::find_kind(argS(0));
        return yes(k && ai::full_match(k->regex, argS(1)));
    }
    if (name == "ai_redact") return ai::redact(argS(0), argS(1));
    if (name == "ai_redact_all") return ai::redact_all(argS(0));
    return {};
}

}  // namespace erelang

// Module registration: see include/erelang/module_registry.hpp.
#include "erelang/module_registry.hpp"
namespace {
const bool kModuleRegistered = erelang::modules::register_dispatch("ai",
    [](erelang::Runtime*, const std::string& name, const std::vector<std::string>& argv) {
        return erelang::__erelang_builtin_ai_dispatch(name, argv);
    });
}  // namespace
ERELANG_MODULE_ANCHOR(ai)
