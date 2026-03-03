// SPDX-License-Identifier: Apache-2.0
//
// Language profile metadata for Erelang/erelang.
// Describes dialect-specific settings (extensions, keyword aliases)
// allowing plugins to surface custom language skins that still compile
// through the shared pipeline.

#pragma once

#include "erelang/erodsl/spec.hpp"

namespace erelang {

using LanguageProfile [[deprecated("use erodsl::DslSpec")]] = erodsl::DslSpec;

[[deprecated("use erodsl::make_default_spec")]] inline LanguageProfile make_default_language_profile() {
    return erodsl::make_default_spec();
}

[[deprecated("use erodsl::canonical_keywords")]] inline const std::unordered_set<std::string>& canonical_language_keywords() {
    return erodsl::canonical_keywords();
}

[[deprecated]] inline LexerOptions build_lexer_options(const LanguageProfile& profile) {
    return erodsl::build_lexer_options(profile);
}

[[deprecated]] inline void apply_language_profile(const LanguageProfile& profile, std::vector<Token>& tokens) {
    erodsl::apply_keyword_aliases(profile, tokens);
}

[[deprecated("use erodsl::normalize_extension")]] inline std::string normalize_extension(std::string ext) {
    return erodsl::normalize_extension(std::move(ext));
}

} // namespace erelang
