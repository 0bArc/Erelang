#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "erelang/lexer.hpp"

namespace erodsl {

struct DslSpec {
    std::string id;
    std::string name;
    std::string version;
    std::vector<std::string> extensions;
    std::unordered_map<std::string, std::string> keywordAliases; // alias(lower) -> canonical(lower)
};

// Default Erelang/erelang spec (baseline dialect).
DslSpec make_default_spec();

// Canonical keyword set shared by the baseline dialect.
const std::unordered_set<std::string>& canonical_keywords();

// Build lexer options for a spec (injects alias keywords).
erelang::LexerOptions build_lexer_options(const DslSpec& spec);

// Canonicalize tokens according to the spec alias map.
void apply_keyword_aliases(const DslSpec& spec, std::vector<erelang::Token>& tokens);

// Normalize file extensions (ensure lowercase + leading dot).
std::string normalize_extension(std::string ext);

} // namespace erodsl
