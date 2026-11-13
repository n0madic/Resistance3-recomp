# SPDX-License-Identifier: GPL-2.0-or-later
"""Starts the host on the installed game: the environment it needs and the options of a run."""
from __future__ import annotations

import logging
import os
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

from .game import GameFolder, Home

LOG = logging.getLogger("r3.run")

HOST_PROCESS = "r3_host"


@dataclass(frozen=True)
class RunOptions:
    fps: bool = False
    log: bool = False
    lang: str | None = None
    window: str | None = None          # WxH, initial window size in points
    fullscreen: bool = False
    mouse_sens: float | None = None    # degrees per point
    unlock_fps: bool = False           # 60 fps where the game clock follows real time (host/r3_fps.cpp)


def run_environment(home: Home, game: GameFolder, vfs_root: Path, opts: RunOptions) -> dict[str, str]:
    """The environment of the host, besides the user's own: where the game's files, saves and caches are, and the options.
    The host's own defaults (host/main.cpp, set_default_env) fill in the rest and never override these."""
    env = {
        # The game's disc: /dev_bdvd/PS3_GAME is <vfs root>/PS3_GAME. Set because the host derives it from the ELF's path.
        "PS3_VFS_ROOT": str(vfs_root),
        "PS3_PARAM_SFO": str(game.param_sfo),
        "PS3_HDD0_ROOT": str(home.hdd0),
        # /dev_hdd1 is the cache partition and the game opens /dev_hdd1/cache/<title id>: that is <data>/cache/<title id>.
        "PS3_HDD1_ROOT": str(home.root),
        "PS3RECOMP_MSL_CACHE": str(home.msl_cache),
        "R3_CONTROLS": str(home.controls),
    }
    if opts.fps:
        env["R3_FPS"] = "1"
    if opts.lang:
        env["PS3_LANG"] = opts.lang
    if opts.window:
        env["PS3RECOMP_METAL_WINDOW"] = opts.window
    if opts.fullscreen:
        env["PS3RECOMP_FULLSCREEN"] = "1"
    if opts.mouse_sens is not None:
        env["PAD_MOUSE_SENS"] = repr(opts.mouse_sens)
    if opts.unlock_fps:
        env["R3_UNLOCK_FPS"] = "1"
    return env


def running_pids() -> list[int]:
    """The pids of host processes already running (exact name: `pgrep -f` would match the launcher's own command line)."""
    try:
        r = subprocess.run(["pgrep", "-x", HOST_PROCESS], capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.SubprocessError):
        return []
    return [int(p) for p in r.stdout.split()]


def require_no_host() -> None:
    """Stops the launcher while a host runs: the build rewrites the disc layout and the module under it."""
    pids = running_pids()
    if pids:
        raise SystemExit(f"{HOST_PROCESS} is already running (pid {' '.join(map(str, pids))}); "
                         f"`r3.py kill` ends it (the host ignores a plain kill)")


def kill_running() -> list[int]:
    """The host ignores SIGTERM, so a stale instance only goes with SIGKILL."""
    pids = running_pids()
    if pids:
        subprocess.run(["pkill", "-9", "-x", HOST_PROCESS], capture_output=True)
    return pids


def prepare_directories(home: Home) -> None:
    for d in (home.hdd0, home.cache, home.msl_cache):
        d.mkdir(parents=True, exist_ok=True)


def start(host: Path, module: Path, elf: Path, home: Home, env_extra: dict[str, str], opts: RunOptions) -> int:
    """Replaces this process by the host, started in the data directory (the host's relative paths are relative to it)."""
    require_no_host()
    prepare_directories(home)
    env = dict(os.environ)
    env.update(env_extra)
    cmd = [str(host), "--module", str(module), str(elf)]
    os.chdir(home.root)
    if opts.log:
        home.logs.mkdir(parents=True, exist_ok=True)
        out = home.logs / f"run-{time.strftime('%Y%m%d-%H%M%S')}.log"
        LOG.info("logging to %s and %s.err", out, out)
        sys.stdout.flush()   # nothing buffered may be written into the log after the redirect
        sys.stderr.flush()
        out_fd = os.open(out, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
        err_fd = os.open(f"{out}.err", os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
        os.dup2(out_fd, 1)
        os.dup2(err_fd, 2)
    LOG.info("starting %s", " ".join(cmd))
    sys.stdout.flush()
    sys.stderr.flush()
    os.execve(cmd[0], cmd, env)
