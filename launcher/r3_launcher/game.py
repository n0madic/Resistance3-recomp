# SPDX-License-Identifier: GPL-2.0-or-later
"""The user's copy of the game (a PS3_GAME folder) and the data directory everything derived from it lives in."""
from __future__ import annotations

import hashlib
import json
import logging
import os
import shutil
import struct
import subprocess
from dataclasses import dataclass
from pathlib import Path

from .profiles import Profile

LOG = logging.getLogger("r3.game")

MIN_FREE_BYTES = 6 << 30   # the build products and what the game writes while it runs, besides the game itself


class GameError(RuntimeError):
    pass


@dataclass(frozen=True)
class Home:
    """The data directory. The names `gamedata/`, `cache/` and `controls.txt` are the ones the project's own checkout
    has always used, so a checkout can be its own data directory (R3_HOME=<checkout>) and keep its saves."""

    root: Path

    @property
    def game_json(self) -> Path:
        return self.root / "game.json"

    @property
    def disc(self) -> Path:        # the VFS root: /dev_bdvd/PS3_GAME is disc/PS3_GAME
        return self.root / "disc"

    @property
    def games(self) -> Path:
        return self.root / "games"

    @property
    def hdd0(self) -> Path:        # /dev_hdd0: the game's HDD install, saves, trophies
        return self.root / "gamedata" / "dev_hdd0"

    @property
    def cache(self) -> Path:       # /dev_hdd1 (the FIOS disk cache, cache/BCES01118) and the translated shaders
        return self.root / "cache"

    @property
    def msl_cache(self) -> Path:
        return self.cache / "msl"

    @property
    def controls(self) -> Path:
        return self.root / "controls.txt"

    @property
    def logs(self) -> Path:
        return self.root / "logs"

    def work(self, profile: Profile) -> Path:
        return self.games / profile.work_name


@dataclass(frozen=True)
class GameFolder:
    root: Path             # the PS3_GAME folder itself
    title_id: str
    version: str           # APP_VER of PARAM.SFO
    param_sfo: Path        # the entries as the dump names them (a dump may use another case)
    eboot_bin: Path


def read_sfo(path: Path) -> dict[str, str | int]:
    """The key/value pairs of a PARAM.SFO."""
    raw = path.read_bytes()
    if raw[:4] != b"\0PSF":
        raise GameError(f"{path} is not a PARAM.SFO")
    key_off, data_off, count = struct.unpack_from("<III", raw, 8)
    out: dict[str, str | int] = {}
    for i in range(count):
        k, fmt, length, _max, d = struct.unpack_from("<HHIII", raw, 20 + 16 * i)
        key = raw[key_off + k:raw.index(b"\0", key_off + k)].decode()
        value = raw[data_off + d:data_off + d + length]
        out[key] = (struct.unpack("<I", value[:4])[0] if fmt == 0x404
                    else value.split(b"\0")[0].decode("utf-8", "replace"))
    return out


def _child(parent: Path, name: str) -> Path | None:
    """The entry of `parent` called `name`, whatever its case (a dump may have it in upper case)."""
    exact = parent / name
    if exact.exists():
        return exact
    try:
        return next((p for p in parent.iterdir() if p.name.lower() == name.lower()), None)
    except OSError:
        return None


def open_game_folder(path: Path) -> GameFolder:
    """Checks `path`, which is the PS3_GAME folder of the disc or the folder that holds it."""
    path = path.expanduser()
    if not path.is_dir():
        raise GameError(f"{path} is not a folder")
    inner = _child(path, "PS3_GAME")
    root = (inner if inner is not None and inner.is_dir() else path).resolve()
    sfo = _child(root, "PARAM.SFO")
    usrdir = _child(root, "USRDIR")
    if sfo is None or usrdir is None:
        raise GameError(f"{path} is not the PS3_GAME folder of a disc: no PARAM.SFO and USRDIR in it")
    eboot = _child(usrdir, "EBOOT.BIN")
    if eboot is None:
        raise GameError(f"{usrdir} has no EBOOT.BIN")
    try:
        info = read_sfo(sfo)
    except (OSError, ValueError, struct.error) as e:
        raise GameError(f"cannot read {sfo}: {e}") from e
    title_id, version = str(info.get("TITLE_ID", "")), str(info.get("APP_VER") or info.get("VERSION") or "")
    if not title_id:
        raise GameError(f"{sfo} has no TITLE_ID")
    return GameFolder(root, title_id, version, sfo, eboot)


def remember(home: Home, game: GameFolder) -> None:
    home.root.mkdir(parents=True, exist_ok=True)
    home.game_json.write_text(json.dumps({"path": str(game.root)}, indent=1) + "\n")


def recall(home: Home) -> Path | None:
    try:
        return Path(json.loads(home.game_json.read_text())["path"])
    except (OSError, ValueError, KeyError, TypeError):
        return None


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(1 << 22):
            h.update(chunk)
    return h.hexdigest()


def decrypt_eboot(host: Path, game: GameFolder, elf: Path) -> bool:
    """EBOOT.BIN -> elf with the host's `unself`, unless it was done for this very EBOOT.BIN. True if it ran."""
    stamp = elf.with_name(elf.name + ".stamp")
    want = sha256_file(game.eboot_bin)
    if elf.exists() and stamp.exists() and stamp.read_text() == want:
        LOG.info("%s is up to date", elf.name)
        return False
    stamp.unlink(missing_ok=True)
    elf.parent.mkdir(parents=True, exist_ok=True)
    r = subprocess.run([str(host), "unself", str(game.eboot_bin), "-o", str(elf)], capture_output=True, text=True)
    if r.returncode:
        raise GameError(f"decrypting {game.eboot_bin} failed:\n{(r.stderr or r.stdout).strip()}")
    stamp.write_text(want)
    return True


def check_elf(profile: Profile, elf: Path) -> None:
    got = sha256_file(elf)
    if got != profile.elf_sha256:
        LOG.warning("%s is not the executable this launcher was made for (sha256 %s, expected %s): the lift may be wrong",
                    elf.name, got[:16], profile.elf_sha256[:16])


def build_disc(home: Home, game: GameFolder, elf: Path) -> Path:
    """Lays out disc/PS3_GAME as the game's /dev_bdvd/PS3_GAME: links to the user's files, and to the decrypted EBOOT.elf
    beside EBOOT.BIN. The user's folder is not changed. Returns the VFS root (disc/)."""
    target = home.disc / "PS3_GAME"
    if target.is_symlink():
        target.unlink()
    elif target.exists():
        shutil.rmtree(target)
    target.mkdir(parents=True)
    for entry in sorted(game.root.iterdir()):
        if entry.name.upper() == "USRDIR" and entry.is_dir():
            usrdir = target / entry.name
            usrdir.mkdir()
            for sub in sorted(entry.iterdir()):
                if sub.name.lower() != "eboot.elf":
                    (usrdir / sub.name).symlink_to(sub)
            (usrdir / "EBOOT.elf").symlink_to(elf)
        else:
            (target / entry.name).symlink_to(entry)
    return home.disc


def free_bytes(path: Path) -> int:
    probe = path
    while not probe.exists() and probe != probe.parent:
        probe = probe.parent
    return shutil.disk_usage(probe).free


def default_home(release_root: Path) -> Path:
    return Path(os.environ.get("R3_HOME") or release_root / "data").resolve()
