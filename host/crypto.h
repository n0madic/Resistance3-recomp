// Small, dependency-free crypto for the executable decryption (unself.cpp): AES-128/256, SHA-1, HMAC-SHA1, AES-CMAC, inflate.
//
// SPDX-License-Identifier: GPL-2.0-or-later
// Taken from the Soulcalibur II HD port of ps3recomp (same author).
//
// Size and independence matter more than speed: this replaces a
// crypto library the host would otherwise have to carry. The code is checked against FIPS-197 / RFC 3174 / RFC 4493
// vectors (crypto_selftest, run by `r3_host selftest`) and against the Python tools it replaces.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace r3 {

class Aes {
public:
    // key_bytes: 16 or 32.
    void set_key(const uint8_t* key, size_t key_bytes);
    void encrypt_block(const uint8_t in[16], uint8_t out[16]) const;
    void decrypt_block(const uint8_t in[16], uint8_t out[16]) const;

private:
    uint32_t enc_[60] = {};
    uint32_t dec_[60] = {};
    int rounds_ = 0;
};

// AES-CBC decryption of `len` bytes (multiple of 16); in and out may be the same buffer.
void aes_cbc_decrypt(const uint8_t* key, size_t key_bytes, const uint8_t iv[16], const uint8_t* in, uint8_t* out,
                     size_t len);
// AES-CTR with a 128-bit big-endian counter that starts at `iv`; in and out may be the same buffer.
void aes_ctr(const uint8_t* key, size_t key_bytes, const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t len);
void aes_ecb_encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
void aes_ecb_decrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

void sha1(const uint8_t* data, size_t len, uint8_t out[20]);
void hmac_sha1(const uint8_t* key, size_t key_len, const uint8_t* data, size_t len, uint8_t out[20]);
void aes_cmac(const uint8_t key[16], const uint8_t* data, size_t len, uint8_t out[16]);

// Big-endian words and hex strings, shared with unself.cpp.
inline uint32_t load_be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
inline void store_be32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
// "0a1B..." -> bytes (pairs of hex digits; the input is a constant of the program, not validated).
std::vector<uint8_t> hex(const char* s);

// zlib stream (RFC 1950) -> bytes. Throws std::runtime_error on corrupt input (including a wrong Adler-32) and when the
// output would pass `max_out` bytes (a small stream can inflate to gigabytes).
std::vector<uint8_t> zlib_inflate(const uint8_t* data, size_t len, size_t max_out = SIZE_MAX);

// Self-check against published test vectors; returns the name of the first failing one, or nullptr.
const char* crypto_selftest();

}  // namespace r3
