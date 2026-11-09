# SPDX-License-Identifier: GPL-2.0-or-later
"""`r3.py doctor`: what this machine and the installation lack, in one list."""
from __future__ import annotations

import argparse
import platform
import sys
from pathlib import Path

from . import devhost, runner
from .game import MIN_FREE_BYTES, GameError, Home, free_bytes, open_game_folder, recall
from .profiles import find_profile, supported
from .toolchain import ToolchainError, describe, find_compiler, machine

OK, WARN, FAIL = "ok  ", "warn", "FAIL"
MIN_FREE_GB = MIN_FREE_BYTES >> 30


def checks(root: Path, home: Home, args: argparse.Namespace) -> list[tuple[str, str]]:
    out: list[tuple[str, str]] = []

    def add(status: str, text: str) -> None:
        out.append((status, text))

    if platform.system() == "Darwin" and machine() == "arm64":
        add(OK, f"Apple Silicon Mac, macOS {platform.mac_ver()[0]}")
    else:
        add(WARN, f"{platform.system()} {platform.machine()}: only Apple Silicon Macs (Metal) have been tried")
    add(OK, f"Python {sys.version_info.major}.{sys.version_info.minor}")

    try:
        add(OK, f"compiler: {describe(find_compiler())}")
    except ToolchainError as e:
        add(FAIL, str(e))

    free = free_bytes(home.root) / (1 << 30)
    add(OK if free >= MIN_FREE_GB else WARN, f"{free:.0f} GB free where the data directory is ({home.root}); "
        f"{MIN_FREE_GB} GB are needed besides the game")

    host_dir = args.host_dir or (devhost.dist_host_dir(root) if devhost.is_checkout(root) else root / "host")
    exe, abi = host_dir / devhost.HOST_EXE, host_dir / "sdk" / "ABI"
    if exe.exists() and abi.exists():
        stale = devhost.is_checkout(root) and devhost.is_stale(root, host_dir)
        add(WARN if stale else OK, f"host: {exe} (ABI {abi.read_text().strip()})"
            + ("; older than host/ or patches/, it is rebuilt at the next run" if stale else ""))
    else:
        add(FAIL, f"no host in {host_dir} (expected {devhost.HOST_EXE} and sdk/)")

    path = args.game or recall(home)
    if path is None:
        add(FAIL, "no game yet: run `r3.py --game <the PS3_GAME folder of your disc dump>`")
    else:
        try:
            game = open_game_folder(path)
            profile = find_profile(game.title_id, game.version)
            if profile is None:
                add(FAIL, f"{game.title_id} {game.version} in {game.root} is not supported (supported: {supported()})")
            else:
                add(OK, f"game: {game.title_id} {game.version} in {game.root}")
                work = home.work(profile)
                for name, f in (("decrypted executable", work / "EBOOT.elf"), ("lift", work / "lift.stamp"),
                                ("game module", work / "module" / "game.dylib")):
                    add(OK if f.exists() else WARN, f"{name}: " + ("built" if f.exists() else "not built yet (r3.py build)"))
        except GameError as e:
            add(FAIL, str(e))

    pids = runner.running_pids()
    if pids:
        add(WARN, f"{runner.HOST_PROCESS} is running (pid {' '.join(map(str, pids))}); `r3.py kill` ends it")
    return out


def run(root: Path, home: Home, args: argparse.Namespace) -> int:
    results = checks(root, home, args)
    for status, text in results:
        print(f"[{status}] {text}")
    return 1 if any(s == FAIL for s, _ in results) else 0
