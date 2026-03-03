#pragma once
#include <string>
#include <unordered_map>
namespace erelang {
struct Symbol { std::string name; std::string kind; };
class SymbolTable {
    std::unordered_map<std::string, Symbol> map_;
public:
    void add(const std::string& n, const std::string& kind) { map_[n] = Symbol{n,kind}; }
    const Symbol* find(const std::string& n) const { auto it = map_.find(n); return it==map_.end()?nullptr:&it->second; }
};
}
