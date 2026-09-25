#include "erelang/packages.hpp"
#include "erelang/lexer.hpp"
#include "erelang/parser.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace erelang {
namespace fs = std::filesystem;

namespace {

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

std::string eval_string_expr(const ExprPtr& e) {
    if (!e) return {};
    if (const auto* s = std::get_if<ExprString>(&e->node)) return s->v;
    return {};
}

struct SemVer {
    int major{0};
    int minor{0};
    int patch{0};
};

std::optional<SemVer> parse_semver(const std::string& text) {
    SemVer v;
    std::string s = text;
    if (!s.empty() && s[0] == 'v') s.erase(s.begin());
    std::istringstream iss(s);
    char dot = 0;
    if (!(iss >> v.major)) return std::nullopt;
    if (iss.peek() == '.') {
        iss >> dot >> v.minor;
    }
    if (iss.peek() == '.') {
        iss >> dot >> v.patch;
    }
    return v;
}

bool version_satisfies(const std::string& available, const std::string& requirement) {
    if (requirement.empty() || requirement == "*") return true;
    if (!requirement.empty() && requirement[0] == '^') {
        auto want = parse_semver(requirement.substr(1));
        auto have = parse_semver(available);
        if (!want || !have) return available == requirement.substr(1);
        if (have->major != want->major) return false;
        if (have->minor != want->minor) return have->minor > want->minor;
        return have->patch >= want->patch;
    }
    return available == requirement;
}

std::uint64_t fnv1a64(const std::string& data) {
    std::uint64_t h = 14695981039346656037ull;
    for (unsigned char c : data) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

std::string hex64(std::uint64_t v) {
    std::ostringstream oss;
    oss << std::hex << v;
    return oss.str();
}

Program parse_elan_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    LexerOptions opts;
    opts.enableDurations = true;
    opts.enableUnits = true;
    opts.enablePolyIdentifiers = true;
    Lexer lx(ss.str(), opts);
    Parser ps(lx.lex(), path.string());
    return ps.parse();
}

std::string global_leaf(const std::string& qualified) {
    auto pos = qualified.rfind("::");
    if (pos == std::string::npos) return qualified;
    return qualified.substr(pos + 2);
}

std::string global_ns(const std::string& qualified) {
    auto pos = qualified.rfind("::");
    if (pos == std::string::npos) return {};
    return qualified.substr(0, pos);
}

} // namespace

std::optional<PackageManifest> load_package_manifest(const fs::path& packageElan) {
    if (!fs::exists(packageElan)) return std::nullopt;
    Program prog;
    try {
        prog = parse_elan_file(packageElan);
    } catch (...) {
        return std::nullopt;
    }
    PackageManifest m;
    m.root = packageElan.parent_path();
    for (const auto& g : prog.globals) {
        const std::string leaf = global_leaf(g.name);
        const std::string ns = global_ns(g.name);
        const std::string value = eval_string_expr(g.value);
        if (ns == "pkg" || ns.empty()) {
            if (leaf == "name") m.name = value;
            else if (leaf == "version") m.version = value;
            else if (leaf == "entry") m.entry = value.empty() ? "lib.elan" : value;
        } else if (ns == "pkg::depends" || ns == "pkg.depends") {
            if (!leaf.empty() && !value.empty()) {
                m.depends.push_back(PackageDepend{leaf, value});
            }
        }
    }
    if (m.name.empty() || m.version.empty()) return std::nullopt;
    return m;
}

std::optional<PackageLock> load_package_lock(const fs::path& lockPath) {
    if (!fs::exists(lockPath)) return std::nullopt;
    std::ifstream in(lockPath);
    if (!in) return std::nullopt;
    PackageLock lock;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        std::istringstream iss(line);
        LockEntry e;
        if (!(iss >> e.name >> e.version >> e.hash)) continue;
        std::string pathStr;
        if (iss >> pathStr) e.path = pathStr;
        lock.entries.push_back(std::move(e));
    }
    return lock;
}

bool write_package_lock(const fs::path& lockPath, const PackageLock& lock) {
    std::ofstream out(lockPath, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << "# erelang.lock — exact version + content hash\n";
    for (const auto& e : lock.entries) {
        out << e.name << ' ' << e.version << ' ' << e.hash;
        if (!e.path.empty()) out << ' ' << e.path.generic_string();
        out << '\n';
    }
    return true;
}

std::string hash_package_tree(const fs::path& root) {
    std::vector<fs::path> files;
    std::error_code ec;
    if (!fs::exists(root, ec)) return {};
    for (auto it = fs::recursive_directory_iterator(root, ec);
         it != fs::recursive_directory_iterator(); ++it) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    std::ostringstream acc;
    for (const auto& f : files) {
        acc << f.lexically_relative(root).generic_string() << '\n';
        std::ifstream in(f, std::ios::binary);
        if (!in) continue;
        acc << in.rdbuf() << '\n';
    }
    return hex64(fnv1a64(acc.str()));
}

std::optional<fs::path> find_registry_version(
    const fs::path& registry,
    const std::string& name,
    const std::string& requirement) {
    const fs::path pkgRoot = registry / name;
    if (!fs::is_directory(pkgRoot)) return std::nullopt;
    std::vector<std::string> versions;
    std::error_code ec;
    for (auto it = fs::directory_iterator(pkgRoot, ec); it != fs::directory_iterator(); ++it) {
        if (ec) break;
        if (!it->is_directory(ec)) continue;
        versions.push_back(it->path().filename().string());
    }
    std::sort(versions.begin(), versions.end(), [](const std::string& a, const std::string& b) {
        auto va = parse_semver(a);
        auto vb = parse_semver(b);
        if (!va || !vb) return a < b;
        if (va->major != vb->major) return va->major < vb->major;
        if (va->minor != vb->minor) return va->minor < vb->minor;
        return va->patch < vb->patch;
    });
    std::optional<fs::path> best;
    for (const auto& ver : versions) {
        if (!version_satisfies(ver, requirement)) continue;
        const fs::path cand = pkgRoot / ver;
        if (fs::exists(cand / "package.elan")) best = cand;
    }
    return best;
}

PackageLock resolve_and_lock(const PackageManifest& root, const fs::path& registry) {
    PackageLock lock;
    std::unordered_map<std::string, bool> seen;
    std::vector<PackageDepend> queue = root.depends;
    // Include root itself when it lives in a project (not required for apps).
    auto add_pkg = [&](const std::string& name, const std::string& req) {
        if (seen[name]) return;
        auto path = find_registry_version(registry, name, req);
        if (!path) {
            throw std::runtime_error("package not found in registry: " + name + " (" + req + ")");
        }
        auto man = load_package_manifest(*path / "package.elan");
        if (!man) throw std::runtime_error("invalid package.elan: " + (*path / "package.elan").string());
        seen[name] = true;
        LockEntry e;
        e.name = man->name;
        e.version = man->version;
        e.path = *path;
        e.hash = hash_package_tree(*path);
        lock.entries.push_back(e);
        for (const auto& d : man->depends) queue.push_back(d);
    };
    for (size_t i = 0; i < queue.size(); ++i) {
        add_pkg(queue[i].name, queue[i].requirement);
    }
    return lock;
}

bool fetch_locked_packages(
    const PackageLock& lock,
    const fs::path& registry,
    const fs::path& cacheDir) {
    std::error_code ec;
    fs::create_directories(cacheDir, ec);
    for (const auto& e : lock.entries) {
        fs::path src = e.path;
        if (src.empty()) {
            auto found = find_registry_version(registry, e.name, e.version);
            if (!found) return false;
            src = *found;
        }
        const fs::path dest = cacheDir / e.name / e.version;
        fs::create_directories(dest, ec);
        for (auto it = fs::recursive_directory_iterator(src, ec);
             it != fs::recursive_directory_iterator(); ++it) {
            if (ec) return false;
            if (!it->is_regular_file(ec)) continue;
            const fs::path rel = it->path().lexically_relative(src);
            const fs::path out = dest / rel;
            fs::create_directories(out.parent_path(), ec);
            fs::copy_file(it->path(), out, fs::copy_options::overwrite_existing, ec);
            if (ec) return false;
        }
    }
    return true;
}

std::optional<fs::path> resolve_package_import(
    const std::string& importPath,
    const fs::path& fromFile,
    const fs::path& registryHint) {
    std::string path = importPath;
    for (char& c : path) if (c == '\\') c = '/';
    std::string name;
    if (path.rfind("pkg/", 0) == 0) name = path.substr(4);
    else if (path.rfind("package/", 0) == 0) name = path.substr(8);
    else return std::nullopt;
    auto slash = name.find('/');
    std::string sub;
    if (slash != std::string::npos) {
        sub = name.substr(slash + 1);
        name = name.substr(0, slash);
    }
    if (name.empty()) return std::nullopt;

    fs::path cursor = fs::absolute(fromFile).parent_path();
    fs::path lockPath;
    for (int i = 0; i < 32; ++i) {
        const fs::path cand = cursor / "erelang.lock";
        if (fs::exists(cand)) {
            lockPath = cand;
            break;
        }
        if (cursor == cursor.root_path()) break;
        cursor = cursor.parent_path();
    }
    if (lockPath.empty()) return std::nullopt;
    auto lock = load_package_lock(lockPath);
    if (!lock) return std::nullopt;
    const LockEntry* entry = nullptr;
    for (const auto& e : lock->entries) {
        if (e.name == name) { entry = &e; break; }
    }
    if (!entry) return std::nullopt;

    fs::path pkgRoot;
    const fs::path projectRoot = lockPath.parent_path();
    const fs::path cacheCand = projectRoot / ".erelang" / "pkgs" / entry->name / entry->version;
    if (fs::is_directory(cacheCand)) pkgRoot = cacheCand;
    else if (!entry->path.empty() && fs::is_directory(entry->path)) pkgRoot = entry->path;
    else if (!registryHint.empty()) {
        auto found = find_registry_version(registryHint, entry->name, entry->version);
        if (found) pkgRoot = *found;
    }
    if (pkgRoot.empty()) {
        const char* envReg = std::getenv("ERELANG_REGISTRY");
        if (envReg && *envReg) {
            auto found = find_registry_version(fs::path(envReg), entry->name, entry->version);
            if (found) pkgRoot = *found;
        }
    }
    if (pkgRoot.empty()) return std::nullopt;

    if (!sub.empty()) {
        fs::path file = pkgRoot / sub;
        if (!file.has_extension()) file.replace_extension(".elan");
        if (fs::exists(file)) return file;
        return std::nullopt;
    }
    auto man = load_package_manifest(pkgRoot / "package.elan");
    if (!man) return std::nullopt;
    fs::path entryFile = pkgRoot / man->entry;
    if (fs::exists(entryFile)) return entryFile;
    return std::nullopt;
}

} // namespace erelang
