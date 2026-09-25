// SPDX-License-Identifier: Apache-2.0
#include "erelang/runtime_helpers.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace erelang {
namespace {

std::string lower_type(std::string_view t) {
    std::string out(t);
    out.erase(std::remove_if(out.begin(), out.end(), [](unsigned char c) { return std::isspace(c) != 0; }), out.end());
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

std::optional<int> ptr_id_of(const Value& v) {
    if (v.kind == ValueKind::Handle && v.h.kind == HandleKind::Ptr) {
        return static_cast<int>(v.h.id);
    }
    return parse_pointer_handle(to_display_string(v));
}

RawMemBlock* raw_at(int id) {
    auto it = g_rawMem.find(id);
    if (it == g_rawMem.end()) return nullptr;
    return &it->second;
}

void destroy_shared_if_unused(int id) {
    auto it = g_shareds.find(id);
    if (it == g_shareds.end()) return;
    if (it->second.strong > 0) return;
    it->second.payload = Value::null_value();
    if (it->second.weak <= 0) g_shareds.erase(it);
}

} // namespace

std::size_t mem_elem_size(std::string_view typeName) {
    const std::string t = lower_type(typeName);
    if (t == "bool" || t == "u8" || t == "i8" || t == "char") return 1;
    if (t == "u16" || t == "i16") return 2;
    if (t == "float" || t == "u32" || t == "i32") return 4;
    if (t == "double" || t == "int" || t == "i64" || t == "u64" || t == "long" || t == "pointer") return 8;
    if (!t.empty() && (t.front() == '*' || t.back() == '*')) return 8;
    return 8;
}

Value mem_zero_value(std::string_view typeName) {
    const std::string t = lower_type(typeName);
    if (t == "bool") return Value::from_bool(false);
    if (t == "double" || t == "float") return Value::from_float(0.0);
    if (t == "string" || t == "str" || t == "char") return Value::from_string("");
    return Value::from_int(0);
}

Value mem_alloc(std::string_view elemType, std::size_t count) {
    if (count == 0) count = 1;
    const int id = g_nextPtrId++;
    RawMemBlock block;
    block.elemType = std::string(elemType);
    block.elemSize = mem_elem_size(elemType);
    block.cells = std::make_shared<std::vector<Value>>(count, mem_zero_value(elemType));
    block.index = 0;
    g_rawMem[id] = std::move(block);
    g_ptrs[id] = std::string(count * mem_elem_size(elemType), '\0');
    g_freedPtrIds.erase(id);
    g_memStats.allocs += 1;
    g_memStats.live += 1;
    return make_handle_value(HandleKind::Ptr, static_cast<uint32_t>(id));
}

void mem_free_ptr(const Value& ptr) {
    auto idOpt = ptr_id_of(ptr);
    if (!idOpt) throw std::runtime_error("free: not a pointer");
    const int id = *idOpt;
    if (g_freedPtrIds.count(id) || (!g_rawMem.count(id) && !g_ptrs.count(id))) {
        g_memStats.double_frees += 1;
        throw std::runtime_error("free: invalid or already freed pointer");
    }
    g_rawMem.erase(id);
    g_ptrs.erase(id);
    g_freedPtrIds.insert(id);
    g_memStats.frees += 1;
    if (g_memStats.live > 0) g_memStats.live -= 1;
}

Value mem_realloc(const Value& ptr, std::size_t count) {
    auto idOpt = ptr_id_of(ptr);
    if (!idOpt) throw std::runtime_error("realloc: not a pointer");
    const int id = *idOpt;
    auto* raw = raw_at(id);
    if (raw && raw->cells) {
        auto& cells = *raw->cells;
        if (count < cells.size()) cells.resize(count);
        else {
            const Value z = mem_zero_value(raw->elemType);
            while (cells.size() < count) cells.push_back(z);
        }
        g_ptrs[id] = std::string(count * raw->elemSize, '\0');
        return make_handle_value(HandleKind::Ptr, static_cast<uint32_t>(id));
    }
    auto it = g_ptrs.find(id);
    if (it == g_ptrs.end()) throw std::runtime_error("realloc: invalid pointer");
    it->second.resize(count, '\0');
    return make_handle_value(HandleKind::Ptr, static_cast<uint32_t>(id));
}

void mem_copy(const Value& dst, const Value& src, std::size_t count) {
    auto dstId = ptr_id_of(dst);
    auto srcId = ptr_id_of(src);
    if (!dstId || !srcId) throw std::runtime_error("copy: expected pointers");
    auto* d = raw_at(*dstId);
    auto* s = raw_at(*srcId);
    if (d && s && d->cells && s->cells) {
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t di = d->index + i;
            const std::size_t si = s->index + i;
            if (di >= d->cells->size() || si >= s->cells->size()) break;
            (*d->cells)[di] = (*s->cells)[si];
        }
        return;
    }
    auto dit = g_ptrs.find(*dstId);
    auto sit = g_ptrs.find(*srcId);
    if (dit == g_ptrs.end() || sit == g_ptrs.end()) throw std::runtime_error("copy: invalid pointer");
    const std::size_t n = std::min({count, dit->second.size(), sit->second.size()});
    if (n > 0) std::memcpy(dit->second.data(), sit->second.data(), n);
}

void mem_move(const Value& dst, const Value& src, std::size_t count) {
    auto dstId = ptr_id_of(dst);
    auto srcId = ptr_id_of(src);
    if (!dstId || !srcId) throw std::runtime_error("move: expected pointers");
    auto* d = raw_at(*dstId);
    auto* s = raw_at(*srcId);
    if (d && s && d->cells && s->cells) {
        std::vector<Value> tmp;
        tmp.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t si = s->index + i;
            if (si >= s->cells->size()) break;
            tmp.push_back((*s->cells)[si]);
        }
        for (std::size_t i = 0; i < tmp.size(); ++i) {
            const std::size_t di = d->index + i;
            if (di >= d->cells->size()) break;
            (*d->cells)[di] = std::move(tmp[i]);
        }
        return;
    }
    auto dit = g_ptrs.find(*dstId);
    auto sit = g_ptrs.find(*srcId);
    if (dit == g_ptrs.end() || sit == g_ptrs.end()) throw std::runtime_error("move: invalid pointer");
    const std::size_t n = std::min({count, dit->second.size(), sit->second.size()});
    if (n > 0) std::memmove(dit->second.data(), sit->second.data(), n);
}

void mem_fill(const Value& ptr, const Value& value, std::size_t count) {
    auto idOpt = ptr_id_of(ptr);
    if (!idOpt) throw std::runtime_error("fill: not a pointer");
    auto* raw = raw_at(*idOpt);
    if (raw && raw->cells) {
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t idx = raw->index + i;
            if (idx >= raw->cells->size()) break;
            (*raw->cells)[idx] = value;
        }
        return;
    }
    auto it = g_ptrs.find(*idOpt);
    if (it == g_ptrs.end()) throw std::runtime_error("fill: invalid pointer");
    const char byte = static_cast<char>(value_as_int(value) & 0xFF);
    const std::size_t n = std::min(count, it->second.size());
    std::fill_n(it->second.begin(), n, byte);
}

void mem_zero(const Value& ptr, std::size_t count) {
    auto idOpt = ptr_id_of(ptr);
    if (!idOpt) throw std::runtime_error("zero: not a pointer");
    auto* raw = raw_at(*idOpt);
    if (raw && raw->cells) {
        const Value z = mem_zero_value(raw->elemType);
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t idx = raw->index + i;
            if (idx >= raw->cells->size()) break;
            (*raw->cells)[idx] = z;
        }
        return;
    }
    auto it = g_ptrs.find(*idOpt);
    if (it == g_ptrs.end()) throw std::runtime_error("zero: invalid pointer");
    const std::size_t n = std::min(count, it->second.size());
    std::fill_n(it->second.begin(), n, '\0');
}

Value mem_deref(const Value& ptr) {
    if (value_is_handle(ptr, HandleKind::Own)) return mem_own_deref(ptr);
    if (value_is_handle(ptr, HandleKind::Shared)) return mem_shared_deref(ptr);
    auto idOpt = ptr_id_of(ptr);
    if (!idOpt) throw std::runtime_error("dereference of non-pointer");
    const int id = *idOpt;
    auto* raw = raw_at(id);
    if (raw && raw->cells) {
        if (raw->index >= raw->cells->size()) throw std::runtime_error("dereference out of bounds");
        return (*raw->cells)[raw->index];
    }
    auto it = g_ptrs.find(id);
    if (it == g_ptrs.end()) {
        if (g_freedPtrIds.count(id)) g_memStats.use_after_free += 1;
        throw std::runtime_error("dereference of freed pointer");
    }
    if (it->second.rfind("ref:", 0) == 0) return Value::from_string(it->second);
    return value_from_legacy_string(it->second);
}

void mem_ptr_set(const Value& ptr, const Value& value) {
    if (value_is_handle(ptr, HandleKind::Own)) {
        mem_own_set(ptr, value);
        return;
    }
    if (value_is_handle(ptr, HandleKind::Shared)) {
        mem_shared_set(ptr, value);
        return;
    }
    auto idOpt = ptr_id_of(ptr);
    if (!idOpt) throw std::runtime_error("pointer assignment target is not a pointer");
    const int id = *idOpt;
    auto* raw = raw_at(id);
    if (raw && raw->cells) {
        if (raw->index >= raw->cells->size()) throw std::runtime_error("pointer write out of bounds");
        (*raw->cells)[raw->index] = value;
        return;
    }
    auto it = g_ptrs.find(id);
    if (it == g_ptrs.end()) throw std::runtime_error("pointer assignment to freed pointer");
    if (it->second.rfind("ref:", 0) == 0) {
        throw std::runtime_error("ref pointer set requires env");
    }
    it->second = to_display_string(value);
}

Value mem_ptr_add(const Value& ptr, std::int64_t delta) {
    auto idOpt = ptr_id_of(ptr);
    if (!idOpt) throw std::runtime_error("pointer arithmetic on non-pointer");
    const int id = *idOpt;
    auto* raw = raw_at(id);
    if (!raw || !raw->cells) throw std::runtime_error("pointer arithmetic requires typed alloc");
    const int newId = g_nextPtrId++;
    RawMemBlock view = *raw;
    const std::int64_t next = static_cast<std::int64_t>(raw->index) + delta;
    if (next < 0) throw std::runtime_error("pointer arithmetic underflow");
    view.index = static_cast<std::size_t>(next);
    g_rawMem[newId] = std::move(view);
    g_ptrs[newId] = g_ptrs.count(id) ? g_ptrs[id] : std::string{};
    return make_handle_value(HandleKind::Ptr, static_cast<uint32_t>(newId));
}

bool mem_is_ptr(const Value& v) {
    if (value_is_handle(v, HandleKind::Ptr)) return true;
    return parse_pointer_handle(to_display_string(v)).has_value();
}

bool mem_is_owner(const Value& v) {
    return value_is_handle(v, HandleKind::Own) ||
           value_is_handle(v, HandleKind::Shared) ||
           value_is_handle(v, HandleKind::Buffer);
}

Value mem_make_own(std::string_view elemType, Value payload) {
    const int id = g_nextOwnId++;
    OwnState st;
    st.elemType = std::string(elemType);
    st.payload = std::move(payload);
    st.alive = true;
    g_owns[id] = std::move(st);
    g_memStats.allocs += 1;
    g_memStats.live += 1;
    return make_handle_value(HandleKind::Own, static_cast<uint32_t>(id));
}

Value mem_make_shared(std::string_view elemType, Value payload) {
    const int id = g_nextSharedId++;
    SharedState st;
    st.elemType = std::string(elemType);
    st.payload = std::move(payload);
    st.strong = 1;
    st.weak = 0;
    g_shareds[id] = std::move(st);
    g_memStats.allocs += 1;
    g_memStats.live += 1;
    return make_handle_value(HandleKind::Shared, static_cast<uint32_t>(id));
}

Value mem_make_weak(const Value& shared) {
    if (!value_is_handle(shared, HandleKind::Shared)) {
        throw std::runtime_error("weak() expects shared<T>");
    }
    const int sid = static_cast<int>(value_handle_id(shared));
    auto it = g_shareds.find(sid);
    if (it == g_shareds.end()) throw std::runtime_error("weak() on expired shared");
    it->second.weak += 1;
    const int id = g_nextWeakId++;
    g_weaks[id] = WeakState{sid};
    return make_handle_value(HandleKind::Weak, static_cast<uint32_t>(id));
}

Value mem_weak_get(const Value& weak) {
    if (!value_is_handle(weak, HandleKind::Weak)) {
        throw std::runtime_error("weak.get() on non-weak");
    }
    const int wid = static_cast<int>(value_handle_id(weak));
    auto wit = g_weaks.find(wid);
    if (wit == g_weaks.end()) {
        return Value::from_string("None");
    }
    auto sit = g_shareds.find(wit->second.sharedId);
    if (sit == g_shareds.end() || sit->second.strong <= 0) {
        return Value::from_string("None");
    }
    std::vector<std::string> payloads;
    payloads.push_back(to_display_string(sit->second.payload));
    return Value::from_string(encode_enum_variant("Some", payloads));
}

Value mem_make_buffer(std::string_view elemType, std::size_t count) {
    const int id = g_nextBufferId++;
    BufferState st;
    st.elemType = std::string(elemType);
    st.capacity = count;
    st.data.assign(count, mem_zero_value(elemType));
    g_buffers[id] = std::move(st);
    g_memStats.allocs += 1;
    g_memStats.live += 1;
    return make_handle_value(HandleKind::Buffer, static_cast<uint32_t>(id));
}

Value mem_own_deref(const Value& own) {
    if (!value_is_handle(own, HandleKind::Own)) throw std::runtime_error("heap deref on non-heap");
    const int id = static_cast<int>(value_handle_id(own));
    auto it = g_owns.find(id);
    if (it == g_owns.end() || !it->second.alive) {
        throw std::runtime_error("use after move of heap<T>");
    }
    return it->second.payload;
}

void mem_own_set(const Value& own, const Value& value) {
    if (!value_is_handle(own, HandleKind::Own)) throw std::runtime_error("heap set on non-heap");
    const int id = static_cast<int>(value_handle_id(own));
    auto it = g_owns.find(id);
    if (it == g_owns.end() || !it->second.alive) {
        throw std::runtime_error("use after move of heap<T>");
    }
    it->second.payload = value;
}

Value mem_shared_deref(const Value& shared) {
    if (!value_is_handle(shared, HandleKind::Shared)) throw std::runtime_error("shared deref on non-shared");
    const int id = static_cast<int>(value_handle_id(shared));
    auto it = g_shareds.find(id);
    if (it == g_shareds.end() || it->second.strong <= 0) {
        throw std::runtime_error("dereference of expired shared");
    }
    return it->second.payload;
}

void mem_shared_set(const Value& shared, const Value& value) {
    if (!value_is_handle(shared, HandleKind::Shared)) throw std::runtime_error("shared set on non-shared");
    const int id = static_cast<int>(value_handle_id(shared));
    auto it = g_shareds.find(id);
    if (it == g_shareds.end() || it->second.strong <= 0) {
        throw std::runtime_error("write through expired shared");
    }
    it->second.payload = value;
}

void mem_retain(const Value& v) {
    if (value_is_handle(v, HandleKind::Shared)) {
        const int id = static_cast<int>(value_handle_id(v));
        auto it = g_shareds.find(id);
        if (it != g_shareds.end()) it->second.strong += 1;
    }
}

void mem_release(const Value& v) {
    if (value_is_handle(v, HandleKind::Own)) {
        const int id = static_cast<int>(value_handle_id(v));
        auto it = g_owns.find(id);
        if (it == g_owns.end()) return;
        if (it->second.alive) {
            it->second.alive = false;
            it->second.payload = Value::null_value();
            g_memStats.frees += 1;
            if (g_memStats.live > 0) g_memStats.live -= 1;
        }
        g_owns.erase(it);
        return;
    }
    if (value_is_handle(v, HandleKind::Shared)) {
        const int id = static_cast<int>(value_handle_id(v));
        auto it = g_shareds.find(id);
        if (it == g_shareds.end()) return;
        if (it->second.strong > 0) it->second.strong -= 1;
        if (it->second.strong == 0) {
            g_memStats.frees += 1;
            if (g_memStats.live > 0) g_memStats.live -= 1;
        }
        destroy_shared_if_unused(id);
        return;
    }
    if (value_is_handle(v, HandleKind::Weak)) {
        const int id = static_cast<int>(value_handle_id(v));
        auto wit = g_weaks.find(id);
        if (wit == g_weaks.end()) return;
        const int sid = wit->second.sharedId;
        g_weaks.erase(wit);
        auto sit = g_shareds.find(sid);
        if (sit != g_shareds.end()) {
            if (sit->second.weak > 0) sit->second.weak -= 1;
            destroy_shared_if_unused(sid);
        }
        return;
    }
    if (value_is_handle(v, HandleKind::Buffer)) {
        const int id = static_cast<int>(value_handle_id(v));
        if (g_buffers.erase(id) > 0) {
            g_memStats.frees += 1;
            if (g_memStats.live > 0) g_memStats.live -= 1;
        }
    }
}

std::uint64_t mem_live_count() {
    return static_cast<std::uint64_t>(g_rawMem.size() + g_owns.size() + g_buffers.size()) +
           static_cast<std::uint64_t>(std::count_if(g_shareds.begin(), g_shareds.end(),
               [](const auto& kv) { return kv.second.strong > 0; }));
}

Value mem_stats_value() {
    std::ostringstream oss;
    oss << "allocs=" << g_memStats.allocs
        << " frees=" << g_memStats.frees
        << " live=" << mem_live_count()
        << " double_frees=" << g_memStats.double_frees
        << " use_after_free=" << g_memStats.use_after_free
        << " leaks_at_reset=" << g_memStats.leaks_at_reset;
    return Value::from_string(oss.str());
}

void mem_reset_debug_stats() {
    g_memStats = MemDebugStats{};
    g_freedPtrIds.clear();
}

Value mem_take_own(Value& source) {
    if (!value_is_handle(source, HandleKind::Own)) return source;
    Value out = source;
    source = Value::null_value();
    return out;
}

bool mem_try_move_own_ident(ValueMap& vars, const std::string& name, Value& out) {
    auto it = vars.find(name);
    if (it == vars.end()) return false;
    if (!value_is_handle(it->second, HandleKind::Own)) return false;
    out = it->second;
    it->second = Value::null_value();
    return true;
}

Value mem_buffer_index_get(const Value& buf, std::int64_t index) {
    if (!value_is_handle(buf, HandleKind::Buffer)) throw std::runtime_error("index on non-buffer");
    auto it = g_buffers.find(static_cast<int>(value_handle_id(buf)));
    if (it == g_buffers.end()) throw std::runtime_error("invalid buffer");
    if (index < 0 || static_cast<std::size_t>(index) >= it->second.data.size()) {
        throw std::runtime_error("buffer index out of bounds");
    }
    return it->second.data[static_cast<std::size_t>(index)];
}

void mem_buffer_index_set(const Value& buf, std::int64_t index, const Value& value) {
    if (!value_is_handle(buf, HandleKind::Buffer)) throw std::runtime_error("index assign on non-buffer");
    auto it = g_buffers.find(static_cast<int>(value_handle_id(buf)));
    if (it == g_buffers.end()) throw std::runtime_error("invalid buffer");
    if (index < 0 || static_cast<std::size_t>(index) >= it->second.data.size()) {
        throw std::runtime_error("buffer index out of bounds");
    }
    it->second.data[static_cast<std::size_t>(index)] = value;
}

Value mem_buffer_method(const Value& buf, std::string_view method, const std::vector<Value>& args) {
    if (!value_is_handle(buf, HandleKind::Buffer)) throw std::runtime_error("buffer method on non-buffer");
    auto it = g_buffers.find(static_cast<int>(value_handle_id(buf)));
    if (it == g_buffers.end()) throw std::runtime_error("invalid buffer");
    BufferState& st = it->second;
    if (method == "len") return Value::from_int(static_cast<int64_t>(st.data.size()));
    if (method == "cap") return Value::from_int(static_cast<int64_t>(st.capacity));
    if (method == "ptr") {
        const int id = g_nextPtrId++;
        RawMemBlock block;
        block.elemType = st.elemType;
        block.elemSize = mem_elem_size(st.elemType);
        block.cells = std::make_shared<std::vector<Value>>(st.data);
        block.index = 0;
        // Keep buffer and ptr view independent copy for safety; sync not required for tests.
        g_rawMem[id] = std::move(block);
        g_ptrs[id] = std::string(st.data.size() * mem_elem_size(st.elemType), '\0');
        return make_handle_value(HandleKind::Ptr, static_cast<uint32_t>(id));
    }
    if (method == "clear") {
        st.data.clear();
        return Value::null_value();
    }
    if (method == "reserve") {
        if (args.empty()) throw std::runtime_error("buffer.reserve(n)");
        const std::size_t n = static_cast<std::size_t>(std::max<int64_t>(0, value_as_int(args[0])));
        if (n > st.capacity) st.capacity = n;
        st.data.reserve(n);
        return Value::null_value();
    }
    if (method == "resize") {
        if (args.empty()) throw std::runtime_error("buffer.resize(n)");
        const std::size_t n = static_cast<std::size_t>(std::max<int64_t>(0, value_as_int(args[0])));
        if (n > st.capacity) st.capacity = n;
        const Value z = mem_zero_value(st.elemType);
        st.data.resize(n, z);
        return Value::null_value();
    }
    if (method == "push") {
        if (args.empty()) throw std::runtime_error("buffer.push(value)");
        if (st.data.size() >= st.capacity) {
            st.capacity = st.capacity == 0 ? 1 : st.capacity * 2;
        }
        st.data.push_back(args[0]);
        return Value::null_value();
    }
    if (method == "pop") {
        if (st.data.empty()) throw std::runtime_error("buffer.pop on empty");
        Value out = st.data.back();
        st.data.pop_back();
        return out;
    }
    throw std::runtime_error("unknown buffer method: " + std::string(method));
}

} // namespace erelang
