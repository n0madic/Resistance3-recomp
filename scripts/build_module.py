#!/usr/bin/env python3
"""Lifts the decrypted EBOOT.elf and compiles the game module (game.dylib) the host loads.

usage: build_module.py [--elf PATH] [--sdk DIR] [--work DIR] [--jobs N] [--skip-lift]

Everything it writes is derived from the user's copy of the game and stays in --work (default: ./module).
The tools and headers come from the host build's sdk/ so the module matches the host's ABI.
Modelled on the launcher of the Soulcalibur II HD port (lift.py, module.py).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import logging
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

LOG = logging.getLogger("r3.module")
ROOT = Path(__file__).resolve().parent.parent

# End of the executable sections of EBOOT.elf; .rodata follows inside the same R-X segment and must not be lifted.
CODE_END = 0xC9DA84
CHUNK_LINES = 150000
SPU_REGISTER_FN = "r3_spu_register_all"
IG_REGIONS_C = "igjob_regions.c"

# The lift reaches the host's trampoline state through exported accessors (toolkit patch 0010).
MODULE_DEFS = ("-DPPU_MODULE_ACCESSORS", "-DSPU_MODULE_ACCESSORS")
PPU_FLAGS = ("-std=gnu++20", "-O3", "-DNDEBUG", "-w", "-ffp-contract=off", "-fno-strict-aliasing", "-fPIC")
SPU_FLAGS = ("-std=gnu17", "-O2", "-DNDEBUG", "-w", "-DSPU_USE_DISPATCH_MT", "-fno-strict-aliasing", "-fPIC")
CC, CXX = "/usr/bin/clang", "/usr/bin/clang++"   # the compilers the host was built with


class BuildError(RuntimeError):
    pass


def run(cmd: list[str | Path], log_file: Path | None = None) -> None:
    LOG.debug("%s", " ".join(map(str, cmd)))
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if log_file:
        log_file.write_text(r.stdout + r.stderr)
    if r.returncode:
        raise BuildError(f"{Path(str(cmd[0])).name} {Path(str(cmd[1])).name} failed ({r.returncode}):\n"
                         f"{(r.stdout + r.stderr)[-3000:]}")


def tool(python: Path, script: Path, *args: str | Path, log_file: Path | None = None) -> None:
    # -E -s rather than -I: the toolkit's tools import their siblings (spu_elf), which needs the script's own directory
    # on sys.path. The game's files are arguments, never a directory Python searches.
    run([python, "-E", "-s", script, *args], log_file)


def lift(sdk: Path, elf: Path, work: Path, python: Path) -> tuple[Path, Path]:
    tools = sdk / "tools"
    loader_out, ppu, images, spu = work / "out", work / "recompiled", work / "spu_images", work / "spu"
    for d in (ppu, spu):
        if d.exists():
            shutil.rmtree(d)
        d.mkdir(parents=True)
    loader_out.mkdir(parents=True, exist_ok=True)

    LOG.info("loading the image")
    tool(python, tools / "ppu_loader.py", elf, "-o", f"{loader_out}/", log_file=work / "ppu_loader.log")
    LOG.info("lifting PPU code")
    tool(python, tools / "ppu_lifter.py", elf,
         "--functions", loader_out / "EBOOT.functions.json", "--hle-stubs", loader_out / "EBOOT.imports.json",
         "--code-end", hex(CODE_END), "--chunk-lines", CHUNK_LINES, "-o", ppu, log_file=work / "ppu_lift.log")

    LOG.info("lifting SPU images")
    if images.exists():
        shutil.rmtree(images)
    tool(python, tools / "extract_spu_images.py", elf, "--output", images, log_file=work / "spu_extract.log")
    shutil.copytree(images, spu / "images")
    # The SPU code that is not an ELF: the SPURS policy modules and the WWS job modules.
    jobmods = spu / "jobmods"
    ig_manifest = spu / "igjob_manifest.json"
    tool(python, ROOT / "scripts" / "extract_spu_raw.py", elf, "--tools", tools, "--images", spu / "images",
         "--jobmods", jobmods, "--ig-manifest", ig_manifest, log_file=work / "spu_extract_raw.log")
    tool(python, tools / "lift_jobmods.py", "--jobmods", jobmods, "--out", spu,
         "--register", spu / "jobmods_register.c", "--symbol-prefix", "r3", log_file=work / "jobmods_lift.log")
    tool(python, tools / "build_spu_workloads.py", "--images", spu / "images", "--lifted", spu,
         "--out", spu / "spu_workloads.c", "--register-fn", SPU_REGISTER_FN, "--constructor", "--title", "r3",
         log_file=work / "spu_lift.log")
    write_ig_regions(ig_manifest, spu / "spu_workloads.c", spu / IG_REGIONS_C)
    return ppu, spu


def write_ig_regions(manifest: Path, workloads_c: Path, out: Path) -> None:
    """Registers each Insomniac job module's span as resident code of its lifted image.

    The job dispatcher loads a module over the policy module that runs it; the runtime then dispatches inside that span
    to the module's lift only once a GET from the module's address has marked it resident (spu_overlay_note_get).
    """
    image_ids = {m.group(2): int(m.group(1))
                 for m in re.finditer(r"spu_begin_image\((\d+)\); (\w+)_spu_recomp_register\(\);", workloads_c.read_text())}
    lines = []
    for module in json.loads(manifest.read_text()):
        image = image_ids.get(module["name"])
        if image is None:
            raise BuildError(f"{module['name']} was not lifted (see spu_lift.log)")
        lines.append(f"    spu_overlay_register_region({module['ea']:#010x}u, {module['span']:#x}u, {image});")
    out.write_text(
        "/* Generated by scripts/build_module.py from igjob_manifest.json: the Insomniac job modules as resident code. */\n"
        "#include <stdint.h>\n\n"
        "void spu_overlay_register_region(uint32_t content_ea, uint32_t span, int image_id);\n\n"
        "__attribute__((constructor)) static void r3_register_ig_job_regions(void)\n{\n"
        + "\n".join(lines) + "\n}\n")


def compile_and_link(sdk: Path, ppu: Path, spu: Path, work: Path, jobs: int) -> Path:
    abi = (sdk / "ABI").read_text().strip()
    obj_dir = work / "obj"
    obj_dir.mkdir(parents=True, exist_ok=True)

    meta = work / "module_meta.c"
    meta_text = f'const char r3_module_abi[] = "{abi}";\n'
    if not meta.exists() or meta.read_text() != meta_text:
        meta.write_text(meta_text)

    units: list[tuple[str, Path, tuple[str, ...]]] = [("c++", s, PPU_FLAGS) for s in sorted(ppu.glob("ppu_recomp_*.cpp"))]
    spu_c = sorted(spu.glob("*/spu_recomp.c"))
    for name in ("spu_workloads.c", "jobmods_register.c", IG_REGIONS_C):
        if (spu / name).exists():
            spu_c.append(spu / name)
    spu_c.append(sdk / "runtime" / "spu" / "spu_dispatch_mt.c")
    units += [("c", s, SPU_FLAGS) for s in spu_c]
    units.append(("c", meta, ()))
    if not any(u[0] == "c++" for u in units):
        raise BuildError(f"no lifted PPU sources in {ppu}")

    include = [f"-I{ppu}", f"-I{sdk / 'include'}", f"-I{sdk / 'runtime' / 'ppu'}", f"-I{sdk / 'runtime' / 'spu'}",
               f"-I{sdk / 'runtime' / 'memory'}", f"-I{sdk / 'runtime' / 'platform'}"]

    def compile_one(unit: tuple[str, Path, tuple[str, ...]]) -> Path:
        lang, src, flags = unit
        cmd = [CXX if lang == "c++" else CC, *flags, *MODULE_DEFS, *include]
        # The cache key covers the source and the command: a changed lift or flag rebuilds the unit.
        h = hashlib.sha256(src.read_bytes())
        h.update(" ".join(cmd).encode())
        key = h.hexdigest()[:16]
        obj = obj_dir / f"{hashlib.sha1(str(src).encode()).hexdigest()[:8]}-{src.name}.o"
        stamp = Path(str(obj) + ".key")
        if obj.exists() and stamp.exists() and stamp.read_text() == key:
            return obj
        run([*cmd, "-c", src, "-o", obj])
        stamp.write_text(key)
        LOG.info("compiled %s", src.name)
        return obj

    LOG.info("compiling %d translation units with %d jobs", len(units), jobs)
    with ThreadPoolExecutor(jobs) as ex:
        objs = list(ex.map(compile_one, units))

    out = work / "game.dylib"
    # The module binds to the host's exported symbols when it is loaded, so it links without the host.
    run([CXX, "-shared", "-o", out, *objs, "-undefined", "dynamic_lookup"])
    return out


def main() -> int:
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--elf", type=Path, default=ROOT / "PS3_GAME/USRDIR/EBOOT.elf")
    ap.add_argument("--sdk", type=Path, default=ROOT / "dist/host/sdk")
    ap.add_argument("--work", type=Path, default=ROOT / "module")
    ap.add_argument("--python", type=Path, default=ROOT / ".venv/bin/python")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--skip-lift", action="store_true", help="reuse the lift in --work")
    args = ap.parse_args()
    try:
        args.work.mkdir(parents=True, exist_ok=True)
        if args.skip_lift:
            ppu, spu = args.work / "recompiled", args.work / "spu"
        else:
            ppu, spu = lift(args.sdk, args.elf, args.work, args.python)
        out = compile_and_link(args.sdk, ppu, spu, args.work, args.jobs)
    except BuildError as e:
        LOG.error("error: %s", e)
        return 1
    LOG.info("built %s", out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
