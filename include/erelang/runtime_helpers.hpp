#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace erelang {

struct Program;
struct StructDecl;
struct Action;

extern int g_nextListId;
extern int g_nextDictId;
extern int g_nextPtrId;
extern int g_nextFileId;
extern int g_nextStrBufId;
extern int g_nextSetId;
extern int g_nextQueueId;
extern std::unordered_map<int, std::vector<std::string>> g_lists;
extern std::unordered_map<int, std::unordered_map<std::string, std::string>> g_dicts;
extern std::unordered_map<int, std::string> g_ptrs;
extern std::unordered_map<int, std::unique_ptr<std::fstream>> g_fileStreams;
extern std::unordered_map<int, std::string> g_strBuffers;
extern std::unordered_set<std::string> g_deprecationWarningsShown;
extern std::unordered_map<int, std::unordered_set<std::string>> g_sets;
extern std::unordered_map<int, std::deque<std::string>> g_queues;

[[nodiscard]] std::string slurp_text(const std::filesystem::path& p);
[[nodiscard]] std::string trim_copy(std::string_view value);
// Clear per-run global container state (lists, dicts, pointers, files, sets,
// queues, string buffers) so repeated run() calls do not leak IDs or handles.
void reset_global_container_state();
[[nodiscard]] std::string join_strings(std::vector<std::string> items, char separator = ',');
[[nodiscard]] std::pair<std::string, std::string> split_core_query(const std::string& query);
[[nodiscard]] int64_t to_int(const std::string& s);
[[nodiscard]] std::string format_pointer_handle(int id);
[[nodiscard]] std::optional<int> parse_pointer_handle(const std::string& handle);
[[nodiscard]] std::filesystem::path path_from_u8(const std::string& s);
[[nodiscard]] std::filesystem::path infer_entry_script_directory(const Program& program);
[[nodiscard]] std::filesystem::path resolve_filesystem_path(
    const std::string& raw,
    const std::filesystem::path& scriptDirectory);
[[nodiscard]] double to_double(const std::string& s);
[[nodiscard]] bool is_float_string(const std::string& s);
[[nodiscard]] bool is_int_string(const std::string& s);
[[nodiscard]] bool is_truthy(const std::string& v);
[[nodiscard]] bool is_identifier_text(std::string_view text);
[[nodiscard]] std::string normalize_runtime_type_name(std::string typeName);
[[nodiscard]] bool parse_runtime_array_type(const std::string& typeName, std::string& elementType);
[[nodiscard]] bool parse_runtime_map_type(const std::string& typeName, std::string& keyType, std::string& valueType);
[[nodiscard]] bool runtime_generic_compatible(const std::string& expected, const std::string& actual);
[[nodiscard]] bool runtime_declared_type_matches(const std::string& declaredTypeRaw, const std::string& actualTypeRaw);
[[nodiscard]] const StructDecl* find_struct_decl(const Program& program, std::string_view name);
[[nodiscard]] const Action* find_struct_method(const StructDecl& decl, std::string_view name);

} // namespace erelang
