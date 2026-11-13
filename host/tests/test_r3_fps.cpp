// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Test of host/r3_fps.cpp: the 60 fps switch follows the game clock's bounds in a fake guest memory (the values
 * measured in the game: boot, main menu, play).
 *
 *   cmake --build build/host --target test_r3_fps && build/host/test_r3_fps
 */
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#include "../r3_fps.h"

extern "C" {
uint8_t* vm_base;
}

namespace {

int g_failures, g_checks;

void check(bool ok, const char* what)
{
    g_checks++;
    if (ok) return;
    g_failures++;
    std::printf("FAIL: %s\n", what);
}

void put_f32(uint32_t ea, float f)
{
    uint32_t w;
    std::memcpy(&w, &f, sizeof w);
    for (int i = 0; i < 4; i++) vm_base[ea + i] = (uint8_t)(w >> (24 - 8 * i));
}

void set_clock(float floor, float ceiling)
{
    put_f32(r3::GAME_CLOCK + 4, floor);
    put_f32(r3::GAME_CLOCK + 8, ceiling);
}

}  // namespace

int main()
{
    /* The bounds alone. */
    check(r3::clock_follows_real_time(1.0f / 30, 0.2f), "play: floor 1/30, ceiling 0.2 -> on");
    check(r3::clock_follows_real_time(1.0f / 75, 1000.0f), "boot: floor 1/75, ceiling 1000 -> on");
    check(!r3::clock_follows_real_time(1.0f / 75, 1.0f / 75), "menu: fixed step 1/75 -> off");
    check(!r3::clock_follows_real_time(0.0f, 0.0f), "not set yet -> off");
    check(!r3::clock_follows_real_time(0.1f, 0.05f), "ceiling under the floor -> off");
    check(!r3::clock_follows_real_time(std::numeric_limits<float>::quiet_NaN(), 0.2f), "NaN -> off");
    check(!r3::clock_follows_real_time(1.0f / 30, std::numeric_limits<float>::infinity()), "infinite -> off");

    /* No guest memory yet: nothing happens. */
    check(!r3::fps_poll() && r3_unlock_fps == 0, "no vm_base: no change");

    std::vector<uint8_t> mem(r3::GAME_CLOCK + 0x100);
    vm_base = mem.data();
    check(!r3::fps_poll() && r3_unlock_fps == 0, "zero clock: stays off");
    set_clock(1.0f / 75, 1.0f / 75);
    check(!r3::fps_poll() && r3_unlock_fps == 0, "main menu: stays off");
    set_clock(1.0f / 30, 0.2f);
    check(r3::fps_poll() && r3_unlock_fps == 1, "level loaded: on");
    check(!r3::fps_poll() && r3_unlock_fps == 1, "unchanged: no change reported");
    set_clock(1.0f / 75, 1.0f / 75);
    check(r3::fps_poll() && r3_unlock_fps == 0, "back to the menu: off");

    std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures ? 1 : 0;
}
