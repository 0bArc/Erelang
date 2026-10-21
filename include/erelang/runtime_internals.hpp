// Internal shared state exposure for builtins needing container handles
#pragma once
#include <string>
#include <vector>
#include <unordered_map>

#include "erelang/value.hpp"

namespace erelang {
// Defined in runtime/helpers.cpp (see runtime_helpers.hpp for the full set).
extern int g_nextListId;
extern int g_nextDictId;
extern int g_nextTupleId;
extern std::unordered_map<int, std::vector<Value>> g_lists;
extern std::unordered_map<int, std::unordered_map<std::string, Value>> g_dicts;
extern std::unordered_map<int, std::vector<std::string>> g_tuples;
}
