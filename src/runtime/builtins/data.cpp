// Simple data store / persistence builtins
#include "erelang/runtime.hpp"
#include <unordered_map>
#include <string>
#include <fstream>
#include <sstream>

namespace erelang {

struct DataStore { std::unordered_map<std::string,std::string> kv; };
static std::unordered_map<int, DataStore> g_datastores; static int g_nextStoreId = 1;

// Parse a "data:<id>" handle; returns -1 for malformed handles so callers
// can bail out instead of throwing std::stoi.
static int parse_data_handle(const std::string& h) {
    if (h.rfind("data:", 0) != 0) return -1;
    try {
        auto tail = h.substr(5);
        if (tail.empty()) return -1;
        std::size_t pos = 0;
        int v = std::stoi(tail, &pos);
        if (pos != tail.size()) return -1;
        return v;
    } catch (...) {
        return -1;
    }
}

static DataStore* find_store(int id) {
    auto it = g_datastores.find(id);
    return it == g_datastores.end() ? nullptr : &it->second;
}

static std::string data_dispatch(const std::string& name, const std::vector<std::string>& argv) {
    auto argS = [&](size_t i){ return i<argv.size()?argv[i]:std::string(); };
    if (name == "data_new") {
        int id = g_nextStoreId++; g_datastores[id] = {}; return std::string("data:") + std::to_string(id);
    }
    if (name == "data_set") {
        auto h = argS(0); auto k = argS(1); auto v = argS(2);
        int id = parse_data_handle(h);
        if (id >= 0) { if (auto* store = find_store(id)) store->kv[k] = v; }
        return {};
    }
    if (name == "data_get") {
        auto h = argS(0); auto k = argS(1);
        int id = parse_data_handle(h);
        if (id >= 0) { if (auto* store = find_store(id)) { auto it=store->kv.find(k); if(it!=store->kv.end()) return it->second; } }
        return {};
    }
    if (name == "data_has") {
        auto h = argS(0); auto k = argS(1);
        int id = parse_data_handle(h);
        if (id >= 0) { if (auto* store = find_store(id)) return store->kv.count(k)?"true":"false"; }
        return "false";
    }
    if (name == "data_keys") {
        auto h = argS(0);
        int id = parse_data_handle(h);
        if (id < 0) return {};
        DataStore* store = find_store(id);
        if (!store) return {};
        std::ostringstream ss; bool first=true; for (auto &kv : store->kv){ if(!first) ss<<","; first=false; ss<<kv.first; }
        return ss.str();
    }
    if (name == "data_save") {
        auto h = argS(0); auto path = argS(1);
        int id = parse_data_handle(h);
        if (id < 0) return {};
        DataStore* store = find_store(id);
        if (!store) return {};
        std::ofstream out(path, std::ios::binary);
        if (!out) return "false";
        for (auto &kv : store->kv) { out<<kv.first<<"="<<kv.second<<"\n"; }
        return out.good() ? "true" : "false";
    }
    if (name == "data_load") {
        auto path = argS(0); std::ifstream in(path, std::ios::binary); if(!in) return {};
        int id = g_nextStoreId++; g_datastores[id] = {};
        std::string line; while(std::getline(in,line)){ auto p=line.find('='); if(p!=std::string::npos){ g_datastores[id].kv[line.substr(0,p)]=line.substr(p+1); }}
        return std::string("data:")+std::to_string(id);
    }
    return {};
}

std::string __erelang_builtin_data_dispatch(const std::string& name, const std::vector<std::string>& argv) {
    return data_dispatch(name, argv);
}

} // namespace erelang
