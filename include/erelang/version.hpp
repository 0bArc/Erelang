#pragma once

// Central version + feature flags for erelang / Erelang
// Update here when bumping; runtime and tools should reference only this header.

#define ERELANG_VERSION_MAJOR 0
#define ERELANG_VERSION_MINOR 0
#define ERELANG_VERSION_PATCH 1
#define ERELANG_VERSION_STRING "0.0.1"
// Feature toggles (compile-time) - may expand later
#define ERELANG_FEATURE_THREADS 1
#define ERELANG_FEATURE_DATA 1
#define ERELANG_FEATURE_REGEX 1
#define ERELANG_FEATURE_BINARY 1
#define ERELANG_FEATURE_PERM 1
#define ERELANG_FEATURE_CRYPTO 1
#define ERELANG_FEATURE_COLLatz 1

namespace erelang {
struct BuildInfo {
    static const char* version() { return ERELANG_VERSION_STRING; }
    static int major() { return ERELANG_VERSION_MAJOR; }
    static int minor() { return ERELANG_VERSION_MINOR; }
    static int patch() { return ERELANG_VERSION_PATCH; }
};
}
