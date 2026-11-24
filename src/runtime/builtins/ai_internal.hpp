// Internal interface of builtin/ai. Not installed; shared by the ai_*.cpp files.
#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace erelang::ai {

// One row per well known text format. Drives kind, auto, generate, validate, extract and redact.
struct Kind {
    const char* name;         // "email"
    const char* regex;        // whole text match, no anchors, safe to embed
    const char* open;         // same format without its closing part ("ending in .com"), or nullptr
    const char* description;  // plain English, used by explain and docs
    const char* example;
    bool bounded;             // wrap in \b when searching inside longer text
};
const std::vector<Kind>& kinds();
const Kind* find_kind(std::string_view name);
std::string kind_of(const std::string& text);
std::string majority_kind(const std::vector<std::string>& samples);

// Inference from samples (ai_infer.cpp).
std::string pattern_of(const std::string& sample);                    // expect pattern, x digit X letter
std::string compact_regex(const std::string& sample);                 // [0-9]{4}-[0-9]{2}
std::string pattern_of_all(const std::vector<std::string>& samples);  // expect pattern for many
std::string infer_regex(const std::vector<std::string>& samples);     // regex for many
std::string auto_regex(const std::vector<std::string>& samples);     // kind aware

// Regex tools (ai_regex_tools.cpp).
bool regex_valid(const std::string& rx);
bool full_match(const std::string& rx, const std::string& text);
std::string explain(const std::string& rx);
std::string optimize(const std::string& rx);
std::vector<std::string> examples(const std::string& rx, int count);
std::string escape_literal(std::string_view text);                    // regex escape

// Natural language to regex (ai_nl.cpp). Empty string means "not understood".
std::string generate(const std::string& description);

// Text helpers (ai_text.cpp).
std::vector<std::string> extract(const std::string& text, const std::string& kind);
std::string redact(const std::string& text, const std::string& kind);
std::string redact_all(const std::string& text);

// Script list handles.
std::string make_list(const std::vector<std::string>& items);
std::vector<std::string> read_list(const std::string& handle, bool* ok = nullptr);

}  // namespace erelang::ai
