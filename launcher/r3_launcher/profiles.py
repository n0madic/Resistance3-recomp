# SPDX-License-Identifier: GPL-2.0-or-later
"""What the lifter cannot find out from the game itself: the game versions this launcher knows."""
from __future__ import annotations

from dataclasses import dataclass, field


@dataclass(frozen=True)
class InsnPatch:
    """One instruction the PPU lift emits in two forms, `if (guard) {new} else {orig}` (the lifter's --insn-patches):
    the guard is an int the host defines and may change while the game runs. The lift stops if `orig` is not there."""
    addr: int
    orig: int
    new: int
    guard: str
    note: str

    def to_json(self) -> dict[str, str]:
        return {"addr": f"{self.addr:#010x}", "orig": f"{self.orig:#010x}", "new": f"{self.new:#010x}",
                "guard": self.guard, "note": self.note}


# The host's switch for the 60 fps patches (host/r3_fps.cpp): on while the game clock steps by real time.
UNLOCK_FPS_GUARD = "r3_unlock_fps"

# 60 fps, from RPCS3's community patch "Unlock FPS" for this game (Whatcookie, boma), reworked: the frame limiter
# (func_00736BAC) keeps its own wait, for one vblank (1/60 s) instead of two, and the game clock (func_007EEA44) gets
# no lower bound, which is 1/30 s a frame in play (at 60 fps the game would run twice as fast).
R3_UNLOCK_FPS = (
    InsnPatch(0x00736C60, 0xEC2107F2, 0xFC200818, UNLOCK_FPS_GUARD,
              "frame limiter: fmuls f1,f1,f31 -> frsp f1,f1 (period = 1 vblank, not f31 = 2)"),
    InsnPatch(0x00736CF0, 0xEC2107F2, 0xFC200818, UNLOCK_FPS_GUARD,
              "frame limiter, late-frame path: the same"),
    InsnPatch(0x007EEAC8, 0xC03F0004, 0xFC210828, UNLOCK_FPS_GUARD,
              "game clock: lfs f1,4(r31) (the step's lower bound) -> fsub f1,f1,f1 (0)"),
)

# Post effects the player may turn off (host/r3_fx.cpp), from the Windows port's post-effect toggles
# (github.com/miroslavdanutalexandru-alt/Resistance3Port, MIT). Each effect function compares its strength with a
# threshold constant it loads from code (`lis r3,hi; addi r3,r3,lo; lfs fT,0(r3)`) and returns when the strength is
# below it, the game's own "effect off" path. The patch makes that address 0x10000 (`lis r3,1`), where the image's first
# segment starts with the ELF magic, 0x7F454C46 = 2.6e38 as a float: every strength is below it.
FX_ELF_MAGIC_THRESHOLD = 0x3C600001   # lis r3,1
R3_FX_OFF = (
    InsnPatch(0x00744AC8, 0x386349F0, FX_ELF_MAGIC_THRESHOLD, "r3_fx_no_grain",
              "film grain (func_00744A18): threshold of the grain intensity p+0x08 (0.0 at 0x7449F0)"),
    InsnPatch(0x007426E8, 0x386324A0, FX_ELF_MAGIC_THRESHOLD, "r3_fx_no_motion_blur",
              "camera motion blur (func_007424F8): threshold of p+0x14 (0.0 at 0x7424A0)"),
    InsnPatch(0x00855AF0, 0x38634560, FX_ELF_MAGIC_THRESHOLD, "r3_fx_no_motion_blur",
              "object motion blur (func_00855A98): threshold of the sample scale p+0x0C (at 0x174560)"),
    InsnPatch(0x00855830, 0x38635780, FX_ELF_MAGIC_THRESHOLD, "r3_fx_no_dof",
              "depth of field (func_008557A8): threshold of the near/far blur p+0x28/p+0x38 (at 0x855780)"),
)


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
    # Instructions lifted in both forms, switched by the host at run time.
    insn_patches: tuple[InsnPatch, ...] = ()

    @property
    def work_name(self) -> str:
        return f"{self.title_id}-{self.version}"


PROFILES: dict[tuple[str, str], Profile] = {
    ("BCES01118", "01.00"): Profile(
        "BCES01118", "01.00", code_end=0xC9DA84,
        elf_sha256="d12fc5283e9f036ccd4e888a0fe4cfc76c0e136c4817128e529c1b4d762f00aa",
        wws_job_extra_entries={"8f987d5458b2": (0x5318,)},
        insn_patches=R3_UNLOCK_FPS + R3_FX_OFF),
}


def find_profile(title_id: str, version: str) -> Profile | None:
    return PROFILES.get((title_id, version))


def supported() -> str:
    return ", ".join(f"{t} {v}" for t, v in sorted(PROFILES))
