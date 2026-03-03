#include "erelang/policy.hpp"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <iostream>
#include <cctype>

using namespace erelang;
namespace fs = std::filesystem;

PolicyManager& PolicyManager::instance() { static PolicyManager inst; return inst; }

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"); if (a==std::string::npos) return {}; size_t b = s.find_last_not_of(" \t\r\n"); return s.substr(a,b-a+1);
}

void PolicyManager::load(const std::string& path) {
    if (loaded_) return; // single load for now
    loaded_ = true;
    fs::path p = path;
    if (!fs::exists(p)) {
        std::cerr << "[policy] no policy file at " << p << " (default allow except explicit deny)\n";
        policy_.defaultDeny = false;
        return;
    }
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        std::cerr << "[policy] failed to open policy file: " << path << "\n";
        return;
    }
    // Very small ad-hoc parser (key = value or [section]) lines; allow comma separated lists.
    std::string line; std::string section;
    while (std::getline(in, line)) {
        auto t = trim(line);
        if (t.empty() || t[0]=='#' || t[0]==';') continue;
        if (t.front()=='[' && t.back()==']') { section = t.substr(1,t.size()-2); continue; }
        auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        auto k = trim(t.substr(0,eq)); auto v = trim(t.substr(eq+1));
        if (k == "default_deny") {
            for (auto& c : v) c = (char)std::tolower((unsigned char)c);
            policy_.defaultDeny = (v=="1" || v=="true" || v=="yes");
        } else if (k == "allow" || k == "deny") {
            std::stringstream ss(v); std::string item;
            while (std::getline(ss, item, ',')) {
                item = trim(item); if (!item.empty()) {
                    if (k=="allow") policy_.allow.insert(item); else policy_.deny.insert(item);
                }
            }
        } else if (k == "max_threads") {
            policy_.maxThreads = std::stoi(v);
        } else if (k == "max_list_items") {
            policy_.maxListItems = (size_t)std::stoll(v);
        }
    }
    std::cerr << "[policy] loaded: default_deny=" << policy_.defaultDeny
              << " allow=" << policy_.allow.size() << " deny=" << policy_.deny.size() << "\n";
}

bool PolicyManager::is_allowed(const std::string& builtin) const {
    if (policy_.deny.count(builtin)) return false;
    if (policy_.allow.count(builtin)) return true;
    if (policy_.defaultDeny) return false;
    return true;
}
