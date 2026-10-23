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
* Insomniac buffer modules: the same signature with a second layout, which the dispatcher loads into fixed LS buffers
  instead of 0x100, and the dispatcher's own code. Their load addresses are not in the header: IG_BUFFER_MODULES lists
  them. Each is wrapped at its address and listed in the manifest with its bytes (a .bin beside the manifest), so the
  build registers it as a verified region: it dispatches to its lift only when a load at that address matches.
* Insomniac position-independent modules, loaded at a new LS address each time: some with the layout of the small
  buffer modules (IG_PIC_MODULES lists them), and the ones kept in the data segment, found by a third layout: the
  signature, four layout words, then 0xFFFFFFEF, 0 and two DEADDEAD words, code from +0x30, the entry at layout word
  4 + 0x20 (a function prologue in every one). The game rewrites that header in memory, so only the bytes from +0x30
  are the load's. Each is wrapped at LS 0 and lifted
  position-independent (spu_lifter --pic), and the manifest marks it "pic", with the offset of its code: the build
  registers it with spu_overlay_register_pic_region, so a load anywhere dispatches to that one lift.
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
# The job modules the dispatcher loads at LS 0x100. Other headers with the same signature are a different layout:
# the buffer modules below, and small position-independent modules loaded at a different address each time.
IG_MODULE_LSA = 0x100
# (guest address, LS address) of the buffer modules, from a census of the module loads in a run of the first level
# (Home): every DMA GET whose first 16 bytes are the signature, by source address and LS address. Only modules always
# loaded at one address are listed: 0xDB0400, 0xDB2D80 and 0xE25B80 went to hundreds of addresses (IG_PIC_MODULES),
# 0xE45480 and 0xE47C80 to two (0x7180, 0x7200).
# TODO: take a census on the later levels; a module missing here runs in the interpreter, as before.
IG_BUFFER_MODULES = (
    (0xDB7080, 0x0DF00), (0xE35F00, 0x06600), (0xE37700, 0x06780), (0xE42480, 0x06E80),
    (0xE62280, 0x20680), (0xE62780, 0x20680), (0xE63480, 0x20680), (0xE64080, 0x20680), (0xE65280, 0x20680),
    (0xE66080, 0x20680), (0xE69280, 0x21F00), (0xE6A980, 0x21F00), (0xE6C180, 0x21F00), (0xE74980, 0x21600),
    (0xE75100, 0x21600), (0xE75780, 0x23B80), (0xE75E00, 0x23B80), (0xE78800, 0x23B80), (0xE7D400, 0x24D00),
    (0xE7F100, 0x24D00), (0xE80D80, 0x24D00), (0xE82680, 0x24D00), (0xE83D80, 0x24D00), (0xE84100, 0x24D00),
    (0xE86380, 0x24D00), (0xE89A00, 0x24D00), (0xE8A600, 0x24D00),
)
# The position-independent modules (guest address): their only references to their own addresses are pc-relative
# (0xE25B80 takes its load address with a `brsl` to the next instruction). igjob_00DC0200 runs them the most of
# (Resistance 3's opening cutscene stood at 2 fps for 20-35 s in the interpreter on them); the census saw each loaded
# whole, in one GET of its layout size.
IG_PIC_MODULES = (0xDB2D80, 0xDB0400, 0xE25B80)
IG_PIC_LINK_LSA = 0
# Third layout (data-segment modules): header words 8 and 9, where the code starts, and what layout word 4 counts
# the entry from.
IG_DATA_MODULE_MARK = (0xFFFFFFEF, 0)
IG_DATA_MODULE_CODE = 0x30
IG_DATA_MODULE_ENTRY_BASE = 0x20
# A data-segment module ends at the next header, or (the last one) at a run of this many zero bytes within
# IG_DATA_MODULE_MAX; trailing zeros are alignment the loads do not always cover.
IG_DATA_MODULE_GAP = 0x40
IG_DATA_MODULE_MAX = 0x4000
# igPm's dispatcher (0xE28A00): igPm loads it to the top of LS. Its layout words give no size; this is the load's.
IG_DISPATCHER = (0xE28A00, 0x3F580, 0x510)


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


def ig_buffer_module_size(words: tuple[int, ...]) -> int:
    """Bytes a buffer module's load copies, from its layout words (checked against the loads of a run): a small module
    (entry word 8) is the high halfword of word 7, a larger one the high halfword of word 5, in words. (The low
    halfword of word 4 is the same in most larger ones, not in 0xE25B80, whose loads copy 0x2E80 bytes.)"""
    if words[6] & 0xFFFF == 8:
        return ((words[7] >> 16) * 4 + 15) & ~15
    return (words[5] >> 16) * 4


def ig_buffer_modules(elf: bytes) -> list[tuple[int, int, int, int, bytes]]:
    """(guest address, load address, size, entry, bytes) of the buffer modules and the dispatcher."""
    found = []
    for ea, lsa in IG_BUFFER_MODULES:
        words = struct.unpack_from(">8I", elf, guest_to_file(elf, ea))
        if not all(is_ila_r2(w) for w in words[:IG_SIG_WORDS]):
            raise ValueError(f"no Insomniac module signature at {ea:#x}")
        size = ig_buffer_module_size(words)
        found.append((ea, lsa, size, lsa + (words[6] & 0xFFFF) * 4))
    ea, lsa, size = IG_DISPATCHER
    words = struct.unpack_from(">8I", elf, guest_to_file(elf, ea))
    if not all(is_ila_r2(w) for w in words[:IG_SIG_WORDS]):
        raise ValueError(f"no Insomniac module signature at {ea:#x}")
    found.append((ea, lsa, size, lsa + (words[6] & 0xFFFF) * 4))
    out = []
    for ea, lsa, size, entry in found:
        if not 0 < size <= SPU_LS_SIZE - lsa or not lsa <= entry < lsa + size:
            raise ValueError(f"module at {ea:#x}: size {size:#x} or entry {entry:#x} does not fit LS {lsa:#x}")
        off = guest_to_file(elf, ea)
        out.append((ea, lsa, size, entry, elf[off:off + size]))
    return out


def ig_pic_modules(elf: bytes) -> list[tuple[int, int, int, bytes]]:
    """(guest address, size, entry, bytes) of the position-independent modules, entry at the link address."""
    out = []
    for ea in IG_PIC_MODULES:
        off = guest_to_file(elf, ea)
        words = struct.unpack_from(">8I", elf, off)
        if not all(is_ila_r2(w) for w in words[:IG_SIG_WORDS]):
            raise ValueError(f"no Insomniac module signature at {ea:#x}")
        size = ig_buffer_module_size(words)
        entry = IG_PIC_LINK_LSA + (words[6] & 0xFFFF) * 4
        if not 0 < size <= SPU_LS_SIZE or not IG_PIC_LINK_LSA <= entry < IG_PIC_LINK_LSA + size:
            raise ValueError(f"module at {ea:#x}: size {size:#x} or entry {entry:#x} does not fit")
        out.append((ea, size, entry, elf[off:off + size]))
    return out


def ig_data_modules(elf: bytes) -> list[tuple[int, int, int, bytes]]:
    """(guest address, size, entry, bytes) of the position-independent modules of the third layout, size without the
    trailing zeros, entry at the link address."""
    heads = []
    for _vaddr, offset, filesz in load_segments(elf):
        for p in range(offset, offset + filesz - IG_DATA_MODULE_CODE + 1, 16):
            words = struct.unpack_from(">10I", elf, p)
            if all(is_ila_r2(w) for w in words[:IG_SIG_WORDS]) and words[8:10] == IG_DATA_MODULE_MARK:
                heads.append((p, offset + filesz))
    out = []
    for i, (p, seg_end) in enumerate(heads):
        end = heads[i + 1][0] if i + 1 < len(heads) and heads[i + 1][0] < seg_end else seg_end
        if end == seg_end:
            gap = elf.find(bytes(IG_DATA_MODULE_GAP), p + IG_DATA_MODULE_CODE, min(seg_end, p + IG_DATA_MODULE_MAX))
            if gap < 0:
                # TODO: its loads would tell the size; until then it runs in the interpreter.
                LOG.info("data-segment module at %#x: no end found, left out", file_to_guest(elf, p))
                continue
            end = gap
        while end > p + IG_DATA_MODULE_CODE and not elf[end - 1]:
            end -= 1
        size = (end - p + 15) & ~15
        entry = IG_DATA_MODULE_ENTRY_BASE + struct.unpack_from(">I", elf, p + 16)[0]
        if not IG_DATA_MODULE_CODE <= entry < size <= SPU_LS_SIZE:
            raise ValueError(f"module at {file_to_guest(elf, p):#x}: entry {entry:#x} or size {size:#x} does not fit")
        out.append((file_to_guest(elf, p), size, IG_PIC_LINK_LSA + entry, elf[p:p + size]))
    return out


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
    for ea, lsa, size, entry, code in ig_buffer_modules(elf):
        name = f"igbuf_{ea:08X}"
        (args.images / f"{name}.elf").write_bytes(wrap(code, base=lsa, entry=entry))
        (args.ig_manifest.parent / f"{name}.bin").write_bytes(code)
        manifest.append({"name": name, "ea": ea, "span": size, "load_lsa": lsa, "bytes": f"{name}.bin"})
        LOG.info("ig buffer module %s: LS %#x, %d bytes, entry %#x", name, lsa, size, entry)
    for ea, size, entry, code in ig_pic_modules(elf):
        name = f"igpic_{ea:08X}"
        (args.images / f"{name}.elf").write_bytes(wrap(code, base=IG_PIC_LINK_LSA, entry=entry))
        (args.ig_manifest.parent / f"{name}.bin").write_bytes(code)
        manifest.append({"name": name, "ea": ea, "span": size, "load_lsa": IG_PIC_LINK_LSA, "bytes": f"{name}.bin",
                         "pic": True})
        LOG.info("ig position-independent module %s: %d bytes, entry %#x", name, size, entry)
    for ea, size, entry, code in ig_data_modules(elf):
        name = f"igdat_{ea:08X}"
        (args.images / f"{name}.elf").write_bytes(wrap(code, base=IG_PIC_LINK_LSA, entry=entry))
        (args.ig_manifest.parent / f"{name}.bin").write_bytes(code)
        manifest.append({"name": name, "ea": ea, "span": size, "load_lsa": IG_PIC_LINK_LSA, "bytes": f"{name}.bin",
                         "pic": True, "code_offset": IG_DATA_MODULE_CODE})
        LOG.info("ig data-segment module %s: %d bytes, entry %#x", name, size, entry)
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
