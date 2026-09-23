// Basic crypto/hash helpers for erelang
#include "erelang/runtime.hpp"
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <cwchar>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

namespace erelang {

#ifdef _WIN32
static constexpr size_t kAesGcmNonceBytes = 12;
static constexpr size_t kAesGcmTagBytes = 16;
#endif

static std::string to_hex(const std::vector<uint8_t>& data) {
    std::ostringstream ss; ss<<std::hex<<std::setfill('0');
    for (uint8_t b: data) ss<<std::setw(2)<<(int)b;
    return ss.str();
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static std::vector<uint8_t> from_hex(std::string_view hex) {
    if (hex.size() % 2 != 0) {
        throw std::runtime_error("crypto: invalid hex length");
    }
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = hex_nibble(hex[i]);
        const int lo = hex_nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            throw std::runtime_error("crypto: invalid hex");
        }
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

static std::vector<uint8_t> parse_aes_key_hex(std::string_view key_hex) {
    if (key_hex.size() != 64) {
        throw std::runtime_error("aes: key must be 32 bytes (64 hex chars)");
    }
    auto key = from_hex(key_hex);
    if (key.size() != 32) {
        throw std::runtime_error("aes: key must be 32 bytes (64 hex chars)");
    }
    return key;
}

#ifdef _WIN32
static void bcrypt_check(NTSTATUS status, const char* what) {
    if (!BCRYPT_SUCCESS(status)) {
        throw std::runtime_error(what);
    }
}

static std::string aes256_gcm_encrypt(const std::vector<uint8_t>& key, const std::string& plaintext) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    auto cleanup = [&]() {
        if (hKey) BCryptDestroyKey(hKey);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    };
    try {
        bcrypt_check(BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0),
                     "aes_encrypt: open AES provider failed");
        bcrypt_check(
            BCryptSetProperty(alg, BCRYPT_CHAINING_MODE,
                              reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                              static_cast<ULONG>((wcslen(BCRYPT_CHAIN_MODE_GCM) + 1) * sizeof(wchar_t)), 0),
            "aes_encrypt: set GCM mode failed");
        bcrypt_check(BCryptGenerateSymmetricKey(alg, &hKey, nullptr, 0,
                                                reinterpret_cast<PUCHAR>(const_cast<uint8_t*>(key.data())),
                                                static_cast<ULONG>(key.size()), 0),
                     "aes_encrypt: generate key failed");

        std::vector<uint8_t> nonce(kAesGcmNonceBytes);
        bcrypt_check(BCryptGenRandom(nullptr, nonce.data(), static_cast<ULONG>(nonce.size()),
                                     BCRYPT_USE_SYSTEM_PREFERRED_RNG),
                     "aes_encrypt: generate nonce failed");

        std::vector<uint8_t> tag(kAesGcmTagBytes);
        std::vector<uint8_t> cipher(plaintext.size());
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
        BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
        authInfo.pbNonce = nonce.data();
        authInfo.cbNonce = static_cast<ULONG>(nonce.size());
        authInfo.pbTag = tag.data();
        authInfo.cbTag = static_cast<ULONG>(tag.size());

        ULONG cbResult = 0;
        bcrypt_check(BCryptEncrypt(hKey,
                                   reinterpret_cast<PUCHAR>(const_cast<char*>(plaintext.data())),
                                   static_cast<ULONG>(plaintext.size()), &authInfo, nullptr, 0,
                                   cipher.empty() ? nullptr : cipher.data(),
                                   static_cast<ULONG>(cipher.size()), &cbResult, 0),
                     "aes_encrypt: encrypt failed");
        cipher.resize(cbResult);
        cleanup();

        std::vector<uint8_t> packed;
        packed.reserve(nonce.size() + cipher.size() + tag.size());
        packed.insert(packed.end(), nonce.begin(), nonce.end());
        packed.insert(packed.end(), cipher.begin(), cipher.end());
        packed.insert(packed.end(), tag.begin(), tag.end());
        return to_hex(packed);
    } catch (...) {
        cleanup();
        throw;
    }
}

static std::string aes256_gcm_decrypt(const std::vector<uint8_t>& key, std::string_view ciphertext_hex) {
    auto packed = from_hex(ciphertext_hex);
    if (packed.size() < kAesGcmNonceBytes + kAesGcmTagBytes) {
        throw std::runtime_error("aes_decrypt: ciphertext too short");
    }
    const size_t cipherLen = packed.size() - kAesGcmNonceBytes - kAesGcmTagBytes;
    const uint8_t* nonce = packed.data();
    const uint8_t* cipher = packed.data() + kAesGcmNonceBytes;
    const uint8_t* tag = packed.data() + kAesGcmNonceBytes + cipherLen;

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    auto cleanup = [&]() {
        if (hKey) BCryptDestroyKey(hKey);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    };
    try {
        bcrypt_check(BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0),
                     "aes_decrypt: open AES provider failed");
        bcrypt_check(
            BCryptSetProperty(alg, BCRYPT_CHAINING_MODE,
                              reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                              static_cast<ULONG>((wcslen(BCRYPT_CHAIN_MODE_GCM) + 1) * sizeof(wchar_t)), 0),
            "aes_decrypt: set GCM mode failed");
        bcrypt_check(BCryptGenerateSymmetricKey(alg, &hKey, nullptr, 0,
                                                reinterpret_cast<PUCHAR>(const_cast<uint8_t*>(key.data())),
                                                static_cast<ULONG>(key.size()), 0),
                     "aes_decrypt: generate key failed");

        std::vector<uint8_t> plain(cipherLen);
        std::vector<uint8_t> tagBuf(tag, tag + kAesGcmTagBytes);
        std::vector<uint8_t> nonceBuf(nonce, nonce + kAesGcmNonceBytes);
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
        BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
        authInfo.pbNonce = nonceBuf.data();
        authInfo.cbNonce = static_cast<ULONG>(nonceBuf.size());
        authInfo.pbTag = tagBuf.data();
        authInfo.cbTag = static_cast<ULONG>(tagBuf.size());

        ULONG cbResult = 0;
        const NTSTATUS st = BCryptDecrypt(
            hKey, reinterpret_cast<PUCHAR>(const_cast<uint8_t*>(cipher)), static_cast<ULONG>(cipherLen),
            &authInfo, nullptr, 0, plain.empty() ? nullptr : plain.data(), static_cast<ULONG>(plain.size()),
            &cbResult, 0);
        cleanup();
        if (!BCRYPT_SUCCESS(st)) {
            throw std::runtime_error("aes_decrypt: decrypt failed");
        }
        plain.resize(cbResult);
        return std::string(reinterpret_cast<const char*>(plain.data()), plain.size());
    } catch (...) {
        cleanup();
        throw;
    }
}
#endif

static std::string fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    std::ostringstream ss; ss << std::hex << std::setfill('0') << std::setw(16) << h;
    return ss.str();
}

static std::atomic<uint32_t> g_xor_state{0x12345678u};
static uint32_t xorshift32() {
    uint32_t x = g_xor_state.load(std::memory_order_relaxed);
    x ^= x << 13; x ^= x >> 17; x ^= x << 5; g_xor_state.store(x, std::memory_order_relaxed); return x;
}

static uint32_t rotr32(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

static std::string sha256(const std::string& message) {
    static const uint32_t k[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
    };

    std::vector<uint8_t> data(message.begin(), message.end());
    const uint64_t bitLen = static_cast<uint64_t>(data.size()) * 8ull;
    data.push_back(0x80);
    while ((data.size() % 64) != 56) data.push_back(0);
    for (int i = 7; i >= 0; --i) data.push_back(static_cast<uint8_t>((bitLen >> (i * 8)) & 0xff));

    uint32_t h0 = 0x6a09e667u, h1 = 0xbb67ae85u, h2 = 0x3c6ef372u, h3 = 0xa54ff53au;
    uint32_t h4 = 0x510e527fu, h5 = 0x9b05688cu, h6 = 0x1f83d9abu, h7 = 0x5be0cd19u;

    for (size_t offset = 0; offset < data.size(); offset += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(data[offset + i * 4]) << 24) |
                   (static_cast<uint32_t>(data[offset + i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(data[offset + i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(data[offset + i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, h = h7;
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
            const uint32_t ch = (e & f) ^ ((~e) & g);
            const uint32_t temp1 = h + S1 + ch + k[i] + w[i];
            const uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = S0 + maj;
            h = g; g = f; f = e; e = d + temp1; d = c; c = b; b = a; a = temp1 + temp2;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e; h5 += f; h6 += g; h7 += h;
    }

    std::vector<uint8_t> digest(32);
    auto put = [&](int idx, uint32_t v) {
        digest[idx] = static_cast<uint8_t>((v >> 24) & 0xff);
        digest[idx + 1] = static_cast<uint8_t>((v >> 16) & 0xff);
        digest[idx + 2] = static_cast<uint8_t>((v >> 8) & 0xff);
        digest[idx + 3] = static_cast<uint8_t>(v & 0xff);
    };
    put(0, h0); put(4, h1); put(8, h2); put(12, h3);
    put(16, h4); put(20, h5); put(24, h6); put(28, h7);
    return to_hex(digest);
}

static std::string crypto_dispatch(const std::string& name, const std::vector<std::string>& argv) {
    auto argS = [&](size_t i){ return i<argv.size()?argv[i]:std::string(); };
    if (name == "hash_fnv1a") return fnv1a(argS(0));
    if (name == "hash_sha256" || name == "sha256") return sha256(argS(0));
    if (name == "random_bytes") {
        int n = 0; try { n = std::stoi(argS(0)); } catch (...) { n = 0; }
        if (n < 0) n = 0; if (n > 4096) n = 4096;
        std::vector<uint8_t> buf; buf.reserve(n);
        for (int i=0;i<n;++i) {
            if ((i & 3) == 0) (void)xorshift32();
            const uint32_t s = g_xor_state.load(std::memory_order_relaxed);
            buf.push_back(uint8_t((s >> ((i & 3)*8)) & 0xFF));
        }
        return to_hex(buf);
    }
    if (name == "aes_encrypt") {
#ifdef _WIN32
        const auto key = parse_aes_key_hex(argS(0));
        return aes256_gcm_encrypt(key, argS(1));
#else
        (void)argv;
        throw std::runtime_error("aes_encrypt: not supported on this platform");
#endif
    }
    if (name == "aes_decrypt") {
#ifdef _WIN32
        const auto key = parse_aes_key_hex(argS(0));
        return aes256_gcm_decrypt(key, argS(1));
#else
        (void)argv;
        throw std::runtime_error("aes_decrypt: not supported on this platform");
#endif
    }
    return {};
}

std::string __erelang_builtin_crypto_dispatch(const std::string& name, const std::vector<std::string>& argv) {
    return crypto_dispatch(name, argv);
}

} // namespace erelang
