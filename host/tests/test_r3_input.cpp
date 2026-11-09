// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Test of host/r3_input.cpp: the mouse look's hook with a fake guest memory (the hero, the camera, the FOV and the
 * player's Options > Controls at the game's addresses).
 *
 *   cmake --build build/host --target test_r3_input
 *   build/host/test_r3_input                      # the settings are followed
 *   R3_GAME_SETTINGS=0 build/host/test_r3_input   # they are not
 *
 * The environment is read once per process, so each mode is its own run.
 */
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
uint8_t* vm_base;
static int (*g_look)(double, double);
static int (*g_in_menu)(void);
void cellPad_set_menu_query(int (*in_menu)(void)) { g_in_menu = in_menu; }
void cellPad_set_mouse_look(int (*look)(double, double)) { g_look = look; }
void r3_input_register(void);
}

namespace {

constexpr uint32_t MEM_SIZE = 0x01400000u;
constexpr uint32_t UI_MENU_FLAG = 0x00F09020u, PRESS_START_FLAG = 0x00F08EBCu;
constexpr uint32_t HERO_SLOT_1 = 0x0112336Cu, HERO_SLOT_2 = 0x011E0A68u, HERO = 0x01200000u, HERO_ROWS = 0x370u;
constexpr uint32_t CAMERA = 0x0105B4A0u, HERO_FEET = 0x01069950u, VIEW_FOV = 0x0105B5F0u;
constexpr uint32_t OPTIONS = 0x011CB200u;
constexpr double HIP_FOV = 74.0, TOL = 0.01;

int g_failures, g_checks;

void check(bool ok, const char* what, double got = 0, double want = 0)
{
    g_checks++;
    if (ok) return;
    g_failures++;
    std::printf("FAIL: %s (got %.4f, wanted %.4f)\n", what, got, want);
}

void near(double got, double want, const char* what) { check(std::fabs(got - want) < TOL, what, got, want); }

void put_be32(uint32_t ea, uint32_t v)
{
    for (int i = 0; i < 4; i++) vm_base[ea + i] = (uint8_t)(v >> (24 - 8 * i));
}

void put_f32(uint32_t ea, float f)
{
    uint32_t w;
    std::memcpy(&w, &f, sizeof w);
    put_be32(ea, w);
}

float get_f32(uint32_t ea)
{
    const uint32_t w = (uint32_t)vm_base[ea] << 24 | (uint32_t)vm_base[ea + 1] << 16 | (uint32_t)vm_base[ea + 2] << 8 |
                       vm_base[ea + 3];
    float f;
    std::memcpy(&f, &w, sizeof f);
    return f;
}

void put_row(uint32_t ea, float x, float y, float z)
{
    put_f32(ea, x);
    put_f32(ea + 4, y);
    put_f32(ea + 8, z);
}

/* On foot, looking along +z, the settings at their defaults. */
void reset()
{
    std::memset(vm_base, 0, MEM_SIZE);
    put_be32(HERO_SLOT_1, HERO);
    put_row(HERO + HERO_ROWS, 1, 0, 0);
    put_row(HERO + HERO_ROWS + 16, 0, 1, 0);
    put_row(HERO + HERO_ROWS + 32, 0, 0, 1);
    put_row(CAMERA + 32, 0, 0, 1);
    put_row(CAMERA + 48, 0, 1.6f, 0);   /* the eyes, 1.6 above the feet */
    put_row(HERO_FEET, 0, 0, 0);
    put_f32(VIEW_FOV, (float)HIP_FOV);
    put_f32(OPTIONS + 8, 0.5f);         /* Camera Speed at its default */
}

double yaw_now()
{
    return std::atan2(get_f32(HERO + HERO_ROWS + 32), get_f32(HERO + HERO_ROWS + 40)) * 180.0 / M_PI;
}

double pitch_now() { return std::asin(get_f32(HERO + HERO_ROWS + 36)) * 180.0 / M_PI; }

/* One mouse move from the reset state; the hero's yaw and pitch afterwards. Turning right lowers the game's yaw. */
bool look(double yaw_deg, double pitch_deg, double* yaw, double* pitch)
{
    const int took = g_look(yaw_deg, pitch_deg);
    *yaw = yaw_now();
    *pitch = pitch_now();
    return took != 0;
}

double zoom_ratio(double fov) { return std::tan(fov * M_PI / 360.0) / std::tan(HIP_FOV * M_PI / 360.0); }

void test_basics()
{
    double yaw, pitch;
    reset();
    check(look(10, 5, &yaw, &pitch), "the hook takes the look on foot");
    near(yaw, -10, "yaw on foot");
    near(pitch, 5, "pitch on foot");

    reset();
    check(look(0, 100, &yaw, &pitch), "a big pitch is taken");
    check(pitch > 79.9 && pitch < 80.1, "pitch stops at its limit", pitch, 80);
}

void test_hero_and_menu()
{
    double yaw, pitch;
    reset();
    put_be32(HERO_SLOT_1, 0);
    put_be32(HERO_SLOT_2, HERO);
    check(look(10, 0, &yaw, &pitch), "the hero is found in the second slot (the first is zeroed at death)");
    near(yaw, -10, "yaw through the second slot");

    reset();
    put_be32(HERO_SLOT_1, 0);
    check(!g_look(10, 0), "no hero, no look");

    reset();
    vm_base[UI_MENU_FLAG] = 1;
    check(g_in_menu() == 1, "the menu flag is a menu");
    check(!g_look(10, 0), "in a menu the stick does the look");
    vm_base[PRESS_START_FLAG] = 1;
    check(g_in_menu() == 0, "Press START is not a menu (it takes START only)");

    reset();
    put_row(CAMERA + 48, 0, 20.0f, 0);   /* the camera is far above the hero: a cutscene */
    put_row(CAMERA + 32, 1, 0, 0);
    check(!g_look(10, 0), "a camera away from the eyes is not the hero's view");
}

void test_zoom()
{
    double yaw, pitch;
    reset();
    put_f32(VIEW_FOV, 60.198f);   /* the Carbine aimed */
    look(10, 5, &yaw, &pitch);
    near(yaw, -10 * zoom_ratio(60.198), "yaw is scaled by the field of view");
    near(pitch, 5 * zoom_ratio(60.198), "pitch is scaled by the field of view");

    reset();
    put_f32(VIEW_FOV, 82.0f);
    look(10, 0, &yaw, &pitch);
    near(yaw, -10, "a field wider than the hip's is not scaled");

    reset();
    put_f32(VIEW_FOV, 0.0f);
    look(10, 0, &yaw, &pitch);
    near(yaw, -10, "a field of 0 (loading) is not scaled");
}

void test_settings_followed()
{
    double yaw, pitch;
    reset();
    put_f32(OPTIONS + 8, 1.0f);
    look(10, 4, &yaw, &pitch);
    near(yaw, -15, "Camera Speed 1 turns 1.5 times as far");
    near(pitch, 6, "Camera Speed 1 also on pitch");

    reset();
    put_f32(OPTIONS + 8, 0.0f);
    look(10, 4, &yaw, &pitch);
    near(yaw, -5, "Camera Speed 0 turns half as far");
    near(pitch, 2, "Camera Speed 0 also on pitch");

    reset();
    vm_base[OPTIONS + 7] = 1;
    look(10, 5, &yaw, &pitch);
    near(yaw, -10, "Invert-Y leaves the yaw alone");
    near(pitch, -5, "Invert-Y flips the pitch");

    reset();
    vm_base[OPTIONS + 3] = 1;
    look(10, 5, &yaw, &pitch);
    near(yaw, 10, "Invert-X flips the yaw");
    near(pitch, 5, "Invert-X leaves the pitch alone");

    reset();
    vm_base[OPTIONS + 3] = 2;
    vm_base[OPTIONS + 7] = 2;
    look(10, 5, &yaw, &pitch);
    near(yaw, -10, "an invert byte other than 1 is off (yaw)");
    near(pitch, 5, "an invert byte other than 1 is off (pitch)");

    reset();
    put_f32(OPTIONS + 8, std::nanf(""));
    look(10, 0, &yaw, &pitch);
    near(yaw, -10, "a speed that is not a number is the default");

    reset();
    put_f32(OPTIONS + 8, 3.0f);
    look(10, 0, &yaw, &pitch);
    near(yaw, -10, "a speed above 1 is not a setting");

    reset();   /* a block that is not settings (speed 3) is not trusted for its invert bytes either */
    put_f32(OPTIONS + 8, 3.0f);
    vm_base[OPTIONS + 3] = 1;
    vm_base[OPTIONS + 7] = 1;
    look(10, 5, &yaw, &pitch);
    near(yaw, -10, "no settings, no Invert-X");
    near(pitch, 5, "no settings, no Invert-Y");

    reset();   /* all together: aimed, fast, Y inverted */
    put_f32(VIEW_FOV, 60.198f);
    put_f32(OPTIONS + 8, 1.0f);
    vm_base[OPTIONS + 7] = 1;
    look(10, 4, &yaw, &pitch);
    near(yaw, -10 * zoom_ratio(60.198) * 1.5, "zoom and speed multiply");
    near(pitch, -4 * zoom_ratio(60.198) * 1.5, "zoom, speed and invert together");
}

void test_settings_not_followed()
{
    double yaw, pitch;
    reset();
    put_f32(OPTIONS + 8, 1.0f);
    vm_base[OPTIONS + 3] = 1;
    vm_base[OPTIONS + 7] = 1;
    look(10, 5, &yaw, &pitch);
    near(yaw, -10, "R3_GAME_SETTINGS=0: yaw as asked");
    near(pitch, 5, "R3_GAME_SETTINGS=0: pitch as asked");
}

}  // namespace

int main()
{
    vm_base = (uint8_t*)std::calloc(MEM_SIZE, 1);
    if (!vm_base) return 2;
    r3_input_register();
    check(g_look && g_in_menu, "r3_input_register hooks the pad");

    const char* e = std::getenv("R3_GAME_SETTINGS");
    const bool follow = !(e && e[0] == '0');
    test_basics();
    test_hero_and_menu();
    test_zoom();
    if (follow) test_settings_followed(); else test_settings_not_followed();

    std::printf("%s: %d checks, %d failed\n", follow ? "settings followed" : "settings not followed", g_checks,
                g_failures);
    return g_failures ? 1 : 0;
}
