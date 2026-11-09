# SPDX-License-Identifier: GPL-2.0-or-later
"""Compiles the lifted PPU and SPU code into the game module (game.dylib) the host loads.

The module holds everything derived from the game and nothing else. Its undefined symbols (vm_read32, ps3_hle_call, the
SPU channel API, ...) are resolved against the host's exports when the host loads it, so it links without the host.
"""
from __future__ import annotations

import hashlib
import itertools
import logging
import subprocess
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path

from .lift import IG_REGIONS_C
from .toolchain import Compiler

LOG = logging.getLogger("r3.module")

MODULE_NAME = "game.dylib"
# The PPU lift reaches the host's thread-locals directly: on macOS a module binds to the host's TLS variables when it is
# loaded. A PPU accessor (a call in front of each access; module-abi/0001 had one until v0.19.0) cost ~1% of the PPU
# threads' time in a firefight (2026-10-26, `sample`: accessor + _tlv_get_addr 1.3-1.7% with it, 0.4-0.7% without). The SPU
# lift keeps SPU_MODULE_ACCESSORS for the trampoline slot cached in spu_context (module-abi/0002): direct access put the
# SPU threads' _tlv_get_addr at 1.0-1.5%, the cached slot at 0.35-0.43%.
MODULE_DEFS = ("-DSPU_MODULE_ACCESSORS",)
PPU_FLAGS = ("-std=gnu++20", "-O3", "-DNDEBUG", "-w", "-ffp-contract=off", "-fno-strict-aliasing", "-fPIC")
SPU_FLAGS = ("-std=gnu17", "-O2", "-DNDEBUG", "-w", "-DSPU_USE_DISPATCH_MT", "-fno-strict-aliasing", "-fPIC")
LINK_FLAGS = ("-shared", "-undefined", "dynamic_lookup")   # the module binds to the host's exported symbols when loaded
ABI_SYMBOL = "r3_module_abi"   # the symbol the host reads (main.cpp, load_game_module)
LINK_STAMP = "module.key"


class ModuleBuildError(RuntimeError):
    pass


@dataclass(frozen=True)
class ModuleInputs:
    sdk: Path          # the host build's sdk/ directory (headers, tools, spu_dispatch_mt.c)
    ppu: Path          # ppu_recomp.h + ppu_recomp_NNN.cpp
    spu: Path          # <image>/spu_recomp.c, spu_workloads.c, jobmods_register.c, igjob_regions.c
    out: Path          # object cache and the finished module


def _units(inp: ModuleInputs) -> list[tuple[str, Path, tuple[str, ...]]]:
    """(language, source, flags) for every translation unit of the module."""
    units: list[tuple[str, Path, tuple[str, ...]]] = [("c++", s, PPU_FLAGS) for s in sorted(inp.ppu.glob("ppu_recomp_*.cpp"))]
    if not units:
        raise ModuleBuildError(f"no lifted PPU sources in {inp.ppu}")
    spu_c = sorted(inp.spu.glob("*/spu_recomp.c"))
    for name in ("spu_workloads.c", "jobmods_register.c", IG_REGIONS_C):
        if (inp.spu / name).exists():
            spu_c.append(inp.spu / name)
    spu_c.append(inp.sdk / "runtime" / "spu" / "spu_dispatch_mt.c")
    units += [("c", s, SPU_FLAGS) for s in spu_c]
    return units


def _run(cmd: list[str]) -> None:
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise ModuleBuildError(f"{' '.join(cmd[:3])} ... {cmd[-3]}\n{r.stderr[-3000:]}")


def build_module(inp: ModuleInputs, cc: Compiler, *, jobs: int = 8) -> Path:
    abi = (inp.sdk / "ABI").read_text().strip()
    obj_dir = inp.out / "obj"
    obj_dir.mkdir(parents=True, exist_ok=True)

    meta = inp.out / "module_meta.c"
    meta_text = f'const char {ABI_SYMBOL}[] = "{abi}";\n'
    if not meta.exists() or meta.read_text() != meta_text:
        meta.write_text(meta_text)
    units = _units(inp) + [("c", meta, ())]

    include = [f"-I{inp.ppu}", f"-I{inp.sdk / 'include'}", f"-I{inp.sdk / 'runtime' / 'ppu'}",
               f"-I{inp.sdk / 'runtime' / 'spu'}", f"-I{inp.sdk / 'runtime' / 'memory'}",
               f"-I{inp.sdk / 'runtime' / 'platform'}"]

    compiled = itertools.count(1)   # next() is atomic under the GIL: the workers share it
    started = time.monotonic()

    def compile_one(unit: tuple[str, Path, tuple[str, ...]]) -> tuple[Path, str]:
        lang, src, flags = unit
        driver = [*(cc.cxx if lang == "c++" else cc.cc), *flags, *MODULE_DEFS]
        cmd = [*driver, *include]
        # The cache key covers the source, the compiler and flags and the SDK ABI (a hash of the headers a unit is
        # compiled against): a changed lift, flag, compiler or toolkit header rebuilds the unit. Without the ABI a header
        # change that moved spu_context fields kept the old objects of every unchanged lift, and they read the context at
        # wrong offsets. The include paths are not in it: the headers' content is the ABI, and a release moved to another
        # folder must not recompile everything.
        h = hashlib.sha256(src.read_bytes())
        h.update(" ".join(driver).encode())
        h.update(abi.encode())
        key = h.hexdigest()[:16]
        obj = obj_dir / f"{hashlib.sha1(str(src).encode()).hexdigest()[:8]}-{src.name}.o"
        stamp = Path(str(obj) + ".key")
        if not (obj.exists() and stamp.exists() and stamp.read_text() == key):
            _run([*cmd, "-c", str(src), "-o", str(obj)])
            stamp.write_text(key)
            if (n := next(compiled)) % 25 == 0:
                LOG.info("compiled %d units (%d s)", n, time.monotonic() - started)
        return obj, key

    LOG.info("compiling %d translation units with %d jobs", len(units), jobs)
    with ThreadPoolExecutor(jobs) as ex:
        try:
            built = list(ex.map(compile_one, units))
        except BaseException:
            # A failed unit or Ctrl-C must not wait for the hundreds of units still queued (minutes of compiling).
            ex.shutdown(wait=True, cancel_futures=True)
            raise

    # The module is relinked only when an object changed or the module is missing: it is 200 MB.
    digest = hashlib.sha256(" ".join((*cc.cxx, *LINK_FLAGS)).encode())
    for obj, key in built:
        digest.update(f"{obj.name}={key}\n".encode())
    link_key = digest.hexdigest()
    out, link_stamp = inp.out / MODULE_NAME, inp.out / LINK_STAMP
    if out.exists() and link_stamp.exists() and link_stamp.read_text() == link_key:
        LOG.info("the module is up to date")
        return out
    link_stamp.unlink(missing_ok=True)
    _run([*cc.cxx, *LINK_FLAGS, "-o", str(out), *(str(obj) for obj, _ in built)])
    link_stamp.write_text(link_key)
    return out
