# SPDX-License-Identifier: GPL-2.0-or-later
"""Lifts the decrypted EBOOT.elf with the SDK's tools: PPU code to C++, SPU images and job modules to C.

Everything it writes is derived from the user's copy of the game and stays in the data directory.
"""
from __future__ import annotations

import hashlib
import json
import logging
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

from .game import sha256_file
from .profiles import Profile

LOG = logging.getLogger("r3.lift")

HELPERS = Path(__file__).resolve().parent.parent / "helpers"
SPU_REGISTER_FN = "r3_spu_register_all"
IG_REGIONS_C = "igjob_regions.c"
SYMBOL_PREFIX = "r3"


class LiftError(RuntimeError):
    pass


@dataclass(frozen=True)
class LiftResult:
    ppu: Path          # ppu_recomp.h + chunks
    spu: Path          # SPU lift + registration sources


def _tool(script: Path, *args: str | Path, log_file: Path, env: dict[str, str] | None = None) -> None:
    # -E -s rather than -I: the toolkit's tools import their siblings (spu_elf), which needs the script's own directory
    # on sys.path. The game's files are arguments, never a directory Python searches.
    cmd = [sys.executable, "-E", "-s", str(script), *map(str, args)]
    LOG.debug("%s", " ".join(cmd))
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    log_file.write_text(r.stdout + r.stderr)
    if r.returncode:
        raise LiftError(f"{script.name} failed ({r.returncode}); see {log_file}:\n{(r.stdout + r.stderr)[-3000:]}")


def lift_game(sdk: Path, elf: Path, work: Path, profile: Profile) -> LiftResult:
    tools = sdk / "tools"
    loader_out, ppu, images, spu = work / "out", work / "recompiled", work / "spu_images", work / "spu"
    logs = work / "logs"
    for d in (ppu, spu):
        if d.exists():
            shutil.rmtree(d)
        d.mkdir(parents=True)
    loader_out.mkdir(parents=True, exist_ok=True)
    logs.mkdir(exist_ok=True)
    started = time.monotonic()

    def step(what: str) -> None:
        LOG.info("%s (%d s)", what, time.monotonic() - started)

    step("loading the image")
    _tool(tools / "ppu_loader.py", elf, "-o", f"{loader_out}/", log_file=logs / "ppu_loader.log")
    step("lifting PPU code")
    _tool(tools / "ppu_lifter.py", elf,
          "--functions", loader_out / "EBOOT.functions.json", "--hle-stubs", loader_out / "EBOOT.imports.json",
          "--code-end", hex(profile.code_end), "--chunk-lines", profile.chunk_lines, "-o", ppu,
          log_file=logs / "ppu_lift.log")

    step("lifting SPU images")
    if images.exists():
        shutil.rmtree(images)
    _tool(tools / "extract_spu_images.py", elf, "--output", images, log_file=logs / "spu_extract.log")
    shutil.copytree(images, spu / "images")
    # The SPU code that is not an ELF: the SPURS policy modules and the WWS job modules.
    jobmods = spu / "jobmods"
    ig_manifest = spu / "igjob_manifest.json"
    _tool(HELPERS / "extract_spu_raw.py", elf, "--tools", tools, "--images", spu / "images", "--jobmods", jobmods,
          "--ig-manifest", ig_manifest, log_file=logs / "spu_extract_raw.log")
    # Entries the WWS job lift cannot see. lift_jobmods.py takes them from its environment, keyed by module.
    extra = [*filter(None, os.environ.get("LBP_JOBMOD_EXTRA_FUNCS", "").split(",")),
             *(f"{sha}:{a:#x}" for sha, addrs in profile.wws_job_extra_entries.items() for a in addrs)]
    step("lifting SPU job modules")
    _tool(tools / "lift_jobmods.py", "--jobmods", jobmods, "--out", spu, "--register", spu / "jobmods_register.c",
          "--symbol-prefix", SYMBOL_PREFIX, log_file=logs / "jobmods_lift.log",
          env={**os.environ, "LBP_JOBMOD_EXTRA_FUNCS": ",".join(extra)})
    modules = json.loads(ig_manifest.read_text())
    pic = [arg for m in modules if m.get("pic") for arg in ("--pic", m["name"])]
    extra_funcs = [arg for m in modules if m.get("extra_funcs")
                   for arg in ("--extra-funcs", f"{m['name']}={','.join(m['extra_funcs'])}")]
    step("registering the SPU workloads")
    _tool(tools / "build_spu_workloads.py", "--images", spu / "images", "--lifted", spu,
          "--out", spu / "spu_workloads.c", "--register-fn", SPU_REGISTER_FN, "--constructor",
          "--title", SYMBOL_PREFIX, *pic, *extra_funcs, log_file=logs / "spu_lift.log")
    write_ig_regions(ig_manifest, spu / "spu_workloads.c", spu / IG_REGIONS_C)
    step("lift done")
    return LiftResult(ppu, spu)


def write_ig_regions(manifest: Path, workloads_c: Path, out: Path) -> None:
    """Registers each Insomniac job module's span as resident code of its lifted image.

    The job dispatcher loads a module over the policy module that runs it; the runtime then dispatches inside that span
    to the module's lift only once a GET from the module's address has marked it resident (spu_overlay_note_get).
    A buffer module (one with "load_lsa") is a verified region: resident only when loaded at that address, and its
    lift runs only after the loaded bytes matched the ones it was lifted from (bytes in the generated file).
    A position-independent one ("pic") is registered the same way at its link address, and resident wherever it loads.
    """
    image_ids = {m.group(2): int(m.group(1))
                 for m in re.finditer(r"spu_begin_image\((\d+)\); (\w+)_spu_recomp_register\(\);", workloads_c.read_text())}
    arrays, lines = [], []
    for module in json.loads(manifest.read_text()):
        image = image_ids.get(module["name"])
        if image is None:
            raise LiftError(f"{module['name']} was not lifted (see spu_lift.log)")
        if "load_lsa" not in module:
            lines.append(f"    spu_overlay_register_region({module['ea']:#010x}u, {module['span']:#x}u, {image});")
            continue
        code = (manifest.parent / module["bytes"]).read_bytes()
        name, ea, lsa, span = module["name"], module["ea"], module["load_lsa"], module["span"]
        skip = module.get("code_offset", 0)   # header bytes the game rewrites in memory: not compared
        code = code[skip:]
        arrays.append(f"static const uint8_t {name}_code[{len(code)}] = {{{','.join(f'{b:#04x}' for b in code)}}};")
        lines += [f"    {{ const spu_overlay_code_segment seg = {{{lsa + skip:#x}u, {len(code)}u, {name}_code}};",
                  f"      if (!spu_overlay_register_{'pic' if module.get('pic') else 'verified'}_region("
                  f"{ea:#010x}u, {lsa:#x}u, {span:#x}u, {image}, &seg, 1))",
                  f"          fprintf(stderr, \"[r3] {name} not registered: it runs in the interpreter\\n\"); }}"]
    out.write_text(
        "/* Generated by the launcher (r3_launcher/lift.py) from igjob_manifest.json: the Insomniac job modules as "
        "resident code. */\n"
        "#include <stdint.h>\n#include <stdio.h>\n#include \"spu_overlay_residency.h\"\n\n"
        "void spu_overlay_register_region(uint32_t content_ea, uint32_t span, int image_id);\n\n"
        + "\n".join(arrays) + ("\n\n" if arrays else "")
        + "__attribute__((constructor)) static void r3_register_ig_job_regions(void)\n{\n"
        + "\n".join(lines) + "\n}\n")


def lift_key(sdk: Path, elf: Path, profile: Profile) -> str:
    """What a lift is a function of: the ELF, the SDK's lifter (its ABI string), the profile and the SPU helper."""
    return json.dumps({
        "elf": sha256_file(elf), "abi": (sdk / "ABI").read_text().strip(),
        "code_end": profile.code_end, "chunk_lines": profile.chunk_lines,
        "wws": {k: list(v) for k, v in sorted(profile.wws_job_extra_entries.items())},
        "extra_env": os.environ.get("LBP_JOBMOD_EXTRA_FUNCS", ""),
        "helper": hashlib.sha256((HELPERS / "extract_spu_raw.py").read_bytes()).hexdigest(),
    }, sort_keys=True)


def lift_stage(sdk: Path, elf: Path, work: Path, profile: Profile) -> tuple[LiftResult, bool]:
    """Lifts unless the lift in `work` was made from the same inputs. Returns it and whether it ran."""
    work.mkdir(parents=True, exist_ok=True)
    stamp = work / "lift.stamp"
    want = lift_key(sdk, elf, profile)
    result = LiftResult(work / "recompiled", work / "spu")
    if stamp.exists() and stamp.read_text() == want and (result.ppu / "ppu_recomp.h").exists():
        LOG.info("the lift is up to date")
        return result, False
    stamp.unlink(missing_ok=True)
    result = lift_game(sdk, elf, work, profile)
    stamp.write_text(want)
    return result, True
