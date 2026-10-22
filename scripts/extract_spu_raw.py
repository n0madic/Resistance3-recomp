#!/usr/bin/env python3
"""Extracts the SPU code that is not an ELF from EBOOT.elf: SPURS policy modules and WWS job modules.

usage: extract_spu_raw.py <EBOOT.elf> --tools <sdk/tools> --images <dir> --jobmods <dir> --ig-manifest <json>

* Policy modules (pm): the game hands SPURS a guest address and size, so they are listed in PROGRAMS (found from the
  SPURS log of a run: `AddWorkloadWA: pm=... size=...`). Each is wrapped as an SPU ELF whose PT_LOAD bytes are exactly
  the pm, loaded at LS 0xA00 (where the SPURS kernel places a policy module). The workload registry matches them by the
  FNV-1a-64 of those bytes, like the pm the game passes.
* Job modules (WWS): `4 ila | entryOffset | imageSize | .. | 0xC0DEC0DE`; the module starts 0x20 bytes before the magic.
  Written as jobmod_<sha1[:12]>_e<entry>.bin for tools/lift_jobmods.py.
* Insomniac job modules: a 16-byte signature of four `ila $r2, imm` and a layout of halfword pairs (load address,
  size and entry, each in words). The engine's job dispatcher (igPm's code at 0xE28A00) DMAs the whole module, header
  included, to its load address and calls the entry. Each is wrapped as an SPU ELF at that address and listed in
  --ig-manifest (guest address, span) so the module build can register the span as resident code.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import logging
import struct
import sys
from pathlib import Path

LOG = logging.getLogger("r3.extract_spu_raw")

PM_LS_BASE = 0xA00
JOBMOD_MAGIC = b"\xc0\xde\xc0\xde"
JOBMOD_MAGIC_OFFSET = 0x20
JOBMOD_MAX_SIZE = 0x40000
# (name, guest address, size) of the policy modules, from the game's cellSpursAddWorkloadWithAttribute calls.
PROGRAMS = (
    ("pm_wws", 0xEB9E00, 12544),   # Wws_Job: Sony WWS job manager kernel
    ("pm_ig", 0xE28F80, 560),      # igPm: the engine's own policy module
)
SPU_LS_SIZE = 0x40000
IG_SIG_WORDS = 4
# Every module the dispatcher loads is linked for LS 0x100. Other headers with the same signature are a different
# layout (small position-independent modules, or igPm's dispatcher itself, which igPm loads at a fixed address).
IG_MODULE_LSA = 0x100


def is_ila_r2(word: int) -> bool:
    return (word >> 25) == 0x21 and (word & 0x7F) == 2


def find_ig_modules(elf: bytes) -> list[tuple[int, int, int, int]]:
    """(file offset, load address, size, entry) of each Insomniac job module, scanning the PT_LOAD file images."""
    found = []
    for _vaddr, offset, filesz in load_segments(elf):
        for p in range(offset, offset + filesz - 0x20 + 1, 16):
            words = struct.unpack_from(">8I", elf, p)
            if not all(is_ila_r2(w) for w in words[:IG_SIG_WORDS]):
                continue
            lsa, size, entry = (words[4] >> 16) << 2, (words[5] >> 16) << 2, (words[6] & 0xFFFF) << 2
            if lsa == IG_MODULE_LSA and size and lsa + size <= SPU_LS_SIZE and lsa <= entry < lsa + size \
                    and p + size <= offset + filesz:
                found.append((p, lsa, size, entry))
    return found


def load_segments(elf: bytes) -> list[tuple[int, int, int]]:
    """(vaddr, file offset, file size) of the ELF64 PT_LOAD segments that have file bytes."""
    phoff, = struct.unpack_from(">Q", elf, 0x20)
    phentsize, phnum = struct.unpack_from(">HH", elf, 0x36)
    segs = []
    for i in range(phnum):
        p_type, _flags, p_offset, p_vaddr, _paddr, p_filesz = struct.unpack_from(">IIQQQQ", elf, phoff + i * phentsize)
        if p_type == 1 and p_filesz:
            segs.append((p_vaddr, p_offset, p_filesz))
    return segs


def file_to_guest(elf: bytes, off: int) -> int:
    for vaddr, offset, filesz in load_segments(elf):
        if offset <= off < offset + filesz:
            return vaddr + off - offset
    raise ValueError(f"file offset {off:#x} is not in a PT_LOAD segment")


def guest_to_file(elf: bytes, addr: int) -> int:
    """File offset of a guest address, from the ELF64 PT_LOAD table."""
    for vaddr, offset, filesz in load_segments(elf):
        if vaddr <= addr < vaddr + filesz:
            return offset + addr - vaddr
    raise ValueError(f"address {addr:#x} is not in a PT_LOAD segment")


def main() -> int:
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("elf", type=Path)
    ap.add_argument("--tools", type=Path, required=True)
    ap.add_argument("--images", type=Path, required=True)
    ap.add_argument("--jobmods", type=Path, required=True)
    ap.add_argument("--ig-manifest", type=Path, required=True,
                    help="JSON list of the Insomniac job modules: image name, guest address, span")
    args = ap.parse_args()

    sys.path.insert(0, str(args.tools))
    from wrap_spu_elf import wrap   # noqa: E402

    elf = args.elf.read_bytes()
    args.images.mkdir(parents=True, exist_ok=True)
    args.jobmods.mkdir(parents=True, exist_ok=True)

    for name, addr, size in PROGRAMS:
        off = guest_to_file(elf, addr)
        code = elf[off:off + size]
        (args.images / f"{name}.elf").write_bytes(wrap(code, base=PM_LS_BASE, entry=PM_LS_BASE))
        LOG.info("policy module %s: %#x, %d bytes", name, addr, size)

    manifest = []
    for off, lsa, size, entry in find_ig_modules(elf):
        ea = file_to_guest(elf, off)
        name = f"igjob_{ea:08X}"
        (args.images / f"{name}.elf").write_bytes(wrap(elf[off:off + size], base=lsa, entry=entry))
        manifest.append({"name": name, "ea": ea, "span": size})
        LOG.info("ig job module %s: LS %#x, %d bytes, entry %#x", name, lsa, size, entry)
    if not manifest:
        LOG.error("no Insomniac job modules found")
        return 1
    args.ig_manifest.write_text(json.dumps(manifest, indent=1) + "\n")

    pos = n = 0
    while (hit := elf.find(JOBMOD_MAGIC, pos)) >= 0:
        pos = hit + 1
        start = hit - JOBMOD_MAGIC_OFFSET
        if start < 0:
            continue
        entry, size = struct.unpack_from(">II", elf, start + 0x10)
        if not 0 < size <= JOBMOD_MAX_SIZE or entry >= size or start + size > len(elf):
            continue
        raw = elf[start:start + size]
        digest = hashlib.sha1(raw).hexdigest()[:12]
        (args.jobmods / f"jobmod_{digest}_e{entry:X}.bin").write_bytes(raw)
        LOG.info("job module at %#x: entry %#x, %d bytes", start, entry, size)
        n += 1
    if not n:
        LOG.error("no job modules found")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
