// SPDX-License-Identifier: Apache-2.0
// Shared runtime helpers and global container state.

#include "erelang/runtime_helpers.hpp"
#include "erelang/parser.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string_view>

namespace erelang {
namespace fs = std::filesystem;

int g_nextListId = 1;
std::unordered_map<int, std::vector<std::string>> g_lists;
int g_nextDictId = 1;
std::unordered_map<int, std::unordered_map<std::string, std::string>> g_dicts;
int g_nextTupleId = 1;
std::unordered_map<int, std::vector<std::string>> g_tuples;
int g_nextEnumVarId = 1;
std::unordered_map<int, EnumVarState> g_enumVars;
int g_nextPtrId = 1;
std::unordered_map<int, std::string> g_ptrs;
std::unordered_map<int, RawMemBlock> g_rawMem;
MemDebugStats g_memStats{};
std::unordered_set<int> g_freedPtrIds;
int g_nextOwnId = 1;
std::unordered_map<int, OwnState> g_owns;
int g_nextSharedId = 1;
std::unordered_map<int, SharedState> g_shareds;
int g_nextWeakId = 1;
std::unordered_map<int, WeakState> g_weaks;
int g_nextBufferId = 1;
std::unordered_map<int, BufferState> g_buffers;
int g_nextFileId = 1;
std::unordered_map<int, std::unique_ptr<std::fstream>> g_fileStreams;
std::unordered_map<int, FileBufState> g_fileBufs;
int g_nextStrBufId = 1;
std::unordered_map<int, std::string> g_strBuffers;
std::unordered_set<std::string> g_deprecationWarningsShown;
int g_nextSetId = 1;
std::unordered_map<int, std::unordered_set<std::string>> g_sets;
int g_nextQueueId = 1;
std::unordered_map<int, std::deque<std::string>> g_queues;
int g_nextChanId = 1;
std::unordered_map<int, std::shared_ptr<ChannelState>> g_channels;
int g_nextMutexId = 1;
std::unordered_map<int, std::shared_ptr<ScriptMutex>> g_mutexes;
int g_nextClosureId = 1;
std::unordered_map<int, ClosureData*> g_closures;
int g_nextFutureId = 1;
std::unordered_map<int, std::shared_ptr<FutureState>> g_futures;
thread_local std::shared_ptr<FutureState> tls_current_future;

void notify_channels_for_cancel() {
    for (auto& kv : g_channels) {
        if (kv.second) kv.second->cv.notify_all();
    }
}

namespace {

struct AsyncPoolState {
    std::mutex mu;
    std::condition_variable taskCv;
    std::deque<std::function<void()>> tasks;
    std::vector<std::thread> workers;
    int busyWorkers{0};
    int poolSize{0};
    bool stopping{false};
    bool started{false};
};

AsyncPoolState& async_pool() {
    static AsyncPoolState state;
    return state;
}

bool try_pop_async_task(std::function<void()>& out) {
    auto& pool = async_pool();
    std::lock_guard<std::mutex> lock(pool.mu);
    if (pool.tasks.empty()) return false;
    out = std::move(pool.tasks.front());
    pool.tasks.pop_front();
    return true;
}

void ensure_async_pool_started() {
    auto& pool = async_pool();
    std::lock_guard<std::mutex> lock(pool.mu);
    if (pool.started) return;
    unsigned hc = std::thread::hardware_concurrency();
    if (hc < 2) hc = 2;
    pool.poolSize = static_cast<int>(hc);
    pool.started = true;
    for (int i = 0; i < pool.poolSize; ++i) {
        pool.workers.emplace_back([] {
            auto& p = async_pool();
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lk(p.mu);
                    p.taskCv.wait(lk, [&] { return p.stopping || !p.tasks.empty(); });
                    if (p.stopping && p.tasks.empty()) return;
                    task = std::move(p.tasks.front());
                    p.tasks.pop_front();
                    ++p.busyWorkers;
                }
                try {
                    task();
                } catch (...) {
                }
                {
                    std::lock_guard<std::mutex> lk(p.mu);
                    --p.busyWorkers;
                }
            }
        });
    }
}

} // namespace

void async_pool_submit(std::function<void()> task) {
    ensure_async_pool_started();
    auto& pool = async_pool();
    {
        std::lock_guard<std::mutex> lock(pool.mu);
        pool.tasks.push_back(std::move(task));
    }
    pool.taskCv.notify_one();
}

void async_pool_release_slot() {
    auto& pool = async_pool();
    if (!pool.started) return;
    std::lock_guard<std::mutex> lock(pool.mu);
    if (pool.busyWorkers > 0) --pool.busyWorkers;
}

void async_pool_acquire_slot() {
    auto& pool = async_pool();
    if (!pool.started) return;
    std::lock_guard<std::mutex> lock(pool.mu);
    ++pool.busyWorkers;
}

void async_pool_help_while_waiting(const std::shared_ptr<FutureState>& fut) {
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(fut->mu);
            if (fut->done || fut->cancelled) return;
        }
        std::function<void()> task;
        if (try_pop_async_task(task)) {
            try {
                task();
            } catch (...) {
            }
            continue;
        }
        std::unique_lock<std::mutex> lock(fut->mu);
        fut->cv.wait_for(lock, std::chrono::milliseconds(1), [&] { return fut->done || fut->cancelled; });
        if (fut->done || fut->cancelled) return;
    }
}

void async_pool_shutdown() {
    auto& pool = async_pool();
    {
        std::lock_guard<std::mutex> lock(pool.mu);
        if (!pool.started) return;
        pool.stopping = true;
    }
    pool.taskCv.notify_all();
    for (auto& w : pool.workers) {
        if (w.joinable()) w.join();
    }
    pool.workers.clear();
    pool.started = false;
    pool.stopping = false;
    pool.busyWorkers = 0;
    pool.poolSize = 0;
    pool.tasks.clear();
}

void reset_global_container_state() {
    g_memStats.leaks_at_reset += mem_live_count();
    g_lists.clear();
    g_dicts.clear();
    g_tuples.clear();
    g_enumVars.clear();
    g_ptrs.clear();
    g_rawMem.clear();
    g_owns.clear();
    g_shareds.clear();
    g_weaks.clear();
    g_buffers.clear();
    g_freedPtrIds.clear();
    g_fileStreams.clear();
    g_fileBufs.clear();
    g_strBuffers.clear();
    g_sets.clear();
    g_queues.clear();
    g_channels.clear();
    g_mutexes.clear();
    g_memStats.live = 0;
    {
        std::vector<std::shared_ptr<FutureState>> pending;
        for (auto& kv : g_futures) {
            if (kv.second) pending.push_back(kv.second);
        }
        for (auto& fut : pending) {
            {
                std::lock_guard<std::mutex> cancelLock(fut->mu);
                fut->cancelled = true;
            }
            fut->cv.notify_all();
            std::unique_lock<std::mutex> lock(fut->mu);
            fut->cv.wait(lock, [&] { return fut->done; });
        }
    }
    g_futures.clear();
    for (auto& kv : g_closures) { if (kv.second) { kv.second->refCount = 1; kv.second->release(); } }
    g_closures.clear();
    g_nextListId = 1;
    g_nextDictId = 1;
    g_nextTupleId = 1;
    g_nextEnumVarId = 1;
    g_nextPtrId = 1;
    g_nextOwnId = 1;
    g_nextSharedId = 1;
    g_nextWeakId = 1;
    g_nextBufferId = 1;
    g_nextFileId = 1;
    g_nextStrBufId = 1;
    g_nextSetId = 1;
    g_nextQueueId = 1;
    g_nextChanId = 1;
    g_nextMutexId = 1;
    g_nextClosureId = 1;
    g_nextFutureId = 1;
}

std::string slurp_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string trim_copy(std::string_view value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return std::string{value.substr(begin, end - begin + 1)};
}

std::string join_strings(std::vector<std::string> items, char separator) {
    if (items.empty()) {
        return {};
    }
    std::sort(items.begin(), items.end());
    std::ostringstream oss;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i) {
            oss << separator;
        }
        oss << items[i];
    }
    return oss.str();
}

std::pair<std::string, std::string> split_core_query(const std::string& query) {
    const auto trimmed = trim_copy(query);
    if (trimmed.empty()) {
        return {"", ""};
    }
    auto pos = trimmed.find(':');
    if (pos == std::string::npos) pos = trimmed.find('.');
    if (pos == std::string::npos) pos = trimmed.find('/');
    if (pos == std::string::npos) {
        return {"", trimmed};
    }
    auto left = trim_copy(trimmed.substr(0, pos));
    auto right = trim_copy(trimmed.substr(pos + 1));
    return {left, right};
}

int64_t to_int(const std::string& s) {
    try {
        return std::stoll(s);
    } catch (...) {
        return 0;
    }
}

std::string format_pointer_handle(int id) {
    std::ostringstream oss;
    oss << "0x" << std::uppercase << std::hex << static_cast<unsigned int>(id);
    return oss.str();
}

std::optional<int> parse_pointer_handle(const std::string& handle) {
    if (handle.rfind("ptr:", 0) == 0) {
        return static_cast<int>(to_int(handle.substr(4)));
    }
    if (handle.size() > 2 && handle[0] == '0' && (handle[1] == 'x' || handle[1] == 'X')) {
        try {
            std::size_t idx = 0;
            unsigned long parsed = std::stoul(handle, &idx, 16);
            if (idx == handle.size()) return static_cast<int>(parsed);
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::filesystem::path path_from_u8(const std::string& s) {
    const auto* first = reinterpret_cast<const char8_t*>(s.data());
    const auto* last = first + s.size();
    return std::filesystem::path(std::u8string(first, last));
}

std::filesystem::path infer_entry_script_directory(const Program& program) {
    auto parent_of = [](const std::string& sourcePath) -> std::filesystem::path {
        if (sourcePath.empty()) {
            return {};
        }
        return std::filesystem::path(sourcePath).parent_path();
    };
    const std::string entry = program.runTarget.value_or("main");
    for (const auto& action : program.actions) {
        if (action.name == entry && !action.sourcePath.empty()) {
            return parent_of(action.sourcePath);
        }
    }
    for (const auto& action : program.actions) {
        if (action.name == "main" && !action.sourcePath.empty()) {
            return parent_of(action.sourcePath);
        }
    }
    for (const auto& action : program.actions) {
        if (!action.sourcePath.empty()) {
            return parent_of(action.sourcePath);
        }
    }
    return {};
}

std::filesystem::path resolve_filesystem_path(
    const std::string& raw,
    const std::filesystem::path& scriptDirectory) {
    std::filesystem::path p = path_from_u8(raw);
    if (p.is_absolute()) {
        return p.lexically_normal();
    }
    if (!scriptDirectory.empty()) {
        return (scriptDirectory / p).lexically_normal();
    }
    return (fs::current_path() / p).lexically_normal();
}

double to_double(const std::string& s) {
    try {
        return std::stod(s);
    } catch (...) {
        return 0.0;
    }
}

bool is_float_string(const std::string& s) {
    if (s.empty()) return false;
    bool hasPoint = false;
    bool hasExp = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '.') hasPoint = true;
        if (c == 'e' || c == 'E') hasExp = true;
        if ((c == '+' || c == '-') && i != 0 && !(s[i - 1] == 'e' || s[i - 1] == 'E')) return false;
        if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == '+' || c == '-' || c == 'e' || c == 'E')) return false;
    }
    if (!hasPoint && !hasExp) return false;
    try {
        std::size_t idx;
        std::stod(s, &idx);
        return idx == s.size();
    } catch (...) {
        return false;
    }
}

std::string normalize_runtime_type_name(std::string typeName) {
    std::string out;
    out.reserve(typeName.size());
    for (char ch : typeName) {
        if (!std::isspace(static_cast<unsigned char>(ch))) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
    }
    return out;
}

bool parse_runtime_array_type(const std::string& typeName, std::string& elementType) {
    constexpr const char* prefix = "array<";
    if (typeName.rfind(prefix, 0) != 0 || typeName.back() != '>') return false;
    elementType = typeName.substr(6, typeName.size() - 7);
    return !elementType.empty();
}

bool parse_runtime_map_type(const std::string& typeName, std::string& keyType, std::string& valueType) {
    constexpr const char* prefix = "map<";
    if (typeName.rfind(prefix, 0) != 0 || typeName.back() != '>') return false;
    const std::string inner = typeName.substr(4, typeName.size() - 5);
    int depth = 0;
    for (size_t index = 0; index < inner.size(); ++index) {
        const char ch = inner[index];
        if (ch == '<') {
            ++depth;
            continue;
        }
        if (ch == '>') {
            --depth;
            continue;
        }
        if (ch == ',' && depth == 0) {
            keyType = inner.substr(0, index);
            valueType = inner.substr(index + 1);
            return !keyType.empty() && !valueType.empty();
        }
    }
    return false;
}

bool parse_runtime_set_type(const std::string& typeName, std::string& elementType) {
    constexpr const char* prefix = "set<";
    if (typeName.rfind(prefix, 0) != 0 || typeName.back() != '>') return false;
    elementType = typeName.substr(4, typeName.size() - 5);
    return !elementType.empty();
}

bool runtime_generic_compatible(const std::string& expected, const std::string& actual) {
    if (expected == "any" || expected == "unknown") return true;
    if (actual == "any" || actual == "unknown") return true;
    return expected == actual;
}

bool runtime_declared_type_matches(const std::string& declaredTypeRaw, const std::string& actualTypeRaw) {
    const std::string declaredType = normalize_runtime_type_name(declaredTypeRaw);
    const std::string actualType = normalize_runtime_type_name(actualTypeRaw);
    if (declaredType.empty() || declaredType == "auto" || declaredType == "any") return true;
    if (declaredType == actualType) return true;

    if (declaredType == "string" || declaredType == "str" || declaredType == "char") {
        return actualType == "string";
    }
    if (declaredType == "bool") return actualType == "bool";
    if (declaredType == "int" || declaredType == "u8" || declaredType == "u16" || declaredType == "u32" || declaredType == "u64" ||
        declaredType == "i8" || declaredType == "i16" || declaredType == "i32" || declaredType == "i64" ||
        declaredType == "uint" || declaredType == "unsigned" || declaredType == "unsignedint") {
        return actualType == "int";
    }
    if (declaredType == "double" || declaredType == "float") {
        return actualType == "double" || actualType == "int";
    }

    if (declaredType == "array") return actualType.rfind("array", 0) == 0;
    std::string declaredArrayElem;
    if (parse_runtime_array_type(declaredType, declaredArrayElem)) {
        std::string actualArrayElem;
        if (!parse_runtime_array_type(actualType, actualArrayElem)) return false;
        return runtime_generic_compatible(declaredArrayElem, actualArrayElem);
    }

    if (declaredType == "map" || declaredType == "dictionary") return actualType.rfind("map", 0) == 0;
    std::string declaredMapKey;
    std::string declaredMapValue;
    if (parse_runtime_map_type(declaredType, declaredMapKey, declaredMapValue)) {
        std::string actualMapKey;
        std::string actualMapValue;
        if (!parse_runtime_map_type(actualType, actualMapKey, actualMapValue)) return false;
        return runtime_generic_compatible(declaredMapKey, actualMapKey) && runtime_generic_compatible(declaredMapValue, actualMapValue);
    }

    if (declaredType == "set") return actualType.rfind("set", 0) == 0;
    std::string declaredSetElem;
    if (parse_runtime_set_type(declaredType, declaredSetElem)) {
        std::string actualSetElem;
        if (!parse_runtime_set_type(actualType, actualSetElem)) return false;
        return runtime_generic_compatible(declaredSetElem, actualSetElem);
    }

    return true;
}

bool is_int_string(const std::string& s) {
    if (s.empty()) return false;
    size_t i = 0;
    if (s[0] == '-' || s[0] == '+') i = 1;
    if (i >= s.size()) return false;
    for (; i < s.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

bool is_truthy(const std::string& v) {
    if (v == "true") return true;
    if (v == "false") return false;
    return to_int(v) != 0;
}

bool is_identifier_text(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    auto is_alpha_or_underscore = [](char ch) {
        return std::isalpha(static_cast<unsigned char>(ch)) || ch == '_';
    };
    auto is_alnum_or_underscore = [](char ch) {
        return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
    };
    if (!is_alpha_or_underscore(text.front())) {
        return false;
    }
    for (size_t i = 1; i < text.size(); ++i) {
        if (!is_alnum_or_underscore(text[i])) {
            return false;
        }
    }
    return true;
}

const StructDecl* find_struct_decl(const Program& program, std::string_view name) {
    std::string_view bare = name;
    if (bare.rfind("struct:", 0) == 0) bare = bare.substr(7);
    const auto lt = bare.find('<');
    if (lt != std::string_view::npos) bare = bare.substr(0, lt);
    for (const auto& s : program.structs) {
        if (s.name == bare) return &s;
    }
    return nullptr;
}

const Action* find_struct_method(const StructDecl& decl, std::string_view name) {
    for (const auto& m : decl.methods) {
        if (m.name == name) return &m;
    }
    return nullptr;
}

const EnumDecl* find_enum_decl(const Program& program, std::string_view name) {
    std::string_view bare = name;
    if (bare.rfind("enum:", 0) == 0) bare = bare.substr(5);
    const auto lt = bare.find('<');
    if (lt != std::string_view::npos) bare = bare.substr(0, lt);
    for (const auto& e : program.enums) {
        if (e.name == bare) return &e;
    }
    return nullptr;
}

const Action* find_enum_method(const EnumDecl& decl, std::string_view name) {
    for (const auto& m : decl.methods) {
        if (m.name == name) return &m;
    }
    return nullptr;
}

namespace {

ExprPtr make_ident(std::string name) {
    return std::make_shared<Expr>(Expr{ ExprIdent{ std::move(name) } });
}

ExprPtr make_string(std::string text) {
    return std::make_shared<Expr>(Expr{ ExprString{ std::move(text) } });
}

PatternPtr make_binding(std::string name) {
    return std::make_shared<Pattern>(Pattern{ PatBinding{ std::move(name) } });
}

PatternPtr make_ctor(std::string name, std::vector<PatternPtr> args) {
    return std::make_shared<Pattern>(Pattern{ PatCtor{ std::move(name), std::move(args) } });
}

std::shared_ptr<Block> make_return_block(ExprPtr value) {
    auto body = std::make_shared<Block>();
    body->stmts.push_back(ReturnStmt{ std::move(value) });
    return body;
}

std::shared_ptr<Block> make_fail_block(std::string message) {
    auto body = std::make_shared<Block>();
    FunctionCallExpr call;
    call.name = "fail";
    call.args.push_back(make_string(std::move(message)));
    body->stmts.push_back(ExprStmt{ std::make_shared<Expr>(Expr{ std::move(call) }) });
    return body;
}

bool enum_has_method(const EnumDecl& decl, std::string_view name) {
    for (const auto& m : decl.methods) {
        if (m.name == name) return true;
    }
    return false;
}

std::string enum_bare_name(const std::string& name) {
    const auto pos = name.rfind("::");
    if (pos == std::string::npos) return name;
    return name.substr(pos + 2);
}

Action make_option_unwrap() {
    Action a;
    a.name = "unwrap";
    a.returnType = "T";
    a.visibility = Visibility::Public;
    MatchStmt ms;
    ms.selector = make_ident("self");
    ms.cases.push_back(MatchCase{ make_ctor("Some", { make_binding("v") }), make_return_block(make_ident("v")) });
    ms.cases.push_back(MatchCase{ make_ctor("None", {}), make_fail_block("unwrap() called on `None`") });
    a.body.stmts.push_back(std::move(ms));
    return a;
}

Action make_option_unwrap_or() {
    Action a;
    a.name = "unwrap_or";
    a.params.push_back(Param{ "default", "T" });
    a.returnType = "T";
    a.visibility = Visibility::Public;
    MatchStmt ms;
    ms.selector = make_ident("self");
    ms.cases.push_back(MatchCase{ make_ctor("Some", { make_binding("v") }), make_return_block(make_ident("v")) });
    ms.cases.push_back(MatchCase{ make_ctor("None", {}), make_return_block(make_ident("default")) });
    a.body.stmts.push_back(std::move(ms));
    return a;
}

Action make_result_unwrap() {
    Action a;
    a.name = "unwrap";
    a.returnType = "T";
    a.visibility = Visibility::Public;
    MatchStmt ms;
    ms.selector = make_ident("self");
    ms.cases.push_back(MatchCase{ make_ctor("Ok", { make_binding("v") }), make_return_block(make_ident("v")) });
    ms.cases.push_back(MatchCase{ make_ctor("Error", { make_binding("e") }), make_fail_block("unwrap() called on `Error`") });
    a.body.stmts.push_back(std::move(ms));
    return a;
}

Action make_result_ok() {
    Action a;
    a.name = "ok";
    a.returnType = "Option<T>";
    a.visibility = Visibility::Public;
    MatchStmt ms;
    ms.selector = make_ident("self");
    {
        FunctionCallExpr someCall;
        someCall.name = "Option.Some";
        someCall.args.push_back(make_ident("v"));
        ms.cases.push_back(MatchCase{
            make_ctor("Ok", { make_binding("v") }),
            make_return_block(std::make_shared<Expr>(Expr{ std::move(someCall) }))
        });
    }
    {
        FunctionCallExpr noneCall;
        noneCall.name = "Option.None";
        ms.cases.push_back(MatchCase{
            make_ctor("Error", { make_binding("e") }),
            make_return_block(std::make_shared<Expr>(Expr{ std::move(noneCall) }))
        });
    }
    a.body.stmts.push_back(std::move(ms));
    return a;
}

} // namespace

void inject_standard_enum_methods(Program& program) {
    for (auto& en : program.enums) {
        const std::string bare = enum_bare_name(en.name);
        if (bare == "Option") {
            if (!enum_has_method(en, "unwrap")) en.methods.push_back(make_option_unwrap());
            if (!enum_has_method(en, "unwrap_or")) en.methods.push_back(make_option_unwrap_or());
        } else if (bare == "Result") {
            if (!enum_has_method(en, "unwrap")) en.methods.push_back(make_result_unwrap());
            if (!enum_has_method(en, "ok")) en.methods.push_back(make_result_ok());
        }
    }
}

std::string encode_enum_variant(const std::string& tag, const std::vector<std::string>& payloads) {
    if (payloads.empty()) {
        return std::string("enumvar:") + tag;
    }
    const int id = g_nextEnumVarId++;
    g_enumVars[id] = EnumVarState{tag, payloads};
    return std::string("enumvar:#") + std::to_string(id);
}

bool peek_enum_tag(const std::string& encoded, std::string& tagOut) {
    constexpr const char* kEnumVar = "enumvar:";
    if (encoded.rfind(kEnumVar, 0) != 0) {
        tagOut = encoded;
        return false;
    }
    const std::string body = encoded.substr(std::char_traits<char>::length(kEnumVar));
    if (!body.empty() && body[0] == '#') {
        try {
            const int id = static_cast<int>(std::stoll(body.substr(1)));
            auto it = g_enumVars.find(id);
            if (it == g_enumVars.end()) {
                tagOut = body;
                return false;
            }
            tagOut = it->second.tag;
            return true;
        } catch (...) {
            tagOut = body;
            return false;
        }
    }
    const size_t sep = body.find('\x1f');
    tagOut = (sep == std::string::npos) ? body : body.substr(0, sep);
    return true;
}

bool decode_enum_variant(const std::string& encoded, std::string& tagOut, std::vector<std::string>& payloadsOut) {
    payloadsOut.clear();
    constexpr const char* kEnumVar = "enumvar:";
    if (encoded.rfind(kEnumVar, 0) != 0) {
        tagOut = encoded;
        return false;
    }
    const std::string body = encoded.substr(std::char_traits<char>::length(kEnumVar));
    if (!body.empty() && body[0] == '#') {
        try {
            const int id = static_cast<int>(std::stoll(body.substr(1)));
            auto it = g_enumVars.find(id);
            if (it == g_enumVars.end()) {
                tagOut = body;
                return false;
            }
            tagOut = it->second.tag;
            payloadsOut = it->second.payloads;
            return true;
        } catch (...) {
            tagOut = body;
            return false;
        }
    }
    const size_t sep = body.find('\x1f');
    if (sep == std::string::npos) {
        tagOut = body;
        return true;
    }
    tagOut = body.substr(0, sep);
    const std::string rest = body.substr(sep + 1);

    auto all_digits = [](std::string_view s) {
        if (s.empty()) return false;
        for (char c : s) {
            if (c < '0' || c > '9') return false;
        }
        return true;
    };

    std::vector<std::string> prefixed;
    size_t pos = 0;
    bool okPrefixed = true;
    while (pos < rest.size()) {
        if (pos > 0) {
            if (rest[pos] != '\x1f') { okPrefixed = false; break; }
            ++pos;
        }
        const size_t colon = rest.find(':', pos);
        if (colon == std::string::npos) { okPrefixed = false; break; }
        const std::string lenStr = rest.substr(pos, colon - pos);
        if (!all_digits(lenStr)) { okPrefixed = false; break; }
        size_t len = 0;
        try { len = static_cast<size_t>(std::stoull(lenStr)); } catch (...) { okPrefixed = false; break; }
        if (colon + 1 + len > rest.size()) { okPrefixed = false; break; }
        prefixed.push_back(rest.substr(colon + 1, len));
        pos = colon + 1 + len;
    }
    if (okPrefixed && pos == rest.size()) {
        payloadsOut = std::move(prefixed);
        return true;
    }

    pos = 0;
    while (pos < rest.size()) {
        const size_t next = rest.find('\x1f', pos);
        if (next == std::string::npos) {
            payloadsOut.push_back(rest.substr(pos));
            break;
        }
        payloadsOut.push_back(rest.substr(pos, next - pos));
        pos = next + 1;
    }
    return true;
}

bool enum_variants_equal(const std::string& left, const std::string& right) {
    if (left == right) return true;
    std::string ltag;
    std::string rtag;
    std::vector<std::string> lpayloads;
    std::vector<std::string> rpayloads;
    const bool lEnum = decode_enum_variant(left, ltag, lpayloads);
    const bool rEnum = decode_enum_variant(right, rtag, rpayloads);
    if (!lEnum && !rEnum) return false;
    if (!lEnum) {
        ltag = left;
        lpayloads.clear();
    }
    if (!rEnum) {
        rtag = right;
        rpayloads.clear();
    }
    if (ltag != rtag || lpayloads.size() != rpayloads.size()) return false;
    for (size_t i = 0; i < lpayloads.size(); ++i) {
        if (!enum_variants_equal(lpayloads[i], rpayloads[i]) && lpayloads[i] != rpayloads[i]) {
            return false;
        }
    }
    return true;
}

} // namespace erelang
