// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Resistance 3 -- 60 fps where the game clock follows real time.
 *
 * The game caps itself at 30: its frame limiter (func_00736BAC) waits two vblanks (f1 = 2) of 1/60 s (the float at
 * 0x012B1F40) per frame, in menus and in play. Its clock (func_007EEA44, object GAME_CLOCK) steps by the real time
 * since the last frame, clamped to [floor, ceiling]: from the level load on (cutscenes, play) floor 1/30 and ceiling
 * 0.2 s, so it is real time at 5..30 fps; at boot and in the main menu floor = ceiling = 1/75, a fixed step per frame
 * (0.39x real time at 30 fps, as on the PS3).
 *
 * The lift carries three instruction patches switched by r3_unlock_fps (launcher/r3_launcher/profiles.py, from
 * RPCS3's "Unlock FPS" patch by Whatcookie and boma, reworked): the limiter waits one vblank, and the clock's floor is
 * 0 (with 1/30 a 60 fps frame would step 1/30: the game twice as fast). R3_UNLOCK_FPS=1 turns the switch on whenever
 * the clock is not on a fixed step, so the menus keep their 30 fps and their PS3 pace; a host thread watches it.
 */
#ifndef R3_FPS_H
#define R3_FPS_H

#include <cstdint>

extern "C" {
/* Read by the lifted code at each patched instruction (the lifter's --insn-patches guard). */
extern int r3_unlock_fps;
void r3_fps_register(void);
}

namespace r3 {

constexpr uint32_t GAME_CLOCK = 0x012DAD68u;   /* +4 floor, +8 ceiling of a step, floats in seconds */

/* Whether a clock with these bounds steps by real time (60 fps is safe) rather than by a fixed amount per frame. */
bool clock_follows_real_time(float floor, float ceiling);

/* One look at the game's clock: sets r3_unlock_fps from it. Returns whether the switch changed. */
bool fps_poll();

}  // namespace r3

#endif /* R3_FPS_H */
