// SPDX-License-Identifier: Apache-2.0
//
// Interactive tooling for generating Erelang plugin scaffolding and syntax overrides.

#pragma once

#include <filesystem>
#include <iosfwd>

namespace erelang {

struct CreationKitOptions {
    std::filesystem::path runtimeRoot;
    std::filesystem::path userPluginRoot;
    std::istream* input = nullptr;
    std::ostream* output = nullptr;
    std::ostream* log = nullptr;
};

int run_creation_kit(const CreationKitOptions& options);

} // namespace erelang
