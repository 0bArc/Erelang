// SPDX-License-Identifier: Apache-2.0
// Policy system removed. Stub kept for include-compatibility.
#pragma once
#include <string>

namespace erelang {
class PolicyManager {
public:
    static PolicyManager& instance() { static PolicyManager inst; return inst; }
    void load(const std::string&) {}
    bool is_allowed(const std::string&) const noexcept { return true; }
};
}
