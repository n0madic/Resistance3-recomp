# SPDX-License-Identifier: GPL-2.0-or-later
"""What the lifter cannot find out from the game itself: the game versions this launcher knows."""
from __future__ import annotations

from dataclasses import dataclass, field


@dataclass(frozen=True)
class Profile:
    title_id: str
    version: str
    # End of the executable sections of EBOOT.elf; .rodata follows inside the same R-X segment and must not be lifted.
    code_end: int
    # sha256 of the EBOOT.elf the numbers below were made for; another one is only warned about.
    elf_sha256: str
    chunk_lines: int = 150000
    # Extra function entries for the WWS job modules' lift, keyed by module (the content hash in its name) -> LS addresses
    # (spu-runtime/0010): a bare address would also start a function in every other module whose code spans it.
    # 0x5318 is reached by an indirect branch into the middle of 0x5288 in jobmod_8f987d5458b2 (0xE9F800), ~20M
    # interpreted steps in 100 s of a firefight in Haven.
    wws_job_extra_entries: dict[str, tuple[int, ...]] = field(default_factory=dict)

    @property
    def work_name(self) -> str:
        return f"{self.title_id}-{self.version}"


PROFILES: dict[tuple[str, str], Profile] = {
    ("BCES01118", "01.00"): Profile(
        "BCES01118", "01.00", code_end=0xC9DA84,
        elf_sha256="d12fc5283e9f036ccd4e888a0fe4cfc76c0e136c4817128e529c1b4d762f00aa",
        wws_job_extra_entries={"8f987d5458b2": (0x5318,)}),
}


def find_profile(title_id: str, version: str) -> Profile | None:
    return PROFILES.get((title_id, version))


def supported() -> str:
    return ", ".join(f"{t} {v}" for t, v in sorted(PROFILES))
