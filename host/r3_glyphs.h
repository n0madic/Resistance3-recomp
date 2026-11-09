// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Resistance 3 -- the game's button prompts drawn as the keys and mouse buttons of controls.txt.
 *
 * Every pad button the game shows (in its texts, button bars and HUD) is one of its icons 0..17: Scaleform images
 * "img://icon<N>" the game registers from its "icons" texture pack, DXT5 textures in RSX local memory. The table the
 * game keeps them in is at ICON_TABLE (a pointer to 0x20-byte texture descriptors and their count). The host draws one
 * image per pad button from the first key, mouse button or wheel bound to it (no wider than the game's icon: its
 * button bars place their text by that width), puts the images where the game's local
 * memory ends, and points the table's descriptors at them before the game registers them with Scaleform (it copies
 * each descriptor then; registration came 4.6 s after the table was filled in the runs checked).
 */
#ifndef R3_GLYPHS_H
#define R3_GLYPHS_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "pad_keymap.h"

namespace r3 {

/* What one icon shows. */
struct Glyph {
    enum Kind { KEY, MOUSE } kind = KEY;
    std::string text;      /* KEY: the label on the key cap; MOUSE: a small mark under the buttons ("", "↑", "↓") */
    unsigned mouse = 0;    /* MOUSE: the buttons drawn lit (PAD_MOUSE_*), or 0 for the bare mouse (looking) */
    bool wheel = false;    /* MOUSE: the wheel drawn lit */
};

constexpr int ICON_COUNT = 18;   /* the button icons; the game's icons 18 and up are not buttons */

/* What icon `index` (0..17) should show with `table`, or nothing when no key presses that button (the game's own icon
 * stays). */
std::optional<Glyph> icon_glyph(const std::vector<PadBinding>& table, int index);

/* An image for the RSX: A8R8G8B8, bytes in that order, straight alpha, top row first. */
struct GlyphImage {
    int w = 0, h = 0;
    std::vector<uint8_t> argb;
};

/* Draws `g` `height` pixels tall; as wide as its label needs, at least square, at most `max_width` (when not 0: the
 * label gets smaller instead). Empty on a host that cannot draw. */
GlyphImage draw_glyph(const Glyph& g, int height, int max_width = 0);

/* The images of icons 0..17 placed in guest memory, ready to be put into the game's table. */
class IconPatcher {
public:
    /* Draws the glyphs of `table`; the first patch() writes them to guest memory (vm_base). */
    explicit IconPatcher(const std::vector<PadBinding>& table);

    int prepared() const;   /* how many icons have an image */

    /* Points every descriptor of the game's table that still holds the game's own button icon at our image. Returns
     * how many it changed (0 when the table is not there yet, or already done). */
    int patch();

private:
    struct Placed { bool used = false; uint32_t offset = 0; uint16_t w = 0, h = 0; };
    Placed placed_[ICON_COUNT];
    std::vector<uint8_t> images_[ICON_COUNT];   /* the pixels, until they are in guest memory */
    bool written_ = false;
};

}  // namespace r3

/* Starts the thread that patches the game's icon table, with the keys of controls.txt (r3::controls_table). Off with
 * R3_KEY_GLYPHS=0, for the pad layout, and in a scripted run (PAD_NO_KEYBOARD) unless R3_KEY_GLYPHS=1. */
extern "C" void r3_glyphs_register(void);

#endif /* R3_GLYPHS_H */
