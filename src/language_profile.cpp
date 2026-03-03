// SPDX-License-Identifier: Apache-2.0
//
// Compatibility translation unit: legacy language_profile symbols now forward
// to the new EroDSL spec helpers. The actual implementations live in
// src/erodsl/spec.cpp. This TU remains so existing build scripts that still
// reference language_profile.cpp continue to compile without code changes.

#include "erelang/language_profile.hpp"

namespace erelang {
namespace {
// Intentionally empty namespace to ensure the TU is not stripped by compilers
// that warn on translation units without symbols.
[[maybe_unused]] constexpr int kLanguageProfileCompatAnchor = 0;
} // namespace
} // namespace erelang
