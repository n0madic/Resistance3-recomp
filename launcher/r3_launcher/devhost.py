# SPDX-License-Identifier: GPL-2.0-or-later
"""Running from a source checkout: finds the host build in dist/ and rebuilds it when its sources changed."""
from __future__ import annotations

import logging
import shutil
import subprocess
from pathlib import Path

LOG = logging.getLogger("r3.dev")

# What the host is built from (scripts/build_host.sh): a change to any of these makes the built host stale.
HOST_SOURCES = ("host", "patches", "scripts/build_host.sh", "scripts/lib_patches.sh", "scripts/make_host_header.py")
HOST_EXE = "r3_host"


def is_checkout(root: Path) -> bool:
    return (root / "scripts" / "build_host.sh").exists() and (root / "host" / "CMakeLists.txt").exists()


def dist_host_dir(root: Path) -> Path:
    """The directory build_host.sh writes."""
    return root / "dist" / "host"


def newest_source(root: Path) -> float:
    newest = 0.0
    for rel in HOST_SOURCES:
        path = root / rel
        files = [path] if path.is_file() else (p for p in path.rglob("*") if p.is_file())
        for f in files:
            newest = max(newest, f.stat().st_mtime)
    return newest


def is_stale(root: Path, host_dir: Path) -> bool:
    exe = host_dir / HOST_EXE
    return not exe.exists() or not (host_dir / "sdk" / "ABI").exists() or newest_source(root) > exe.stat().st_mtime


def ensure_host(root: Path) -> Path:
    """Builds the host with scripts/build_host.sh if it is missing or older than its sources; returns its directory."""
    host_dir = dist_host_dir(root)
    if not is_stale(root, host_dir):
        return host_dir
    bash = shutil.which("bash")
    if not bash:
        raise SystemExit("building the host needs bash; or pass --no-host-build to use dist/ as it is")
    LOG.info("%s is missing or older than host/, patches/: running scripts/build_host.sh", host_dir)
    try:
        subprocess.run([bash, str(root / "scripts" / "build_host.sh")], check=True, cwd=root)
    except subprocess.CalledProcessError as e:
        raise SystemExit(f"scripts/build_host.sh failed ({e.returncode})")
    return host_dir
