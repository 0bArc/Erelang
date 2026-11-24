// builtin/data: small key value stores that can be saved to and loaded from a text file.
//
// File format: one `key=value` per line. Backslash escapes `\\`, `\n`, `\r` and `\=` keep keys and
// values with newlines or equals signs intact; files written by older versions (no escapes) still
// load the same.
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "erelang/runtime.hpp"

namespace erelang {

namespace {

struct DataStore {
    std::unordered_map<std::string, std::string> kv;
};

std::mutex g_dataMutex;
std::unordered_map<int, DataStore> g_datastores;
int g_nextStoreId = 1;

// "data:<id>" -> id, or -1 for anything else.
int parse_data_handle(const std::string& h) {
    if (h.rfind("data:", 0) != 0 || h.size() == 5) return -1;
    int v = 0;
    for (std::size_t i = 5; i < h.size(); ++i) {
        const char c = h[i];
        if (c < '0' || c > '9' || v > 100000000) return -1;
        v = v * 10 + (c - '0');
    }
    return v;
}

DataStore* find_store(const std::string& handle) {
    const int id = parse_data_handle(handle);
    if (id < 0) return nullptr;
    auto it = g_datastores.find(id);
    return it == g_datastores.end() ? nullptr : &it->second;
}

std::string escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '=': out += "\\="; break;
            default: out += c;
        }
    }
    return out;
}

std::string unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) { out += s[i]; continue; }
        const char e = s[++i];
        out += e == 'n' ? '\n' : e == 'r' ? '\r' : e;
    }
    return out;
}

// Position of the first `=` not preceded by a backslash escape.
std::size_t key_split(const std::string& line) {
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\\') { ++i; continue; }
        if (line[i] == '=') return i;
    }
    return std::string::npos;
}

std::string new_store() {
    const int id = g_nextStoreId++;
    g_datastores[id] = {};
    return "data:" + std::to_string(id);
}

std::string data_dispatch(const std::string& name, const std::vector<std::string>& argv) {
    const auto argS = [&](std::size_t i) { return i < argv.size() ? argv[i] : std::string(); };
    std::lock_guard<std::mutex> lock(g_dataMutex);
    if (name == "data_new") return new_store();
    if (name == "data_set") {
        DataStore* store = find_store(argS(0));
        if (!store) return "false";
        store->kv[argS(1)] = argS(2);
        return "true";
    }
    if (name == "data_get") {
        DataStore* store = find_store(argS(0));
        if (!store) return {};
        const auto it = store->kv.find(argS(1));
        return it == store->kv.end() ? std::string() : it->second;
    }
    if (name == "data_has") {
        DataStore* store = find_store(argS(0));
        return store && store->kv.count(argS(1)) ? "true" : "false";
    }
    if (name == "data_keys") {
        DataStore* store = find_store(argS(0));
        if (!store) return {};
        std::ostringstream ss;
        bool first = true;
        for (const auto& kv : store->kv) {
            if (!first) ss << ',';
            first = false;
            ss << kv.first;
        }
        return ss.str();
    }
    if (name == "data_save") {
        DataStore* store = find_store(argS(0));
        if (!store) return "false";
        std::ofstream out(argS(1), std::ios::binary);
        if (!out) return "false";
        for (const auto& kv : store->kv) out << escape(kv.first) << '=' << escape(kv.second) << '\n';
        return out.good() ? "true" : "false";
    }
    if (name == "data_load") {
        std::ifstream in(argS(0), std::ios::binary);
        if (!in) return {};
        const std::string handle = new_store();
        DataStore* store = find_store(handle);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const std::size_t p = key_split(line);
            if (p == std::string::npos) continue;
            store->kv[unescape(line.substr(0, p))] = unescape(line.substr(p + 1));
        }
        return handle;
    }
    return {};
}

}  // namespace

std::string __erelang_builtin_data_dispatch(const std::string& name, const std::vector<std::string>& argv) {
    return data_dispatch(name, argv);
}

}  // namespace erelang

// Module registration: see include/erelang/module_registry.hpp.
#include "erelang/module_registry.hpp"
namespace {
const bool kModuleRegistered = erelang::modules::register_dispatch("data",
    [](erelang::Runtime*, const std::string& name, const std::vector<std::string>& argv) {
        return erelang::__erelang_builtin_data_dispatch(name, argv);
    });
}  // namespace
ERELANG_MODULE_ANCHOR(data)
