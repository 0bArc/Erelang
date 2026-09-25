#pragma once

#include <cstdint>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "erelang/value.hpp"

namespace erelang {

struct Program;
struct StructDecl;
struct EnumDecl;
struct Action;
struct ClosureData;

extern int g_nextListId;
extern int g_nextDictId;
extern int g_nextTupleId;
extern int g_nextEnumVarId;
extern int g_nextPtrId;
extern int g_nextFileId;
extern int g_nextStrBufId;
extern int g_nextSetId;
extern int g_nextQueueId;
extern int g_nextClosureId;
extern int g_nextOwnId;
extern int g_nextSharedId;
extern int g_nextWeakId;
extern int g_nextBufferId;
extern std::unordered_map<int, std::vector<std::string>> g_lists;
extern std::unordered_map<int, std::unordered_map<std::string, std::string>> g_dicts;
extern std::unordered_map<int, std::vector<std::string>> g_tuples;

struct EnumVarState {
    std::string tag;
    std::vector<std::string> payloads;
};
extern std::unordered_map<int, EnumVarState> g_enumVars;
extern std::unordered_map<int, std::string> g_ptrs;

struct RawMemBlock {
    std::string elemType;
    std::size_t elemSize{8};
    std::shared_ptr<std::vector<Value>> cells;
    std::size_t index{0};
};
extern std::unordered_map<int, RawMemBlock> g_rawMem;

struct OwnState {
    std::string elemType;
    Value payload;
    bool alive{true};
};
extern std::unordered_map<int, OwnState> g_owns;

struct SharedState {
    std::string elemType;
    Value payload;
    int strong{0};
    int weak{0};
};
extern std::unordered_map<int, SharedState> g_shareds;

struct WeakState {
    int sharedId{-1};
};
extern std::unordered_map<int, WeakState> g_weaks;

struct BufferState {
    std::string elemType;
    std::vector<Value> data;
    std::size_t capacity{0};
};
extern std::unordered_map<int, BufferState> g_buffers;

struct FileBufState {
    std::size_t capacity{4096};
    std::string writePending;
    std::string readCache;
    std::size_t readPos{0};
};
extern std::unordered_map<int, std::unique_ptr<std::fstream>> g_fileStreams;
extern std::unordered_map<int, FileBufState> g_fileBufs;
extern std::unordered_map<int, std::string> g_strBuffers;
extern std::unordered_map<int, ClosureData*> g_closures;
extern std::unordered_set<std::string> g_deprecationWarningsShown;
extern std::unordered_map<int, std::unordered_set<std::string>> g_sets;
extern std::unordered_map<int, std::deque<std::string>> g_queues;

struct MemDebugStats {
    std::uint64_t allocs{0};
    std::uint64_t frees{0};
    std::uint64_t live{0};
    std::uint64_t double_frees{0};
    std::uint64_t use_after_free{0};
    std::uint64_t leaks_at_reset{0};
};

extern MemDebugStats g_memStats;
extern std::unordered_set<int> g_freedPtrIds;

[[nodiscard]] std::size_t mem_elem_size(std::string_view typeName);
[[nodiscard]] Value mem_zero_value(std::string_view typeName);
[[nodiscard]] Value mem_alloc(std::string_view elemType, std::size_t count);
void mem_free_ptr(const Value& ptr);
[[nodiscard]] Value mem_realloc(const Value& ptr, std::size_t count);
void mem_copy(const Value& dst, const Value& src, std::size_t count);
void mem_move(const Value& dst, const Value& src, std::size_t count);
void mem_fill(const Value& ptr, const Value& value, std::size_t count);
void mem_zero(const Value& ptr, std::size_t count);
[[nodiscard]] Value mem_deref(const Value& ptr);
void mem_ptr_set(const Value& ptr, const Value& value);
[[nodiscard]] Value mem_ptr_add(const Value& ptr, std::int64_t delta);
[[nodiscard]] bool mem_is_ptr(const Value& v);
[[nodiscard]] bool mem_is_owner(const Value& v);
[[nodiscard]] std::uint64_t mem_live_count();
[[nodiscard]] Value mem_stats_value();
void mem_reset_debug_stats();

[[nodiscard]] Value mem_make_own(std::string_view elemType, Value payload);
[[nodiscard]] Value mem_make_shared(std::string_view elemType, Value payload);
[[nodiscard]] Value mem_make_weak(const Value& shared);
[[nodiscard]] Value mem_weak_get(const Value& weak);
[[nodiscard]] Value mem_make_buffer(std::string_view elemType, std::size_t count);
[[nodiscard]] Value mem_own_deref(const Value& own);
void mem_own_set(const Value& own, const Value& value);
[[nodiscard]] Value mem_shared_deref(const Value& shared);
void mem_shared_set(const Value& shared, const Value& value);

void mem_retain(const Value& v);
void mem_release(const Value& v);
[[nodiscard]] Value mem_take_own(Value& source);
[[nodiscard]] bool mem_try_move_own_ident(ValueMap& vars, const std::string& name, Value& out);

[[nodiscard]] Value mem_buffer_index_get(const Value& buf, std::int64_t index);
void mem_buffer_index_set(const Value& buf, std::int64_t index, const Value& value);
[[nodiscard]] Value mem_buffer_method(const Value& buf, std::string_view method, const std::vector<Value>& args);

struct ChannelState {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::string> q;
    int capacity{0};
    bool closed{false};
};

struct ScriptMutex {
    std::mutex mu;
};

extern int g_nextChanId;
extern int g_nextMutexId;
extern std::unordered_map<int, std::shared_ptr<ChannelState>> g_channels;
extern std::unordered_map<int, std::shared_ptr<ScriptMutex>> g_mutexes;

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
[[nodiscard]] bool parse_runtime_set_type(const std::string& typeName, std::string& elementType);
[[nodiscard]] bool runtime_generic_compatible(const std::string& expected, const std::string& actual);
[[nodiscard]] bool runtime_declared_type_matches(const std::string& declaredTypeRaw, const std::string& actualTypeRaw);
[[nodiscard]] const StructDecl* find_struct_decl(const Program& program, std::string_view name);
[[nodiscard]] const Action* find_struct_method(const StructDecl& decl, std::string_view name);
[[nodiscard]] const EnumDecl* find_enum_decl(const Program& program, std::string_view name);
[[nodiscard]] const Action* find_enum_method(const EnumDecl& decl, std::string_view name);
void inject_standard_enum_methods(Program& program);

[[nodiscard]] std::string encode_enum_variant(const std::string& tag, const std::vector<std::string>& payloads);
[[nodiscard]] bool decode_enum_variant(const std::string& encoded, std::string& tagOut, std::vector<std::string>& payloadsOut);
[[nodiscard]] bool peek_enum_tag(const std::string& encoded, std::string& tagOut);
[[nodiscard]] bool enum_variants_equal(const std::string& left, const std::string& right);

struct FutureState {
    std::mutex mu;
    std::condition_variable cv;
    bool done{false};
    bool failed{false};
    bool cancelled{false};
    std::string error;
    Value result;
    std::unordered_map<std::string, Value> returnFields;
};

extern thread_local std::shared_ptr<FutureState> tls_current_future;
void notify_channels_for_cancel();

void async_pool_submit(std::function<void()> task);
void async_pool_release_slot();
void async_pool_acquire_slot();
void async_pool_help_while_waiting(const std::shared_ptr<FutureState>& fut);
void async_pool_shutdown();
void async_root_enter();
void async_root_leave();
[[nodiscard]] bool async_in_async_action();
void async_set_in_async_action(bool value);

extern int g_nextFutureId;
extern std::unordered_map<int, std::shared_ptr<FutureState>> g_futures;

struct TryPropagateException {
    Value value;
};

} // namespace erelang
