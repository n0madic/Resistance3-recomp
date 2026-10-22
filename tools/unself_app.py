"""Decrypt a retail (non-NPDRM, program type APP) PS3 SELF into a plain ELF.

Port of the APP path of RPCS3's rpcs3/Crypto/unself.cpp (GPL-2.0-or-later), following the layout of
src/install.cpp in Soulcalibur2HD-recomp. The key is the APP key for the SELF's key revision. No key is kept
here: RPCS3's rpcs3/Crypto/key_vault.cpp is downloaded at a pinned commit, checked against its SHA-256, and the APP
keys are read out of it (--key-vault reads a local copy instead, for offline use).

Usage: python3 -I unself_app.py EBOOT.BIN -o EBOOT.elf [--key-vault key_vault.cpp]
"""
from __future__ import annotations

import argparse
import hashlib
import logging
import re
import struct
import sys
import urllib.error
import urllib.request
import zlib
from pathlib import Path

from Crypto.Cipher import AES
from Crypto.Util import Counter

log = logging.getLogger("unself_app")

SELF_MAGIC = b"SCE\0"
SELF_FLAG_DEBUG = 0x8000
SELF_TYPE_APP = 4
SECTION_TYPE_PHDR = 2
SECTION_ENCRYPTED = 3
SECTION_ZLIB = 2
SCE_HDR_SIZE, EHDR64_SIZE, PHDR64_SIZE, SHDR64_SIZE = 0x20, 0x40, 0x38, 0x40
META_INFO_SIZE, META_HDR_SIZE, META_SHDR_SIZE = 0x40, 0x20, 0x30
MAX_ELF_SIZE = 1 << 30

# RPCS3's key vault at a pinned commit. To move it, change the commit and the digest together.
KEY_VAULT_COMMIT = "e53ac18f8ddd4cfd0e92a57d15740d4170c1e744"
KEY_VAULT_URL = f"https://raw.githubusercontent.com/RPCS3/rpcs3/{KEY_VAULT_COMMIT}/rpcs3/Crypto/key_vault.cpp"
KEY_VAULT_SHA256 = "a2947ff6b681258c198552ec6344e16685e2df649018206daa3f41a0a1aadbd7"
KEY_VAULT_MAX_BYTES = 1 << 20
KEY_VAULT_TIMEOUT_S = 30

# The body of KeyVault::LoadSelfAPPKeys and its entries:
# sk_APP_arr.emplace_back(<version>, <version>, <revision>, KEY_APP, "<erk>", "<riv>", ...).
_APP_LOADER_RE = re.compile(r"void\s+KeyVault::LoadSelfAPPKeys\s*\(\s*\)\s*\{(.*?)\n\}", re.S)
_APP_ENTRY_RE = re.compile(
    r"sk_APP_arr\.emplace_back\(\s*0x[0-9A-Fa-f]+\s*,\s*0x[0-9A-Fa-f]+\s*,\s*0x([0-9A-Fa-f]{1,4})\s*,"
    r"\s*KEY_APP\s*,\s*\"([0-9A-Fa-f]{64})\"\s*,\s*\"([0-9A-Fa-f]{32})\"")


class SelfError(Exception):
    pass


def download_key_vault() -> bytes:
    """RPCS3's key_vault.cpp at KEY_VAULT_COMMIT, verified against KEY_VAULT_SHA256."""
    log.info("downloading %s", KEY_VAULT_URL)
    request = urllib.request.Request(KEY_VAULT_URL, headers={"User-Agent": "unself_app"})
    try:
        with urllib.request.urlopen(request, timeout=KEY_VAULT_TIMEOUT_S) as response:
            data = response.read(KEY_VAULT_MAX_BYTES + 1)
    except (urllib.error.URLError, TimeoutError) as e:
        raise SelfError(f"cannot download key_vault.cpp: {e} (use --key-vault with a local copy)") from e
    if len(data) > KEY_VAULT_MAX_BYTES:
        raise SelfError("downloaded key_vault.cpp is larger than expected")
    digest = hashlib.sha256(data).hexdigest()
    if digest != KEY_VAULT_SHA256:
        raise SelfError(f"downloaded key_vault.cpp has SHA-256 {digest}, expected {KEY_VAULT_SHA256}")
    return data


def parse_app_keys(source: bytes) -> dict[int, tuple[bytes, bytes]]:
    """Revision -> (erk, riv) from the LoadSelfAPPKeys entries of a key_vault.cpp."""
    loader = _APP_LOADER_RE.search(source.decode("utf-8", errors="replace"))
    if not loader:
        raise SelfError("key_vault.cpp has no LoadSelfAPPKeys")
    keys = {int(rev, 16): (bytes.fromhex(erk), bytes.fromhex(riv))
            for rev, erk, riv in _APP_ENTRY_RE.findall(loader.group(1))}
    if not keys:
        raise SelfError("key_vault.cpp has no APP keys")
    return keys


def _need(blob: bytes, off: int, n: int, what: str) -> None:
    if off < 0 or n < 0 or off + n > len(blob):
        raise SelfError(f"truncated SELF: {what}")


def _ctr(key: bytes, iv: bytes, data: bytes) -> bytes:
    ctr = Counter.new(128, initial_value=int.from_bytes(iv, "big"))
    return AES.new(key, AES.MODE_CTR, counter=ctr).decrypt(data)


def _write_at(buf: bytearray, off: int, data: bytes) -> None:
    if off > MAX_ELF_SIZE or len(data) > MAX_ELF_SIZE - off:
        raise SelfError("ELF layout out of range")
    if len(buf) < off + len(data):
        buf.extend(bytes(off + len(data) - len(buf)))
    buf[off:off + len(data)] = data


def decrypt_app_self(blob: bytes, app_keys: dict[int, tuple[bytes, bytes]]) -> bytes:
    _need(blob, 0, 0x70, "SELF header")
    if blob[:4] != SELF_MAGIC:
        raise SelfError("not a SELF")
    flags, = struct.unpack_from(">H", blob, 8)
    meta_off, = struct.unpack_from(">I", blob, 12)
    hdr_size, = struct.unpack_from(">Q", blob, 16)
    if flags & SELF_FLAG_DEBUG:
        raise SelfError("debug fSELF: use ps3recomp tools/unfself.py")

    ext = struct.unpack_from(">8Q", blob, 0x20)
    prog_id_off, ehdr_off, phdr_off, shdr_off = ext[1], ext[2], ext[3], ext[4]
    _need(blob, prog_id_off, 24, "program id")
    program_type, = struct.unpack_from(">I", blob, prog_id_off + 12)
    if program_type != SELF_TYPE_APP:
        raise SelfError(f"program type {program_type} is not APP")
    if flags not in app_keys:
        raise SelfError(f"no APP key for revision {flags:#x}")
    erk, riv = app_keys[flags]

    _need(blob, ehdr_off, EHDR64_SIZE, "ELF header")
    ehdr = blob[ehdr_off:ehdr_off + EHDR64_SIZE]
    if ehdr[4] != 2:
        raise SelfError("only 64-bit SELFs are supported")
    e_phoff, e_shoff = struct.unpack_from(">QQ", ehdr, 0x20)
    e_phnum, = struct.unpack_from(">H", ehdr, 0x38)
    e_shnum, = struct.unpack_from(">H", ehdr, 0x3C)
    _need(blob, phdr_off, e_phnum * PHDR64_SIZE, "program headers")
    phdrs = blob[phdr_off:phdr_off + e_phnum * PHDR64_SIZE]

    info_off = meta_off + SCE_HDR_SIZE
    _need(blob, info_off, META_INFO_SIZE, "metadata info")
    info = AES.new(erk, AES.MODE_CBC, riv).decrypt(blob[info_off:info_off + META_INFO_SIZE])
    meta_key, key_pad, meta_iv, iv_pad = info[:16], info[16:32], info[32:48], info[48:64]
    if key_pad[0] or iv_pad[0]:
        raise SelfError("metadata info did not decrypt (wrong key for this revision?)")

    enc_start = info_off + META_INFO_SIZE
    if hdr_size < enc_start:
        raise SelfError("corrupt SELF header size")
    _need(blob, enc_start, hdr_size - enc_start, "metadata headers")
    headers = _ctr(meta_key, meta_iv, blob[enc_start:hdr_size])

    section_count, key_count = struct.unpack_from(">II", headers, 12)
    keys_off = META_HDR_SIZE + section_count * META_SHDR_SIZE
    _need(headers, keys_off, key_count * 16, "data keys")
    data_keys = headers[keys_off:keys_off + key_count * 16]

    out = bytearray(ehdr)
    _write_at(out, e_phoff, phdrs)
    for i in range(section_count):
        o = META_HDR_SIZE + i * META_SHDR_SIZE
        data_off, data_size, typ, prog_idx = struct.unpack_from(">QQII", headers, o)
        encrypted, key_idx, iv_idx, compressed = struct.unpack_from(">4I", headers, o + 32)
        _need(blob, data_off, data_size, "section data")
        raw = blob[data_off:data_off + data_size]
        if encrypted == SECTION_ENCRYPTED:
            if key_idx >= key_count or iv_idx >= key_count:
                raise SelfError("section key/iv index out of range")
            raw = _ctr(data_keys[key_idx * 16:key_idx * 16 + 16],
                       data_keys[iv_idx * 16:iv_idx * 16 + 16], raw)
        if typ != SECTION_TYPE_PHDR:
            continue
        if prog_idx >= e_phnum:
            raise SelfError("section refers to a missing program header")
        p_offset, = struct.unpack_from(">Q", phdrs, prog_idx * PHDR64_SIZE + 8)
        if compressed == SECTION_ZLIB:
            raw = zlib.decompress(raw)
        _write_at(out, p_offset, raw)
    if shdr_off:
        _need(blob, shdr_off, e_shnum * SHDR64_SIZE, "section headers")
        _write_at(out, e_shoff, blob[shdr_off:shdr_off + e_shnum * SHDR64_SIZE])
    if out[:4] != b"\x7fELF":
        raise SelfError("result is not an ELF")
    return bytes(out)


def main() -> int:
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("self_file", type=Path)
    ap.add_argument("-o", "--output", type=Path, required=True)
    ap.add_argument("--key-vault", type=Path,
                    help="local copy of RPCS3's rpcs3/Crypto/key_vault.cpp instead of downloading it")
    args = ap.parse_args()
    try:
        source = args.key_vault.read_bytes() if args.key_vault else download_key_vault()
        elf = decrypt_app_self(args.self_file.read_bytes(), parse_app_keys(source))
    except SelfError as e:
        log.error("error: %s", e)
        return 1
    args.output.write_bytes(elf)
    log.info("wrote %s (%d bytes)", args.output, len(elf))
    return 0


if __name__ == "__main__":
    sys.exit(main())
