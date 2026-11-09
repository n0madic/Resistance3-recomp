# SPDX-License-Identifier: GPL-2.0-or-later
"""Finds the C/C++ compiler for the game module: Apple's clang from the Xcode command line tools.

Only clang that compiles the lift as it is (C++20, musttail) is accepted. There is no portable fallback: zig 0.17.0 was
tried (2026-11-09). `zig c++` compiles all 263 units, but zig's own Mach-O linker refuses the module: the lifted PPU code
reads the host's thread-local `g_trampoline_fn` directly, and with `-undefined dynamic_lookup` it reports "illegal
thread-local variable reference to regular symbol" and does not take `-Wl,-U` or `-flat_namespace`. Going through an
accessor for that variable would put back the call the module ABI dropped in v0.19.0 (about 1% of the PPU threads).
Adapted from the launcher of the Soulcalibur II HD port.
"""
from __future__ import annotations

import logging
import os
import platform
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

LOG = logging.getLogger("r3.toolchain")


class ToolchainError(RuntimeError):
    pass


@dataclass(frozen=True)
class Compiler:
    """`cc` and `cxx` are argv prefixes."""

    cc: tuple[str, ...]
    cxx: tuple[str, ...]


_PROBE = r"""
#include <atomic>
int f(int x);
int g(int x) { __attribute__((musttail)) return f(x); }
static_assert(__cplusplus >= 202002L, "C++20");
"""

APPLE_CC, APPLE_CXX = "/usr/bin/clang", "/usr/bin/clang++"
INSTALL_HINT = "install the Xcode command line tools: xcode-select --install"


def machine() -> str:
    m = platform.machine().lower()
    return "arm64" if m in ("arm64", "aarch64") else m


def command_line_tools_present() -> bool:
    """/usr/bin/clang is a stub that pops up an installer when the developer tools are missing: ask first."""
    try:
        return subprocess.run(["xcode-select", "-p"], capture_output=True, timeout=30).returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


def _system_clang_ok(cxx: str) -> bool:
    """True if `cxx` is clang and compiles the constructs the lift relies on."""
    with tempfile.TemporaryDirectory() as td:
        src = Path(td, "probe.cpp")
        src.write_text(_PROBE)
        try:
            ver = subprocess.run([cxx, "--version"], capture_output=True, text=True, timeout=30)
            if "clang" not in ver.stdout.lower():
                return False
            r = subprocess.run([cxx, "-std=gnu++20", "-c", str(src), "-o", str(Path(td, "p.o"))],
                               capture_output=True, text=True, timeout=60)
            return r.returncode == 0
        except (OSError, subprocess.SubprocessError):
            return False


def find_compiler() -> Compiler:
    """A suitable clang, Apple's first (the one the host is built with): another clang first on PATH (Homebrew's llvm)
    would build the module with a different compiler than the one the host and the lift were tested with."""
    if command_line_tools_present():
        env_cxx = os.environ.get("R3_CXX")
        for cxx in ([env_cxx] if env_cxx else []) + [APPLE_CXX, "clang++", "c++"]:
            path = shutil.which(cxx)
            if path and _system_clang_ok(path):
                cc = (os.environ.get("R3_CC") if env_cxx else None) or shutil.which(APPLE_CC if path == APPLE_CXX else "clang") \
                    or path.replace("++", "")
                return Compiler((cc,), (path,))
    raise ToolchainError(f"no suitable compiler (clang with C++20 and musttail is needed): {INSTALL_HINT}")


def describe(c: Compiler) -> str:
    return " ".join(c.cxx)


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO)
    print(describe(find_compiler()))
    sys.exit(0)
