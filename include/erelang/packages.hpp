#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace erelang {

struct PackageDepend {
    std::string name;
    std::string requirement; // exact "1.2.3" or caret "^1.2.3"
};

struct PackageManifest {
    std::string name;
    std::string version;
    std::string entry{"lib.elan"};
    std::vector<PackageDepend> depends;
    std::filesystem::path root;
};

struct LockEntry {
    std::string name;
    std::string version;
    std::string hash; // content hash of package tree
    std::filesystem::path path; // resolved package root
};

struct PackageLock {
    std::vector<LockEntry> entries;
};

[[nodiscard]] std::optional<PackageManifest> load_package_manifest(const std::filesystem::path& packageElan);
[[nodiscard]] std::optional<PackageLock> load_package_lock(const std::filesystem::path& lockPath);
[[nodiscard]] bool write_package_lock(const std::filesystem::path& lockPath, const PackageLock& lock);

[[nodiscard]] std::string hash_package_tree(const std::filesystem::path& root);

// Registry layout: <registry>/<name>/<version>/package.elan
[[nodiscard]] std::optional<std::filesystem::path> find_registry_version(
    const std::filesystem::path& registry,
    const std::string& name,
    const std::string& requirement);

[[nodiscard]] PackageLock resolve_and_lock(
    const PackageManifest& root,
    const std::filesystem::path& registry);

[[nodiscard]] bool fetch_locked_packages(
    const PackageLock& lock,
    const std::filesystem::path& registry,
    const std::filesystem::path& cacheDir);

// Resolve #include <pkg/NAME> or import "pkg/NAME" via lockfile + cache/registry.
[[nodiscard]] std::optional<std::filesystem::path> resolve_package_import(
    const std::string& importPath,
    const std::filesystem::path& fromFile,
    const std::filesystem::path& registryHint = {});

} // namespace erelang
