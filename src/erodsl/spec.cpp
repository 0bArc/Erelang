// SPDX-License-Identifier: Apache-2.0
//
// EroDSL spec helpers: describes dialect metadata and keyword aliasing.

#include "erelang/erodsl/spec.hpp"

#include <algorithm>
#include <cctype>

namespace erodsl {
namespace {

std::string lowercase_copy(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char ch : value) {
        out.push_back(static_cast<char>(std::tolower(ch)));
    }
    return out;
}

std::unordered_set<std::string> merged_keywords(const DslSpec& spec) {
    std::unordered_set<std::string> keywords = canonical_keywords();
    for (const auto& [alias, canonical] : spec.keywordAliases) {
        keywords.insert(alias);
        keywords.insert(canonical);
    }
    return keywords;
}

} // namespace

DslSpec make_default_spec() {
    DslSpec spec;
    spec.id = "erelang.dsl.erelang";
    spec.name = "erelang";
    spec.version = "1.0";
    spec.extensions = {".0bs", ".erelang", ".obsecret"};
    spec.keywordAliases.clear();
    spec.keywordAliases.emplace("nullptr", "null");
    return spec;
}

const std::unordered_set<std::string>& canonical_keywords() {
    static const std::unordered_set<std::string> keywords = {
        "if","else","while","for","switch","case","default","return","let","const",
        "action","entity","hook","global","run","in","public","private","export",
        "true","false","null","nil","nullptr"
    };
    return keywords;
}

erelang::LexerOptions build_lexer_options(const DslSpec& spec) {
    erelang::LexerOptions opts;
    opts.enableDurations = true;
    opts.enableUnits = true;
    opts.enablePolyIdentifiers = true;
    opts.emitDocComments = true;
    opts.emitComments = false;
    opts.keywords = merged_keywords(spec);
    return opts;
}

void apply_keyword_aliases(const DslSpec& spec, std::vector<erelang::Token>& tokens) {
    if (spec.keywordAliases.empty()) {
        return;
    }
    for (auto& token : tokens) {
        if (token.kind != erelang::TokenKind::Word && token.kind != erelang::TokenKind::Keyword) {
            continue;
        }
        auto lowered = lowercase_copy(token.text);
        auto it = spec.keywordAliases.find(lowered);
        if (it != spec.keywordAliases.end()) {
            token.text = it->second;
            token.kind = erelang::TokenKind::Keyword;
        }
    }
}

std::string normalize_extension(std::string ext) {
    if (ext.empty()) {
        return ext;
    }
    std::string normalized;
    normalized.reserve(ext.size() + 1);
    if (ext.front() != '.') {
        normalized.push_back('.');
    }
    for (unsigned char ch : ext) {
        if (ch == '.') {
            normalized.push_back('.');
        } else {
            normalized.push_back(static_cast<char>(std::tolower(ch)));
        }
    }
    return normalized;
}

} // namespace erodsl
