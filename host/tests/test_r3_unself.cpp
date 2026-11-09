// SPDX-License-Identifier: GPL-2.0-or-later
//
// Test of unself.cpp: a synthetic APP SELF is built with a test key (the metadata encrypted the way a SELF carries it),
// decrypted, and compared with the ELF it was made from; then the refusals.
//
//   cmake --build build/host --target test_r3_unself && build/host/test_r3_unself
#include "../crypto.h"
#include "../unself.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Bytes = std::vector<uint8_t>;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            g_fail++;                                                            \
        }                                                                        \
    } while (0)

static void put16(Bytes& b, size_t off, uint16_t v) { b[off] = v >> 8; b[off + 1] = (uint8_t)v; }
static void put32(Bytes& b, size_t off, uint32_t v) { r3::store_be32(&b[off], v); }
static void put64(Bytes& b, size_t off, uint64_t v) { put32(b, off, (uint32_t)(v >> 32)); put32(b, off + 4, (uint32_t)v); }

static const char TEST_ERK[] = "00112233445566778899AABBCCDDEEFF0123456789ABCDEFFEDCBA9876543210";
static const char TEST_RIV[] = "A0A1A2A3A4A5A6A7A8A9AAABACADAEAF";
static const uint16_t TEST_REV = 0x0042;
static const r3::AppKey TEST_KEYS[] = {{TEST_REV, TEST_ERK, TEST_RIV}};

static void cbc_encrypt(const uint8_t* key, size_t key_bytes, const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t len)
{
    r3::Aes aes;
    aes.set_key(key, key_bytes);
    uint8_t prev[16];
    memcpy(prev, iv, 16);
    for (size_t o = 0; o < len; o += 16) {
        uint8_t block[16];
        for (int i = 0; i < 16; i++) block[i] = in[o + i] ^ prev[i];
        aes.encrypt_block(block, out + o);
        memcpy(prev, out + o, 16);
    }
}

static uint32_t adler32(const Bytes& d)
{
    uint32_t a = 1, b = 0;
    for (uint8_t c : d) { a = (a + c) % 65521; b = (b + a) % 65521; }
    return b << 16 | a;
}

// A zlib stream of one stored block.
static Bytes zlib_stored(const Bytes& d)
{
    Bytes z = {0x78, 0x01, 0x01, (uint8_t)(d.size() & 0xFF), (uint8_t)(d.size() >> 8), (uint8_t)(~d.size() & 0xFF),
               (uint8_t)((~d.size() >> 8) & 0xFF)};
    z.insert(z.end(), d.begin(), d.end());
    uint32_t ad = adler32(d);
    for (int s = 24; s >= 0; s -= 8) z.push_back((uint8_t)(ad >> s));
    return z;
}

struct Made {
    Bytes self, elf;
};

// A one-segment ELF whose segment (the file at 0x100) is `payload`, wrapped in a SELF.
static Made make_self(bool compress, uint32_t type = 4, uint16_t flags = TEST_REV)
{
    Made m;
    Bytes payload(300);
    for (size_t i = 0; i < payload.size(); i++) payload[i] = (uint8_t)(i * 7 + 3);

    const size_t EHDR = 0x70, PHDR = 0xB0, PROG_ID = 0x100, META = 0x120, INFO = META + 0x20, HDRS = INFO + 0x40;
    const size_t headers_len = 0x20 + 0x30 + 2 * 16, HDR_SIZE = HDRS + headers_len, DATA = HDR_SIZE;
    Bytes seg = compress ? zlib_stored(payload) : payload;

    // Section keys: key 0 is the AES key, key 1 the counter start.
    uint8_t data_key[16], data_iv[16], meta_key[16], meta_iv[16];
    for (int i = 0; i < 16; i++) {
        data_key[i] = (uint8_t)(0x10 + i); data_iv[i] = (uint8_t)(0xF0 - i);
        meta_key[i] = (uint8_t)(0x30 + 3 * i); meta_iv[i] = (uint8_t)(0x77 ^ i);
    }
    data_iv[15] = 0xFF;   // the counter carries into the next byte within the first blocks

    Bytes self(DATA + seg.size(), 0);
    memcpy(&self[0], "SCE\0", 4);
    put16(self, 8, flags);
    put32(self, 12, (uint32_t)META);
    put64(self, 16, HDR_SIZE);
    put64(self, 0x20 + 8, PROG_ID);
    put64(self, 0x20 + 16, EHDR);
    put64(self, 0x20 + 24, PHDR);
    put64(self, 0x20 + 32, 0);   // no section headers
    put32(self, PROG_ID + 12, type);

    self[EHDR + 0] = 0x7F; self[EHDR + 1] = 'E'; self[EHDR + 2] = 'L'; self[EHDR + 3] = 'F'; self[EHDR + 4] = 2;
    put64(self, EHDR + 0x20, 0x40);   // e_phoff
    put16(self, EHDR + 0x38, 1);      // e_phnum
    put64(self, PHDR + 8, 0x100);     // p_offset of segment 0

    // Metadata headers: one section (the segment), two keys; encrypted with meta_key/meta_iv.
    Bytes headers(headers_len, 0);
    put32(headers, 12, 1);   // section count
    put32(headers, 16, 2);   // key count
    uint8_t* s = &headers[0x20];
    memset(s, 0, 0x30);
    r3::store_be32(s + 0, 0); r3::store_be32(s + 4, (uint32_t)DATA);   // data_off (64-bit)
    r3::store_be32(s + 8, 0); r3::store_be32(s + 12, (uint32_t)seg.size());
    r3::store_be32(s + 16, 2);                    // type: PHDR
    r3::store_be32(s + 20, 0);                    // program header index
    r3::store_be32(s + 32, 3);                    // encrypted
    r3::store_be32(s + 36, 0);                    // key index
    r3::store_be32(s + 40, 1);                    // iv index
    r3::store_be32(s + 44, compress ? 2 : 1);     // compressed
    memcpy(&headers[0x20 + 0x30], data_key, 16);
    memcpy(&headers[0x20 + 0x30 + 16], data_iv, 16);
    r3::aes_ctr(meta_key, 16, meta_iv, headers.data(), &self[HDRS], headers.size());

    // Metadata info: meta_key, zero padding, meta_iv, zero padding; CBC-encrypted with the revision's key.
    uint8_t info[0x40] = {};
    memcpy(info, meta_key, 16);
    memcpy(info + 32, meta_iv, 16);
    Bytes erk = r3::hex(TEST_ERK), riv = r3::hex(TEST_RIV);
    cbc_encrypt(erk.data(), 32, riv.data(), info, &self[INFO], 0x40);

    r3::aes_ctr(data_key, 16, data_iv, seg.data(), &self[DATA], seg.size());

    m.self = self;
    m.elf.assign(self.begin() + EHDR, self.begin() + EHDR + 0x40);
    m.elf.resize(0x100 + payload.size(), 0);
    memcpy(&m.elf[0x40], &self[PHDR], 0x38);
    memcpy(&m.elf[0x100], payload.data(), payload.size());
    return m;
}

static std::string error_of(const Bytes& blob, const r3::AppKey* keys = TEST_KEYS, size_t n = 1)
{
    try {
        r3::decrypt_app_self(blob, keys, n);
    } catch (const r3::SelfError& e) {
        return e.what();
    }
    return "";
}

int main()
{
    for (bool compress : {false, true}) {
        Made m = make_self(compress);
        Bytes out;
        try {
            out = r3::decrypt_app_self(m.self, TEST_KEYS, 1);
        } catch (const r3::SelfError& e) {
            fprintf(stderr, "FAIL: round trip (compressed=%d): %s\n", compress, e.what());
            g_fail++;
            continue;
        }
        CHECK(out == m.elf);
    }

    Made plain = make_self(false);
    CHECK(error_of(Bytes(plain.self.begin(), plain.self.begin() + 0x40)).find("truncated") != std::string::npos);
    CHECK(error_of(Bytes(plain.self.begin(), plain.self.end() - 1)).find("truncated") != std::string::npos);
    Bytes bad_magic = plain.self;
    bad_magic[0] = 'X';
    CHECK(error_of(bad_magic) == "not a SELF");
    CHECK(error_of(make_self(false, 4, TEST_REV | 0x8000).self).find("debug") != std::string::npos);
    CHECK(error_of(make_self(false, 8).self).find("not APP") != std::string::npos);   // an NPDRM SELF
    CHECK(error_of(make_self(false, 4, 0x0043).self).find("no APP key") != std::string::npos);
    static const r3::AppKey WRONG[] = {{TEST_REV, "FF112233445566778899AABBCCDDEEFF0123456789ABCDEFFEDCBA9876543210", TEST_RIV}};
    CHECK(error_of(plain.self, WRONG, 1).find("did not decrypt") != std::string::npos);

    // The table the host decrypts with: every entry well-formed, revisions distinct.
    size_t count = 0;
    const r3::AppKey* keys = r3::app_keys(&count);
    CHECK(count > 20);
    for (size_t i = 0; i < count; i++) {
        CHECK(strlen(keys[i].erk) == 64 && strlen(keys[i].riv) == 32);
        for (size_t j = 0; j < i; j++) CHECK(keys[j].revision != keys[i].revision);
    }

    CHECK(r3::crypto_selftest() == nullptr);

    if (g_fail) {
        fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("test_r3_unself ok\n");
    return 0;
}
