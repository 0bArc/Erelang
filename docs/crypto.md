# Crypto helpers

```elan
#include <builtin/crypto> as crypto
```

| Alias | Builtin | Args | Returns |
|-------|---------|------|---------|
| `crypto.hash(s)` | `hash_fnv1a` | string | 16-char hex (FNV-1a 64-bit) |
| `crypto.sha256(s)` | `hash_sha256` | string | 64-char hex (SHA-256) |
| `crypto.random_bytes(n)` | `random_bytes` | byte count | hex string (xorshift PRNG, not CSPRNG) |
| `crypto.aes_encrypt(key_hex, plaintext)` | `aes_encrypt` | 64-hex key, plaintext | hex ciphertext |
| `crypto.aes_decrypt(key_hex, ciphertext)` | `aes_decrypt` | 64-hex key, hex ciphertext | plaintext string |

## AES-256-GCM

- Algorithm: **AES-256-GCM** via Windows BCrypt (`bcrypt.h`).
- Key: exactly **32 bytes** as **64 hex characters**. Wrong length throws a runtime error.
- Ciphertext encoding: hex of `nonce (12 bytes) || ciphertext || tag (16 bytes)`.
- Nonce is random per encrypt (`BCryptGenRandom`).

```elan
@strict
#include <builtin/crypto> as crypto

public action main() {
    string key = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
    string ct = crypto.aes_encrypt(key, "hello");
    string pt = crypto.aes_decrypt(key, ct);
    print pt;
}

run main;
```

MD5 / HMAC / base64 are **not** implemented under this module.

## Example

```elan
@erelang
#include <builtin/crypto> as crypto

public action main {
    print crypto.hash("hello");
    print crypto.sha256("hello");
    print crypto.random_bytes(16);
}

run main;
```

## Related

- [binary.md](binary.md)
- [core-builtins.md](core-builtins.md)
