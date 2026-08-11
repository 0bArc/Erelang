// Binary buffer helpers
#include "erelang/runtime.hpp"
#include <vector>
#include <string>
#include <cstdint>
#include <sstream>
#include <iomanip>

namespace erelang {
struct BinBuf { std::vector<uint8_t> data; };
static std::unordered_map<int,BinBuf> g_binbufs; static int g_nextBin=1;

static std::string to_hex_buf(const std::vector<uint8_t>& d){ std::ostringstream ss; ss<<std::hex<<std::setfill('0'); for(uint8_t b: d) ss<<std::setw(2)<<(int)b; return ss.str(); }

// Parse a "bin:<id>" handle; returns -1 on malformed handles.
static int parse_bin_handle(const std::string& h) {
    if (h.rfind("bin:", 0) != 0) return -1;
    try {
        auto tail = h.substr(4);
        if (tail.empty()) return -1;
        std::size_t pos = 0;
        int v = std::stoi(tail, &pos);
        if (pos != tail.size()) return -1;
        return v;
    } catch (...) {
        return -1;
    }
}

static BinBuf* find_binbuf(int id) {
    auto it = g_binbufs.find(id);
    return it == g_binbufs.end() ? nullptr : &it->second;
}

static int hex_digit_value(char ch) noexcept {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static std::string binary_dispatch(const std::string& name, const std::vector<std::string>& argv){
    auto argS = [&](size_t i){ return i<argv.size()?argv[i]:std::string(); };
    if (name == "bin_new") { int id=g_nextBin++; g_binbufs[id]= {}; return std::string("bin:")+std::to_string(id); }
    if (name == "bin_from_hex") {
        // Reject non-hex digits and odd-length input instead of silently
        // producing zero bytes from garbage.
        std::string h = argS(0);
        if (h.size() % 2 != 0) return {};
        for (char ch : h) {
            if (hex_digit_value(ch) < 0) return {};
        }
        int id=g_nextBin++; g_binbufs[id]={};
        auto& data = g_binbufs[id].data;
        for (size_t i=0;i<h.size();i+=2){
            data.push_back(static_cast<uint8_t>((hex_digit_value(h[i]) << 4) | hex_digit_value(h[i+1])));
        }
        return std::string("bin:")+std::to_string(id);
    }
    if (name == "bin_len") { int id=parse_bin_handle(argS(0)); if(id<0) return "0"; auto* buf=find_binbuf(id); return buf ? std::to_string(buf->data.size()) : "0"; }
    if (name == "bin_hex") { int id=parse_bin_handle(argS(0)); if(id<0) return {}; auto* buf=find_binbuf(id); return buf ? to_hex_buf(buf->data) : std::string(); }
    if (name == "bin_push_u8") {
        auto h=argS(0); int v=0; try{ v=std::stoi(argS(1)); }catch(...){}
        int id=parse_bin_handle(h); if(id<0) return {};
        auto* buf=find_binbuf(id); if (buf) buf->data.push_back((uint8_t)(v & 0xFF));
        return {};
    }
    if (name == "bin_get_u8") {
        auto h=argS(0); int idx=0; try{ idx=std::stoi(argS(1)); }catch(...){}
        int id=parse_bin_handle(h); if(id<0) return {};
        auto* buf=find_binbuf(id);
        if (buf && idx>=0 && idx<(int)buf->data.size()) return std::to_string((int)buf->data[idx]);
        return {};
    }
    return {};
}

std::string __erelang_builtin_binary_dispatch(const std::string& name, const std::vector<std::string>& argv){ return binary_dispatch(name, argv);}    

} // namespace erelang
