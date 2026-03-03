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
#endif

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

#ifdef _WIN32
    void retain_handle(HMODULE handle) {
        std::scoped_lock lock(mutex);
        dynamicHandles.push_back(handle);
    }
#endif

    mutable std::mutex mutex;
    std::vector<ModuleDef> modules;
#ifdef _WIN32
    std::vector<HMODULE> dynamicHandles; // kept alive for module lifetime
#endif
};

ModuleRegistry& registry() {
    static ModuleRegistry instance;
    return instance;
}

#ifdef _WIN32
struct FindHandle {
    HANDLE handle{INVALID_HANDLE_VALUE};
    FindHandle() = default;
    explicit FindHandle(HANDLE h) : handle(h) {}
    FindHandle(const FindHandle&) = delete;
    FindHandle& operator=(const FindHandle&) = delete;
    FindHandle(FindHandle&& other) noexcept : handle(std::exchange(other.handle, INVALID_HANDLE_VALUE)) {}
    FindHandle& operator=(FindHandle&& other) noexcept {
        if (this != &other) {
            reset();
            handle = std::exchange(other.handle, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    ~FindHandle() { reset(); }
    void reset() noexcept {
        if (handle != INVALID_HANDLE_VALUE) {
            FindClose(handle);
            handle = INVALID_HANDLE_VALUE;
        }
    }
    explicit operator bool() const noexcept { return handle != INVALID_HANDLE_VALUE; }
};
#endif

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

void load_dynamic_modules_in_dir(const std::filesystem::path& dir) {
#ifdef _WIN32
    if (dir.empty()) {
        return;
    }

    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        return;
    }

    const std::wstring pattern = (dir / L"*.odll").wstring();
    WIN32_FIND_DATAW data{};
    FindHandle finder{FindFirstFileW(pattern.c_str(), &data)};
    if (!finder) {
        return;
    }

    const auto try_register_module = [&](const std::filesystem::path& modulePath) {
        if (modulePath.empty()) {
            return;
        }

        if (HMODULE handle = LoadLibraryW(modulePath.c_str()); handle != nullptr) {
            registry().retain_handle(handle);
            using GetModuleFn = const ModuleDef* (*)();
            if (const auto fn = reinterpret_cast<GetModuleFn>(GetProcAddress(handle, "ErelangGetModule")); fn != nullptr) {
                if (const ModuleDef* def = fn(); def != nullptr && def->files != nullptr && def->file_count > 0) {
                    register_embedded_module(*def);
                }
            }
        }
    };

    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            try_register_module(dir / data.cFileName);
        }
    } while (FindNextFileW(finder.handle, &data));

#else
    (void)dir;
#endif
}

} // namespace erelang
