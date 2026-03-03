// Basic crypto/hash helpers for erelang
#include "erelang/runtime.hpp"
#include <string>
#include <vector>
#include <array>
#include <cstdint>
#include <sstream>
#include <iomanip>

namespace erelang {

static std::string to_hex(const std::vector<uint8_t>& data) {
    std::ostringstream ss; ss<<std::hex<<std::setfill('0');
    for (uint8_t b: data) ss<<std::setw(2)<<(int)b;
    return ss.str();
}

// Simple FNV-1a 64-bit hash (fast, not cryptographically secure)
static std::string fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return std::to_string((unsigned long long)h);
}

// Simple XORShift32 PRNG state (per process) for random_bytes fallback
static uint32_t g_xor_state = 0x12345678u;
static uint32_t xorshift32() {
    uint32_t x = g_xor_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5; g_xor_state = x; return x;
}

static std::string crypto_dispatch(const std::string& name, const std::vector<std::string>& argv) {
    auto argS = [&](size_t i){ return i<argv.size()?argv[i]:std::string(); };
    if (name == "hash_fnv1a") return fnv1a(argS(0));
    if (name == "random_bytes") {
        int n = 0; try { n = std::stoi(argS(0)); } catch (...) { n = 0; }
        if (n < 0) n = 0; if (n > 4096) n = 4096;
        std::vector<uint8_t> buf; buf.reserve(n);
        for (int i=0;i<n;++i) {
            if ((i & 3) == 0) (void)xorshift32();
            buf.push_back(uint8_t((g_xor_state >> ((i & 3)*8)) & 0xFF));
        }
        return to_hex(buf);
    }
    // TODO: add SHA256 / HMAC in later iteration (needs small implementation or library)
    return {};
}

std::string __erelang_builtin_crypto_dispatch(const std::string& name, const std::vector<std::string>& argv) {
    return crypto_dispatch(name, argv);
}

} // namespace erelang
