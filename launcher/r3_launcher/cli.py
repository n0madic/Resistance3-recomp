# SPDX-License-Identifier: GPL-2.0-or-later
"""r3: set up the user's copy of Resistance 3, build the game module from it, run it on the prebuilt host.

    r3.py --game <PS3_GAME>      first run: remembers the game, builds whatever is stale, runs
    r3.py                        later runs
    r3.py build                  build only          r3.py doctor      check this machine and the installation
    r3.py kill                   end a stuck host    r3.py clean-cache / reset   start parts over

Everything derived from the game is written under the data directory (--data, R3_HOME), by default ./data next to r3.py.
"""
from __future__ import annotations

import argparse
import logging
import os
import re
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path

from . import devhost, doctor, runner
from .game import (MIN_FREE_BYTES, GameError, GameFolder, Home, build_disc, check_elf, decrypt_eboot, default_home,
                   free_bytes, open_game_folder, recall, remember)
from .lift import LiftError, lift_stage
from .module import MODULE_NAME, ModuleBuildError, ModuleInputs, build_module
from .profiles import Profile, find_profile, supported
from .toolchain import ToolchainError, describe, find_compiler

LOG = logging.getLogger("r3")
ROOT = Path(__file__).resolve().parents[2]   # the release directory, or the source checkout (devhost.is_checkout)
COMMANDS = ("run", "build", "doctor", "kill", "clean-cache", "reset")


@dataclass(frozen=True)
class Context:
    home: Home
    host: Path
    sdk: Path
    game: GameFolder
    profile: Profile
    work: Path

    @property
    def elf(self) -> Path:
        return self.work / "EBOOT.elf"

    @property
    def module(self) -> Path:
        return self.work / "module" / MODULE_NAME


def host_paths(host_dir: Path) -> tuple[Path, Path]:
    # Absolute: the host is started after a chdir into the data directory, which would break a relative --host-dir.
    host_dir = host_dir.resolve()
    exe = host_dir / devhost.HOST_EXE
    if not exe.exists() or not (host_dir / "sdk" / "ABI").exists():
        raise SystemExit(f"no host in {host_dir} (expected {devhost.HOST_EXE} and sdk/); pass --host-dir")
    return exe, host_dir / "sdk"


def locate_host_dir(args: argparse.Namespace) -> Path:
    if args.host_dir is not None:
        return args.host_dir
    if devhost.is_checkout(ROOT):
        return devhost.dist_host_dir(ROOT) if args.no_host_build else devhost.ensure_host(ROOT)
    return ROOT / "host"


def resolve_game(home: Home, given: Path | None) -> GameFolder:
    if given is not None:
        game = open_game_folder(given)
        if recall(home) != game.root:
            remember(home, game)
        return game
    path = recall(home)
    if path is None:
        raise SystemExit("no game yet: run `r3.py --game <the PS3_GAME folder of your disc dump>` once")
    try:
        return open_game_folder(path)
    except GameError as e:
        raise SystemExit(f"{e}\nthe game folder that was set up earlier has moved or changed: "
                         f"run `r3.py --game <the PS3_GAME folder>` again")


def make_context(args: argparse.Namespace, home: Home) -> Context:
    host, sdk = host_paths(locate_host_dir(args))
    game = resolve_game(home, args.game)
    profile = find_profile(game.title_id, game.version)
    if profile is None:
        raise SystemExit(f"{game.title_id} {game.version} is not a supported game version (supported: {supported()})")
    return Context(home, host, sdk, game, profile, home.work(profile))


def confirm(question: str) -> bool:
    if not sys.stdin.isatty():
        return False
    return input(f"{question} [y/N] ").strip().lower() in ("y", "yes")


class Stages:
    """Numbered banners, so a first run of several minutes shows where it is."""

    def __init__(self, total: int) -> None:
        self.total, self.n = total, 0

    def begin(self, title: str) -> None:
        self.n += 1
        LOG.info("[%d/%d] %s", self.n, self.total, title)


def build(ctx: Context, args: argparse.Namespace, stages: Stages) -> Path:
    stages.begin("the game: decrypting the executable, laying out the disc")
    if decrypt_eboot(ctx.host, ctx.game, ctx.elf):
        LOG.info("decrypted %s", ctx.game.eboot_bin)
    check_elf(ctx.profile, ctx.elf)
    build_disc(ctx.home, ctx.game, ctx.elf)
    LOG.info("the game's files are linked from %s (nothing there is changed)", ctx.game.root)

    stages.begin("lifting the game's code (a few minutes, the first time only)")
    lifted, ran = lift_stage(ctx.sdk, ctx.elf, ctx.work, ctx.profile)
    if ran:
        LOG.info("the lift is in %s", ctx.work)

    stages.begin("compiling the game module (a few minutes, the first time only)")
    cc = find_compiler()
    LOG.info("compiler: %s", describe(cc))
    module = build_module(ModuleInputs(ctx.sdk, lifted.ppu, lifted.spu, ctx.work / "module"), cc, jobs=args.jobs)
    LOG.info("module: %s", module)
    return module


def run_options(args: argparse.Namespace) -> runner.RunOptions:
    return runner.RunOptions(fps=args.fps, log=args.log, lang=args.lang, window=args.window,
                             fullscreen=args.fullscreen, mouse_sens=args.sens,
                             unlock_fps=args.unlock_fps)


def clean_cache(home: Home, shaders: bool) -> int:
    """The game's FIOS disk cache (cache/<title id>), which a data-path bug may have poisoned; the translated shaders too
    with --shaders."""
    removed = 0
    if home.cache.is_dir():
        for entry in home.cache.iterdir():
            if entry == home.msl_cache and not shaders:
                continue
            shutil.rmtree(entry) if entry.is_dir() and not entry.is_symlink() else entry.unlink()
            removed += 1
    LOG.info("removed %d entries from %s", removed, home.cache)
    return 0


def reset(home: Home, everything: bool, yes: bool) -> int:
    """Removes what is derived from the game (the lift, the module, the decrypted executable, the disc layout). The saves,
    the caches and controls.txt stay unless `everything`."""
    victims = [home.games, home.disc, home.game_json]
    if everything:
        victims += [home.root / "gamedata", home.cache, home.controls, home.logs]
    if everything and not (yes or confirm(f"This erases the saves and trophies in {home.root / 'gamedata'}. Continue?")):
        raise SystemExit("nothing removed (--yes confirms)")
    for v in victims:
        if v.is_dir() and not v.is_symlink():
            shutil.rmtree(v)
        elif v.exists() or v.is_symlink():
            v.unlink()
    LOG.info("removed %s", ", ".join(v.name for v in victims))
    return 0


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog="r3", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", nargs="?", choices=COMMANDS, default="run",
                    help="run (default): build whatever is stale, then run")
    ap.add_argument("--game", type=Path, metavar="PS3_GAME",
                    help="the PS3_GAME folder of your disc dump (or the folder holding it); remembered in the data directory")
    ap.add_argument("--data", type=Path, default=None, help="where derived data lives (default: R3_HOME, else ./data)")
    ap.add_argument("--host-dir", type=Path,
                    help="directory with r3_host and sdk/ (default: host/ of a release; in a source checkout dist/host, "
                         "rebuilt by scripts/build_host.sh when host/ or patches/ are newer)")
    ap.add_argument("--no-host-build", action="store_true", help="in a source checkout, use the host in dist/ as it is")
    ap.add_argument("--yes", action="store_true", help="answer yes: erase with reset --all")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4, help="parallel compiler jobs (default: %(default)s)")
    ap.add_argument("-v", "--verbose", action="store_true")
    g = ap.add_argument_group("run options")
    g.add_argument("--fps", action="store_true", help="print the presented frame rate")
    g.add_argument("--log", action="store_true", help="write the host's output to logs/run-<time>.log{,.err} in the data directory")
    g.add_argument("--lang", metavar="CODE", help="game language, e.g. en or ru (default: the macOS language, else English)")
    g.add_argument("--window", metavar="WxH", help="initial window size in points, e.g. 1600x900")
    g.add_argument("--fullscreen", action="store_true", help="start in full screen")
    g.add_argument("--sens", type=float, metavar="DEG", help="mouse speed in degrees per point (default: the host's)")
    g.add_argument("--unlock-fps", action="store_true",
                   help="60 fps in play and cutscenes (the game's own limit is 30); menus stay at 30")
    c = ap.add_argument_group("clean-cache and reset")
    c.add_argument("--shaders", action="store_true", help="clean-cache: also drop the translated shaders (cache/msl)")
    c.add_argument("--all", action="store_true", help="reset: also erase the saves, caches and controls.txt")
    args = ap.parse_args(argv)
    if args.window and not re.fullmatch(r"[1-9][0-9]*x[1-9][0-9]*", args.window):
        ap.error("--window expects WIDTHxHEIGHT, e.g. 1600x900")
    if args.sens is not None and args.sens <= 0:
        ap.error("--sens expects a positive number")
    if args.jobs < 1:
        ap.error("--jobs expects a positive number")
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO, format="%(message)s")
    home = Home((args.data or default_home(ROOT)).resolve())

    try:
        if args.command == "kill":
            pids = runner.kill_running()
            LOG.info("ended %s", " ".join(map(str, pids)) if pids else f"nothing: no {runner.HOST_PROCESS} is running")
            return 0
        if args.command == "clean-cache":
            return clean_cache(home, args.shaders)
        if args.command == "reset":
            return reset(home, args.all, args.yes)
        if args.command == "doctor":
            return doctor.run(ROOT, home, args)

        # Before anything is built: the disc layout and the module are rewritten under the running host otherwise.
        runner.require_no_host()
        if free_bytes(home.root) < MIN_FREE_BYTES:
            LOG.warning("less than %d GB free where %s is: the build and the game's writes may not fit",
                        MIN_FREE_BYTES >> 30, home.root)
        ctx = make_context(args, home)
        home.root.mkdir(parents=True, exist_ok=True)
        stages = Stages(3 if args.command == "build" else 4)
        build(ctx, args, stages)
        if args.command == "build":
            return 0
        stages.begin("starting the game")
        opts = run_options(args)
        return runner.start(ctx.host, ctx.module, ctx.elf, home, runner.run_environment(home, ctx.game, home.disc, opts), opts)
    except (GameError, LiftError, ModuleBuildError, ToolchainError) as e:
        raise SystemExit(f"error: {e}")
    except KeyboardInterrupt:
        raise SystemExit(130)
