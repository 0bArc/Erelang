// Simple data store / persistence builtins
#include "erelang/runtime.hpp"
#include <unordered_map>
#include <string>
#include <fstream>
#include <sstream>

namespace erelang {

struct DataStore { std::unordered_map<std::string,std::string> kv; };
static std::unordered_map<int, DataStore> g_datastores; static int g_nextStoreId = 1;

static std::string data_dispatch(const std::string& name, const std::vector<std::string>& argv) {
    auto argS = [&](size_t i){ return i<argv.size()?argv[i]:std::string(); };
    if (name == "data_new") {
        int id = g_nextStoreId++; g_datastores[id] = {}; return std::string("data:") + std::to_string(id);
    }
    if (name == "data_set") {
        auto h = argS(0); auto k = argS(1); auto v = argS(2);
        if (h.rfind("data:",0)==0){ int id = std::stoi(h.substr(5)); g_datastores[id].kv[k]=v; }
        return {};
    }
    if (name == "data_get") {
        auto h = argS(0); auto k = argS(1);
        if (h.rfind("data:",0)==0){ int id = std::stoi(h.substr(5)); auto it=g_datastores[id].kv.find(k); if(it!=g_datastores[id].kv.end()) return it->second; }
        return {};
    }
    if (name == "data_has") {
        auto h = argS(0); auto k = argS(1);
        if (h.rfind("data:",0)==0){ int id = std::stoi(h.substr(5)); return g_datastores[id].kv.count(k)?"true":"false"; }
        return "false";
    }
    if (name == "data_keys") {
        auto h = argS(0); if (h.rfind("data:",0)!=0) return {};
        int id = std::stoi(h.substr(5)); int lid = ++g_nextStoreId; // reuse counter for lists? safer to not; but using list is simpler via runtime would need access
        // Fallback: return joined keys since we cannot allocate list here without internal handle access
        std::ostringstream ss; bool first=true; for (auto &kv : g_datastores[id].kv){ if(!first) ss<<","; first=false; ss<<kv.first; }
        return ss.str();
    }
    if (name == "data_save") {
        auto h = argS(0); auto path = argS(1);
        if (h.rfind("data:",0)==0) { int id = std::stoi(h.substr(5)); std::ofstream out(path, std::ios::binary); for(auto &kv: g_datastores[id].kv){ out<<kv.first<<"="<<kv.second<<"\n"; } }
        return {};
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
