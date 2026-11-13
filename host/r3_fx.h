// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Resistance 3 -- post effects the player may turn off: film grain, motion blur (camera and objects), depth of field.
 *
 * Each effect function compares its strength with a threshold constant and returns below it, the game's own "off"
 * path. The lift carries instruction patches (launcher/r3_launcher/profiles.py, R3_FX_OFF) that, while their guard is
 * set, read that threshold from 0x10000, the ELF magic of the image's first segment (2.6e38 as a float). The idea of
 * the toggles is the Windows port's (github.com/miroslavdanutalexandru-alt/Resistance3Port).
 *
 * R3_FX_OFF is a comma-separated list of grain, motion-blur, dof (or all), read once at start-up.
 */
#ifndef R3_FX_H
#define R3_FX_H

#include <string>

extern "C" {
/* Read by the lifted code at each patched instruction (the lifter's --insn-patches guards). */
extern int r3_fx_no_grain;
extern int r3_fx_no_motion_blur;
extern int r3_fx_no_dof;
void r3_fx_register(void);
}

namespace r3 {

struct FxOff {
    bool grain = false, motion_blur = false, dof = false;
};

/* Parses an R3_FX_OFF list; names it does not know go to `unknown` (comma-separated), and are otherwise ignored. */
FxOff parse_fx_off(const char* list, std::string* unknown = nullptr);

/* Sets the guards. */
void apply_fx_off(const FxOff& off);

}  // namespace r3

#endif /* R3_FX_H */
