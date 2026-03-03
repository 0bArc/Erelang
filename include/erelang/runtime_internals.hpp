// Internal shared state exposure for builtins needing container handles
#pragma once
#include <string>
#include <vector>
#include <unordered_map>

namespace erelang {
// These are defined in runtime.cpp
extern int g_nextListId;
extern int g_nextDictId;
extern std::unordered_map<int, std::vector<std::string>> g_lists;
extern std::unordered_map<int, std::unordered_map<std::string, std::string>> g_dicts;
}
