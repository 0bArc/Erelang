#include "erelang/modules.hpp"

#include <algorithm>
#include <iterator>
#include <mutex>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include <filesystem>

namespace erelang {

namespace {
struct ModuleRegistry {
    void add(const ModuleDef& def) {
        std::scoped_lock lock(mutex);
        modules.push_back(def);
    }

    [[nodiscard]] std::vector<ModuleDef> snapshot() const {
        std::scoped_lock lock(mutex);
        return modules;
    }

    void retain_handle(void* handle) {
        std::scoped_lock lock(mutex);
        dynamicHandles.push_back(handle);
    }

    mutable std::mutex mutex;
    std::vector<ModuleDef> modules;
    std::vector<void*> dynamicHandles; // HMODULE or dlopen handle, kept alive for module lifetime
};

ModuleRegistry& registry() {
    static ModuleRegistry instance;
    return instance;
}


} // namespace

std::vector<ModuleInfo> resolve_imports(const std::vector<std::string>& imports) {
    std::vector<ModuleInfo> resolved;
    resolved.reserve(imports.size());
    std::transform(imports.begin(), imports.end(), std::back_inserter(resolved), [](const std::string& entry) {
        return ModuleInfo{entry, entry};
    });
    return resolved;
}

void register_embedded_module(const ModuleDef& def) {
    registry().add(def);
}

std::vector<ModuleDef> get_registered_modules() {
    return registry().snapshot();
}

// Loads every *.odll in `dir` (a shared library exporting ErelangGetModule) and registers its module.
void load_dynamic_modules_in_dir(const std::filesystem::path& dir) {
    if (dir.empty()) {
        return;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return;
    }

    using GetModuleFn = const ModuleDef* (*)();
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".odll") {
            continue;
        }
#ifdef _WIN32
        HMODULE handle = LoadLibraryW(entry.path().c_str());
        if (!handle) continue;
        // Through void* so GCC does not warn about casting FARPROC to another function type.
        const auto fn = reinterpret_cast<GetModuleFn>(reinterpret_cast<void*>(GetProcAddress(handle, "ErelangGetModule")));
#else
        void* handle = dlopen(entry.path().c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle) continue;
        const auto fn = reinterpret_cast<GetModuleFn>(dlsym(handle, "ErelangGetModule"));
#endif
        registry().retain_handle(reinterpret_cast<void*>(handle));
        if (fn != nullptr) {
            if (const ModuleDef* def = fn(); def != nullptr && def->files != nullptr && def->file_count > 0) {
                register_embedded_module(*def);
            }
        }
    }
}

} // namespace erelang
