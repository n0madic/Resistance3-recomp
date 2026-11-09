// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Test of host/r3_glyphs.cpp: which key each of the game's button icons shows, the images, and the patching of the
 * game's icon table in a fake guest memory.
 *
 *   cmake --build build/host --target test_r3_glyphs && build/host/test_r3_glyphs [dir]
 *
 * With a directory it also writes the images there (icon<N>.ppm, over grey) to look at.
 */
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

#include "../r3_controls.h"
#include "../r3_glyphs.h"
#include "../r3_guest.h"

#include "cellPad.h"

extern "C" {
uint8_t* vm_base;
}

namespace {

using r3::load_be32;
using r3::store_be32;

std::vector<PadBinding> g_given;   /* what r3_controls_register handed cellPad */

/* macOS key codes, as in the host's key names. */
constexpr int K_A = 0, K_S = 1, K_D = 2, K_F = 3, K_G = 5, K_C = 8, K_V = 9, K_Q = 12, K_W = 13, K_E = 14, K_R = 15,
              K_1 = 18, K_J = 38, K_RETURN = 36, K_TAB = 48, K_SPACE = 49, K_BACKSPACE = 51, K_ESCAPE = 53,
              K_SHIFT = 56, K_CTRL = 59, K_LEFT = 123, K_RIGHT = 124, K_DOWN = 125, K_UP = 126;

const char* const UP_ARROW = "\xE2\x86\x91";

/* cellPad's built-in fps table (libs/input/cellPad.c with patches/input/0008). */
std::vector<PadBinding> builtin()
{
    return {
        {PAD_SRC_KEY, K_W, PAD_ACT_MOVE_UP}, {PAD_SRC_KEY, K_S, PAD_ACT_MOVE_DOWN},
        {PAD_SRC_KEY, K_A, PAD_ACT_MOVE_LEFT}, {PAD_SRC_KEY, K_D, PAD_ACT_MOVE_RIGHT},
        {PAD_SRC_KEY, K_SPACE, CELL_PAD_CTRL_CROSS},
        {PAD_SRC_KEY, K_C, CELL_PAD_CTRL_CIRCLE}, {PAD_SRC_KEY, K_CTRL, CELL_PAD_CTRL_CIRCLE},
        {PAD_SRC_KEY, K_BACKSPACE, CELL_PAD_CTRL_CIRCLE},
        {PAD_SRC_KEY, K_E, CELL_PAD_CTRL_SQUARE}, {PAD_SRC_KEY, K_R, CELL_PAD_CTRL_SQUARE},
        {PAD_SRC_KEY, K_Q, CELL_PAD_CTRL_TRIANGLE},
        {PAD_SRC_KEY, K_G, CELL_PAD_CTRL_L2}, {PAD_SRC_KEY, K_F, CELL_PAD_CTRL_R2},
        {PAD_SRC_KEY, K_SHIFT, CELL_PAD_CTRL_L3}, {PAD_SRC_KEY, K_V, CELL_PAD_CTRL_R3},
        {PAD_SRC_KEY, K_UP, CELL_PAD_CTRL_UP}, {PAD_SRC_KEY, K_1, CELL_PAD_CTRL_UP},
        {PAD_SRC_KEY, K_RIGHT, CELL_PAD_CTRL_RIGHT}, {PAD_SRC_KEY, K_DOWN, CELL_PAD_CTRL_DOWN},
        {PAD_SRC_KEY, K_LEFT, CELL_PAD_CTRL_LEFT},
        {PAD_SRC_KEY, K_TAB, CELL_PAD_CTRL_SELECT},
        {PAD_SRC_KEY, K_RETURN, PAD_ACT_CONFIRM}, {PAD_SRC_KEY, K_ESCAPE, PAD_ACT_BACK},
        {PAD_SRC_MOUSE, (int)PAD_MOUSE_LEFT, CELL_PAD_CTRL_R1}, {PAD_SRC_MOUSE, (int)PAD_MOUSE_RIGHT, CELL_PAD_CTRL_L1},
        {PAD_SRC_MOUSE, (int)PAD_MOUSE_MIDDLE, CELL_PAD_CTRL_R3},
        {PAD_SRC_WHEEL, 0, CELL_PAD_CTRL_TRIANGLE},
    };
}

/* The game's icon order. */
enum Icon { CIRCLE, SQUARE, CROSS, TRIANGLE, SELECT, START, L1, L2, L3, STICK_L, R1, R2, R3, STICK_R, UP, DOWN, LEFT, RIGHT };
constexpr uint16_t ICON_W[] = {52, 52, 52, 52, 116, 116, 84, 84, 52, 52, 84, 84, 52, 52, 52, 52, 52, 52};
constexpr uint16_t ICON_H[] = {52, 52, 52, 52, 52, 52, 52, 52, 48, 48, 52, 52, 48, 48, 52, 52, 52, 52};

constexpr uint32_t ICON_TABLE = 0x01150610u, ENTRIES = 0x02000000u, ENTRY = 0x20;
constexpr uint32_t DXT5 = 0x0001A829u, ARGB = 0x0001A529u, GAME_PITCH_HIGH = 0x00100000u;
constexpr uint32_t RSX_LOCAL_EA = 0xC0000000u, GLYPH_BEGIN = 0x0FC00000u, GLYPH_END = 0x10000000u;
constexpr uint64_t VM_SIZE = 0x100000000ull;

int g_checks, g_failed;

void check(bool ok, const std::string& what)
{
    g_checks++;
    if (ok) return;
    g_failed++;
    std::printf("FAIL: %s\n", what.c_str());
}

std::string key_text(const std::vector<PadBinding>& t, int icon)
{
    const auto g = r3::icon_glyph(t, icon);
    if (!g) return "<none>";
    if (g->kind == r3::Glyph::MOUSE) return "<mouse>";
    return g->text;
}

void expect(const std::vector<PadBinding>& t, int icon, const std::string& want, const char* what)
{
    const std::string got = key_text(t, icon);
    check(got == want, std::string(what) + ": got '" + got + "', wanted '" + want + "'");
}

void test_builtin_table()
{
    const auto t = builtin();
    expect(t, CROSS, "\xE2\x90\xA3", "cross is the jump key, Space drawn as the open box");
    expect(t, CIRCLE, "C", "circle is its first key");
    expect(t, SQUARE, "E", "square");
    expect(t, TRIANGLE, "Q", "triangle: the key listed before the wheel");
    expect(t, L2, "G", "L2");
    expect(t, R2, "F", "R2");
    expect(t, L3, "Shift", "L3");
    expect(t, R3, "V", "R3: the key listed before the middle button");
    expect(t, SELECT, "Tab", "select");
    expect(t, START, "Enter", "start has no key of its own: the menu confirm key");
    expect(t, UP, UP_ARROW, "d-pad up is the arrow");
    expect(t, STICK_L, "WASD", "the left stick is the walking keys");

    const auto r1 = r3::icon_glyph(t, R1);
    check(r1 && r1->kind == r3::Glyph::MOUSE && r1->mouse == PAD_MOUSE_LEFT && !r1->wheel, "R1 is the left mouse button");
    const auto l1 = r3::icon_glyph(t, L1);
    check(l1 && l1->kind == r3::Glyph::MOUSE && l1->mouse == PAD_MOUSE_RIGHT, "L1 is the right mouse button");
    const auto rs = r3::icon_glyph(t, STICK_R);
    check(rs && rs->kind == r3::Glyph::MOUSE && rs->mouse == 0 && !rs->wheel, "the right stick is the bare mouse");
    check(!r3::icon_glyph(t, -1) && !r3::icon_glyph(t, r3::ICON_COUNT), "no icon outside 0..17");
}

void test_remapped()
{
    std::vector<PadBinding> t = {
        {PAD_SRC_KEY, K_J, CELL_PAD_CTRL_CROSS},
        {PAD_SRC_WHEEL, 1, CELL_PAD_CTRL_TRIANGLE},
        {PAD_SRC_KEY, 110, CELL_PAD_CTRL_SQUARE},
        {PAD_SRC_KEY, K_ESCAPE, PAD_ACT_BACK},
        {PAD_SRC_KEY, K_UP, PAD_ACT_MOVE_UP}, {PAD_SRC_KEY, K_LEFT, PAD_ACT_MOVE_LEFT},
        {PAD_SRC_KEY, K_DOWN, PAD_ACT_MOVE_DOWN}, {PAD_SRC_KEY, K_RIGHT, PAD_ACT_MOVE_RIGHT},
        {PAD_SRC_KEY, K_RETURN, CELL_PAD_CTRL_R3},
    };
    expect(t, CROSS, "J", "a remapped jump");
    expect(t, CIRCLE, "Esc", "circle with no key of its own: the menu back key");
    expect(t, SQUARE, "#110", "a key with no name shows its code");
    expect(t, R3, "Enter", "Return is labelled Enter");
    expect(t, L2, "<none>", "an unbound button keeps the game's icon");
    expect(t, START, "<none>", "start with neither a key nor a confirm key");
    expect(t, STICK_L, "\xE2\x86\x91\xE2\x86\x90\xE2\x86\x93\xE2\x86\x92", "walking on the arrows");
    const auto tri = r3::icon_glyph(t, TRIANGLE);
    check(tri && tri->kind == r3::Glyph::MOUSE && tri->wheel && tri->text == UP_ARROW, "the wheel up, marked so");

    t.push_back({PAD_SRC_MOUSE, (int)PAD_MOUSE_LEFT, CELL_PAD_CTRL_CROSS});
    expect(t, CROSS, "J", "the first binding wins");

    const std::vector<PadBinding> mixed = {{PAD_SRC_KEY, K_RETURN, PAD_ACT_MOVE_UP}, {PAD_SRC_KEY, K_A, PAD_ACT_MOVE_LEFT}};
    expect(mixed, STICK_L, "Enter A", "longer names are spaced");
    const std::vector<PadBinding> mouse_walk = {{PAD_SRC_MOUSE, (int)PAD_MOUSE_LEFT, PAD_ACT_MOVE_UP}};
    expect(mouse_walk, STICK_L, "<none>", "the stick shows keys only");
}

int opaque_pixels(const r3::GlyphImage& img)
{
    int n = 0;
    for (size_t i = 0; i < img.argb.size(); i += 4) n += img.argb[i] == 255;
    return n;
}

void test_drawing()
{
    r3::Glyph key;
    key.text = "Space";
    const r3::GlyphImage wide = r3::draw_glyph(key, 52);
    check(wide.h == 52 && wide.w > 52 && wide.w % 2 == 0, "a long label makes a wide key, even width");
    check(wide.argb.size() == (size_t)wide.w * wide.h * 4, "four bytes a pixel");
    check(wide.argb[0] == 0, "the corner is clear");
    check(opaque_pixels(wide) > wide.w * wide.h / 20, "the key is drawn");

    const r3::GlyphImage capped = r3::draw_glyph(key, 52, 52);
    check(capped.w == 52 && capped.h == 52, "a long label in a narrow icon stays that narrow");
    check(opaque_pixels(capped) > 0, "with its label smaller");
    check(r3::draw_glyph(key, 52, 200).w == wide.w, "a wide enough icon leaves the key as it is");

    /* The space bar's open box is drawn as lines across the key: its white ink spans close to half the height. */
    r3::Glyph space;
    space.text = "\xE2\x90\xA3";
    const r3::GlyphImage box = r3::draw_glyph(space, 52, 52);
    int left = box.w, right = -1;
    for (int y = 0; y < box.h; ++y)
        for (int x = 0; x < box.w; ++x) {
            const uint8_t* p = &box.argb[((size_t)y * box.w + x) * 4];
            if (p[0] == 255 && p[1] > 240 && p[2] > 240 && p[3] > 240) left = std::min(left, x), right = std::max(right, x);
        }
    check(box.w == 52 && right - left >= 52 * 2 / 5, "the open box is wide enough to read");

    key.text = "Q";
    const r3::GlyphImage square = r3::draw_glyph(key, 52);
    check(square.w == 52 && square.h == 52, "a one-letter key is square");

    r3::Glyph mouse;
    mouse.kind = r3::Glyph::MOUSE;
    mouse.mouse = PAD_MOUSE_LEFT;
    const r3::GlyphImage m = r3::draw_glyph(mouse, 48);
    check(m.h == 48 && m.w > 0 && m.w < 48, "a mouse is narrower than tall");
    check(opaque_pixels(m) > 0, "the mouse is drawn");
    check(r3::draw_glyph(key, 0).argb.empty(), "no image of height 0");
}

/* The table as the game leaves it after loading its pack: 362 icons, the first 18 the DXT5 buttons. */
void fake_game_table(uint32_t count = 362)
{
    store_be32(ICON_TABLE, ENTRIES);
    store_be32(ICON_TABLE + 4, count);
    for (uint32_t i = 0; i < 20; i++) {
        const uint32_t e = ENTRIES + i * ENTRY;
        store_be32(e + 0x00, 0x02963600u + i * 0xB00);
        store_be32(e + 0x04, i < 18 ? DXT5 : 0x0001A629u);
        store_be32(e + 0x08, 0x00730303u);
        store_be32(e + 0x18, i < 18 ? (uint32_t)ICON_W[i] << 16 | ICON_H[i] : 100u << 16 | 30);
        store_be32(e + 0x1C, GAME_PITCH_HIGH | 0xD0);
    }
}

void test_patching()
{
    r3::IconPatcher p(builtin());
    check(p.prepared() == r3::ICON_COUNT, "every button has an image with the built-in table");

    std::memset(vm_base + ICON_TABLE, 0, 8);
    check(p.patch() == 0, "no table yet: nothing to do");
    fake_game_table(10);
    check(p.patch() == 0, "a table too short is not the icons");

    fake_game_table();
    store_be32(ENTRIES + SELECT * ENTRY + 4, 0x12345678u);   /* not the game's select icon */
    check(p.patch() == r3::ICON_COUNT - 1, "the button icons are taken, the unknown one is not");
    check(load_be32(ENTRIES + SELECT * ENTRY + 4) == 0x12345678u, "the unknown entry is left alone");
    check(load_be32(ENTRIES + 18 * ENTRY + 4) == 0x0001A629u, "icon 18 is not a button");

    const uint32_t e = ENTRIES + CROSS * ENTRY;
    const uint32_t off = load_be32(e), wh = load_be32(e + 0x18), w = wh >> 16;
    check(load_be32(e + 4) == ARGB, "cross is A8R8G8B8 now, still local, 2D, one level");
    check(off >= GLYPH_BEGIN && off + w * (wh & 0xFFFF) * 4 <= GLYPH_END && off % 256 == 0,
          "its image is past the game's local memory, aligned");
    check((wh & 0xFFFF) == ICON_H[CROSS] && w == ICON_W[CROSS], "the size is the image's, no wider than the game's");
    for (int i = 0; i < r3::ICON_COUNT; i++) {
        const uint32_t s = load_be32(ENTRIES + i * ENTRY + 0x18);
        if (i != SELECT) check((s >> 16) <= ICON_W[i] && (s & 0xFFFF) == ICON_H[i], "icon " + std::to_string(i) + " fits its place");
    }
    check(load_be32(e + 0x1C) == (GAME_PITCH_HIGH | w * 4), "the pitch is the image's, the high bits kept");
    check(load_be32(e + 8) == 0x00730303u, "the sampling words are the game's");
    bool drawn = false;
    for (uint32_t i = 0; i < w * 4 && !drawn; i++) drawn = vm_base[RSX_LOCAL_EA + off + (wh & 0xFFFF) / 2 * w * 4 + i];
    check(drawn, "the pixels are in guest memory");

    uint32_t ends[r3::ICON_COUNT];
    for (int i = 0; i < r3::ICON_COUNT; i++) ends[i] = load_be32(ENTRIES + i * ENTRY);
    bool apart = true;
    for (int i = 0; i < r3::ICON_COUNT; i++)
        for (int j = i + 1; j < r3::ICON_COUNT; j++) apart = apart && (i == SELECT || j == SELECT || ends[i] != ends[j]);
    check(apart, "each icon has its own image");

    check(p.patch() == 0, "done once");
    store_be32(e, 0x02963600u);   /* the game finished filling the entry after us */
    check(p.patch() == 1 && load_be32(e) == off, "an offset written back by the game is put right");

    /* The game may write any other field after us too: start's image is narrower than its icon, so each of these
     * leaves an entry that is neither the game's nor ours. */
    const uint32_t st = ENTRIES + START * ENTRY;
    const uint32_t st_wh = load_be32(st + 0x18), st_pitch = load_be32(st + 0x1C);
    check((st_wh >> 16) < ICON_W[START] && st_pitch != (GAME_PITCH_HIGH | 0xD0), "start's image is narrower");
    store_be32(st + 0x1C, GAME_PITCH_HIGH | 0xD0);
    check(p.patch() == 1 && load_be32(st + 0x1C) == st_pitch, "a pitch written back by the game is put right");
    store_be32(st + 0x04, DXT5);
    check(p.patch() == 1 && load_be32(st + 0x04) == ARGB, "a format written back by the game is put right");
    store_be32(st + 0x18, (uint32_t)ICON_W[START] << 16 | ICON_H[START]);
    check(p.patch() == 1 && load_be32(st + 0x18) == st_wh, "a size written back by the game is put right");
    check(p.patch() == 0, "and left alone after that");

    fake_game_table();          /* the pack loaded again */
    check(p.patch() == r3::ICON_COUNT, "a new table is patched again");
}

bool same(const std::vector<PadBinding>& a, const std::vector<PadBinding>& b)
{
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const PadBinding& x, const PadBinding& y) {
        return x.src == y.src && x.code == y.code && x.action == y.action;
    });
}

void register_controls(const std::string& path, const std::string& text)
{
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) { check(false, "cannot write " + path); return; }
    std::fputs(text.c_str(), f);
    std::fclose(f);
    r3_controls_register();
}

/* controls_table() after controls.txt was applied: what cellPad was given, even when that is nothing at all. */
void test_applied_controls()
{
    const char* tmp = std::getenv("TMPDIR");
    const std::string path = std::string(tmp && *tmp ? tmp : "/tmp") + "/test_r3_glyphs_" + std::to_string(getpid()) + ".txt";
    unsetenv("PAD_NO_KEYBOARD");
    setenv("PAD_KEYBOARD_LAYOUT", "fps", 1);
    setenv("R3_CONTROLS", path.c_str(), 1);

    register_controls(path, "jump = J\n");
    check(same(r3::controls_table(), g_given) && !g_given.empty(), "the applied table is what cellPad was given");
    expect(r3::controls_table(), CROSS, "J", "and the icons follow it");

    std::istringstream rendered(r3::render_controls(builtin()));
    std::string line, none;
    while (std::getline(rendered, line))   /* every action listed with no keys */
        if (!line.empty() && line[0] != '#' && line.find('=') != std::string::npos)
            none += line.substr(0, line.find('=') + 1) + "\n";
    register_controls(path, none);
    check(g_given.empty() && r3::controls_table().empty(), "a file that unbinds everything leaves no table");
    expect(r3::controls_table(), CROSS, "<none>", "and no key on the icons");
    std::remove(path.c_str());
}

void dump_images(const std::string& dir)
{
    const auto t = builtin();
    for (int i = 0; i < r3::ICON_COUNT; i++) {
        const auto g = r3::icon_glyph(t, i);
        if (!g) continue;
        const r3::GlyphImage img = r3::draw_glyph(*g, ICON_H[i], ICON_W[i]);
        const std::string path = dir + "/icon" + std::to_string(i) + ".ppm";
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) continue;
        std::fprintf(f, "P6\n%d %d\n255\n", img.w, img.h);
        for (size_t k = 0; k < img.argb.size(); k += 4) {
            const unsigned a = img.argb[k];
            for (int c = 1; c < 4; c++) std::fputc((int)((img.argb[k + c] * a + 96 * (255 - a)) / 255), f);
        }
        std::fclose(f);
    }
}

}  // namespace

extern "C" {
int cellPad_default_key_bindings(PadBinding* out, int max)
{
    const auto t = builtin();
    for (int i = 0; out && i < max && i < (int)t.size(); i++) out[i] = t[(size_t)i];
    return (int)t.size();
}
void cellPad_set_key_bindings(const PadBinding* b, int n)
{
    g_given.assign(b, b + std::max(n, 0));
}
}

int main(int argc, char** argv)
{
    void* vm = mmap(nullptr, VM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (vm == MAP_FAILED) return 2;
    vm_base = (uint8_t*)vm;

    test_builtin_table();
    test_remapped();
    test_drawing();
    test_patching();
    check(same(r3::controls_table(), builtin()), "with no controls.txt applied the table is cellPad's");
    test_applied_controls();
    if (argc > 1) dump_images(argv[1]);

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
