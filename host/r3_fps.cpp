// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Resistance 3 -- 60 fps where the game clock follows real time (see r3_fps.h).
 */
#include "r3_fps.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "r3_guest.h"

extern "C" int r3_unlock_fps = 0;

namespace {

/* The game sets the clock's bounds when it changes mode (boot, menu, level load): a few times a session. A frame or
 * two at the old setting after a change is not seen. */
constexpr auto POLL_PERIOD = std::chrono::milliseconds(10);

float load_f32(uint32_t ea)
{
    const uint32_t w = r3::load_be32(ea);
    float f;
    std::memcpy(&f, &w, sizeof f);
    return f;
}

}  // namespace

namespace r3 {

bool clock_follows_real_time(float floor, float ceiling)
{
    /* Before the game sets it the object is zero; a fixed step has floor == ceiling. */
    return std::isfinite(floor) && std::isfinite(ceiling) && floor > 0.0f && ceiling > floor;
}

bool fps_poll()
{
    if (!vm_base) return false;
    const float floor = load_f32(GAME_CLOCK + 4), ceiling = load_f32(GAME_CLOCK + 8);
    const int want = clock_follows_real_time(floor, ceiling) ? 1 : 0;
    if (__atomic_load_n(&r3_unlock_fps, __ATOMIC_RELAXED) == want) return false;
    __atomic_store_n(&r3_unlock_fps, want, __ATOMIC_RELAXED);
    std::printf("[r3] 60 fps %s (game clock step %.4f..%.3f s)\n", want ? "on" : "off: fixed step", floor, ceiling);
    return true;
}

}  // namespace r3

extern "C" void r3_fps_register(void)
{
    const char* e = std::getenv("R3_UNLOCK_FPS");
    if (!e || e[0] != '1') return;
    std::printf("[r3] 60 fps wherever the game clock follows real time (R3_UNLOCK_FPS=1)\n");
    std::thread([] {
        for (;;) {
            r3::fps_poll();
            std::this_thread::sleep_for(POLL_PERIOD);
        }
    }).detach();
}
