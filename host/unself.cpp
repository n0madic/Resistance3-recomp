// SPDX-License-Identifier: GPL-2.0-or-later
//
// Decrypts the EBOOT.BIN of a retail PS3 disc (a SELF of program type APP) into a plain ELF, for the lifter.
//
//   r3_host unself <EBOOT.BIN> -o <EBOOT.elf>
//
// The algorithm is the APP path of RPCS3's rpcs3/Crypto/unself.cpp (GPL-2.0-or-later), laid out as in the NPDRM
// decryption of the Soulcalibur II HD port; the metadata key of each key revision is in self_app_keys.inc (RPCS3's
// key_vault.cpp, see tools/gen_app_keys.py), so it needs no Python, no crypto library and no network. It checks no
// signature or section hash: a SELF that does not decrypt fails the padding check of its metadata or yields no ELF.
#include "unself.h"

#include "crypto.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace r3 {
namespace {

using Bytes = std::vector<uint8_t>;

constexpr AppKey APP_KEYS[] = {
#include "self_app_keys.inc"
};

// --- layout constants ------------------------------------------------------------------------------------------------
constexpr uint32_t SELF_FLAG_DEBUG = 0x8000;
constexpr size_t SCE_HDR_SIZE = 0x20, EHDR64_SIZE = 0x40, PHDR64_SIZE = 0x38, SHDR64_SIZE = 0x40;
constexpr size_t META_INFO_SIZE = 0x40, META_HDR_SIZE = 0x20, META_SHDR_SIZE = 0x30;
constexpr uint32_t SELF_TYPE_APP = 4, SECTION_TYPE_PHDR = 2, SECTION_ENCRYPTED = 3, SECTION_ZLIB = 2;
constexpr uint64_t MAX_ELF_SIZE = uint64_t(1) << 30;
constexpr uint64_t MAX_SELF_SIZE = uint64_t(1) << 30;

uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t* p) { return load_be32(p); }
uint64_t be64(const uint8_t* p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

void need(const Bytes& b, uint64_t off, uint64_t n, const char* what)
{
    if (off > b.size() || n > b.size() - off) throw SelfError(std::string("truncated SELF: ") + what);
}

void write_at(Bytes& buf, uint64_t off, const uint8_t* data, size_t n)
{
    if (off > MAX_ELF_SIZE || n > MAX_ELF_SIZE - off) throw SelfError("ELF layout out of range");
    if (buf.size() < off + n) buf.resize(off + n, 0);
    memcpy(buf.data() + off, data, n);
}

}  // namespace

const AppKey* app_keys(size_t* count)
{
    *count = sizeof APP_KEYS / sizeof APP_KEYS[0];
    return APP_KEYS;
}

Bytes decrypt_app_self(const Bytes& blob, const AppKey* keys, size_t key_count)
{
    need(blob, 0, 0x70, "SELF header");
    if (memcmp(blob.data(), "SCE\0", 4) != 0) throw SelfError("not a SELF");
    uint16_t flags = be16(blob.data() + 8);
    uint32_t meta_off = be32(blob.data() + 12);
    uint64_t hdr_size = be64(blob.data() + 16);
    if (flags & SELF_FLAG_DEBUG) throw SelfError("debug fSELFs are not supported");

    const uint8_t* ext = blob.data() + 0x20;   // eight 64-bit offsets
    uint64_t prog_id_off = be64(ext + 8), ehdr_off = be64(ext + 16), phdr_off = be64(ext + 24), shdr_off = be64(ext + 32);
    need(blob, prog_id_off, 24, "program id");
    uint32_t program_type = be32(blob.data() + prog_id_off + 12);
    if (program_type != SELF_TYPE_APP) throw SelfError("program type " + std::to_string(program_type) + " is not APP");

    const AppKey* key = nullptr;
    for (size_t i = 0; i < key_count; i++)
        if (keys[i].revision == flags) key = &keys[i];
    if (!key) throw SelfError("no APP key for revision " + std::to_string(flags));
    Bytes erk = hex(key->erk), riv = hex(key->riv);

    need(blob, ehdr_off, EHDR64_SIZE, "ELF header");
    const uint8_t* ehdr = blob.data() + ehdr_off;
    if (ehdr[4] != 2) throw SelfError("only 64-bit SELFs are supported");
    uint64_t e_phoff = be64(ehdr + 0x20), e_shoff = be64(ehdr + 0x28);
    unsigned e_phnum = be16(ehdr + 0x38), e_shnum = be16(ehdr + 0x3C);
    need(blob, phdr_off, e_phnum * PHDR64_SIZE, "program headers");
    const uint8_t* phdrs = blob.data() + phdr_off;

    uint64_t info_off = (uint64_t)meta_off + SCE_HDR_SIZE;
    need(blob, info_off, META_INFO_SIZE, "metadata info");
    uint8_t info[META_INFO_SIZE];
    aes_cbc_decrypt(erk.data(), 32, riv.data(), blob.data() + info_off, info, META_INFO_SIZE);
    const uint8_t *meta_key = info, *key_pad = info + 16, *meta_iv = info + 32, *iv_pad = info + 48;
    if (key_pad[0] || iv_pad[0]) throw SelfError("metadata info did not decrypt (wrong key for this revision?)");

    uint64_t enc_start = info_off + META_INFO_SIZE;
    if (hdr_size < enc_start) throw SelfError("corrupt SELF header size");
    need(blob, enc_start, hdr_size - enc_start, "metadata headers");
    Bytes headers(hdr_size - enc_start);
    aes_ctr(meta_key, 16, meta_iv, blob.data() + enc_start, headers.data(), headers.size());
    need(headers, 0, META_HDR_SIZE, "metadata header");

    uint32_t section_count = be32(headers.data() + 12), key_count_meta = be32(headers.data() + 16);
    uint64_t keys_off = META_HDR_SIZE + (uint64_t)section_count * META_SHDR_SIZE;
    need(headers, keys_off, (uint64_t)key_count_meta * 16, "data keys");
    const uint8_t* data_keys = headers.data() + keys_off;

    Bytes out(ehdr, ehdr + EHDR64_SIZE);
    write_at(out, e_phoff, phdrs, e_phnum * PHDR64_SIZE);
    for (uint32_t i = 0; i < section_count; i++) {
        const uint8_t* s = headers.data() + META_HDR_SIZE + (size_t)i * META_SHDR_SIZE;
        uint64_t data_off = be64(s), data_size = be64(s + 8);
        uint32_t typ = be32(s + 16), prog_idx = be32(s + 20), encrypted = be32(s + 32), key_idx = be32(s + 36),
                 iv_idx = be32(s + 40), compressed = be32(s + 44);
        need(blob, data_off, data_size, "section data");
        Bytes raw(blob.begin() + data_off, blob.begin() + data_off + data_size);
        if (encrypted == SECTION_ENCRYPTED) {
            if (key_idx >= key_count_meta || iv_idx >= key_count_meta) throw SelfError("section key/iv index out of range");
            aes_ctr(data_keys + key_idx * 16, 16, data_keys + iv_idx * 16, raw.data(), raw.data(), raw.size());
        }
        if (typ != SECTION_TYPE_PHDR) continue;
        if (prog_idx >= e_phnum) throw SelfError("section refers to a missing program header");
        uint64_t p_offset = be64(phdrs + prog_idx * PHDR64_SIZE + 8);
        if (compressed == SECTION_ZLIB) {
            try {
                raw = zlib_inflate(raw.data(), raw.size(), MAX_ELF_SIZE);
            } catch (const std::runtime_error& e) {
                throw SelfError(std::string("corrupt compressed section: ") + e.what());
            }
        }
        write_at(out, p_offset, raw.data(), raw.size());
    }
    if (shdr_off) {
        need(blob, shdr_off, (uint64_t)e_shnum * SHDR64_SIZE, "section headers");
        write_at(out, e_shoff, blob.data() + shdr_off, e_shnum * SHDR64_SIZE);
    }
    if (out.size() < 4 || memcmp(out.data(), "\x7f" "ELF", 4) != 0) throw SelfError("result is not an ELF");
    return out;
}

int unself_main(int argc, char** argv)
{
    const char *in_path = nullptr, *out_path = nullptr;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) out_path = argv[++i];
        else if (argv[i][0] != '-' && !in_path) in_path = argv[i];
        else in_path = nullptr, out_path = nullptr, i = argc;   // an unknown option: usage below
    }
    if (!in_path || !out_path) {
        fprintf(stderr, "usage: r3_host unself <EBOOT.BIN> -o <EBOOT.elf>\n");
        return 2;
    }
    try {
        std::ifstream in(in_path, std::ios::binary);
        if (!in) throw SelfError(std::string("cannot read ") + in_path);
        in.seekg(0, std::ios::end);
        std::streamoff size = in.tellg();
        if (size < 0 || (uint64_t)size > MAX_SELF_SIZE) throw SelfError("the SELF is larger than expected");
        in.seekg(0);
        Bytes blob((size_t)size);
        if (!in.read((char*)blob.data(), size)) throw SelfError(std::string("cannot read ") + in_path);

        size_t count = 0;
        const AppKey* keys = app_keys(&count);
        Bytes elf = decrypt_app_self(blob, keys, count);

        // Written beside the target first, so a failure never leaves half an ELF under the final name.
        std::string tmp = std::string(out_path) + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out.write((const char*)elf.data(), (std::streamsize)elf.size());
            out.close();
            if (!out) throw SelfError("cannot write " + tmp);
        }
        if (std::rename(tmp.c_str(), out_path) != 0) {
            std::remove(tmp.c_str());
            throw SelfError(std::string("cannot write ") + out_path);
        }
        printf("wrote %s (%zu bytes)\n", out_path, elf.size());
        return 0;
    } catch (const std::exception& e) {   // SelfError, and anything else (bad_alloc on a corrupt size, ...): never an abort
        fprintf(stderr, "r3_host unself: error: %s\n", e.what());
        return 1;
    }
}

int selftest_main()
{
    if (const char* bad = crypto_selftest()) {
        fprintf(stderr, "selftest FAILED: %s\n", bad);
        return 1;
    }
    printf("selftest ok\n");
    return 0;
}

}  // namespace r3
