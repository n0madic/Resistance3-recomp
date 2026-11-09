/*
 * Resistance 3 -- button prompts as keys (see r3_glyphs.h).
 *
 * Found in the game (2026-11-09): its texts carry private-use characters that ui_manager.swf's IconFuncs replaces with
 * images (U+E001+i -> "img://icon<i>"; U+E101+i -> the icon of layout action i, through a mapping the game passes in),
 * and its SWFs also show "img://icon0".."17" directly. The game fills the table at ICON_TABLE from its "icons" pack and
 * then registers every entry as a Scaleform image, which copies the texture descriptor (func_003710B0 ->
 * func_00828FE4); the entry is not read again.
 */
#include "r3_glyphs.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

#include "cellPad.h"   /* CELL_PAD_CTRL_* */
#include "r3_controls.h"
#include "r3_guest.h"

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#endif

namespace {

using r3::load_be32;
using r3::store_be32;

/* The game's icon table: the address of its first descriptor, then their count. */
constexpr uint32_t ICON_TABLE = 0x01150610u;
constexpr uint32_t ICON_ENTRY_SIZE = 0x20;
/* Fields of a descriptor (its words are the RSX texture registers, plus the size and pitch). */
constexpr uint32_t ENTRY_OFFSET = 0x00, ENTRY_FORMAT = 0x04, ENTRY_SIZE_WH = 0x18, ENTRY_PITCH = 0x1C;
/* The format word of the game's button icons: local memory, 2D, one level, DXT5 (0x88) | linear (0x20). */
constexpr uint32_t ICON_FORMAT_DXT5 = 0x0001A829u;
constexpr uint32_t FORMAT_BYTE_SHIFT = 8, FORMAT_BYTE_MASK = 0xFFu << FORMAT_BYTE_SHIFT;
constexpr uint32_t TEX_A8R8G8B8_LINEAR = 0x85u | 0x20u;
/* Ours: the same word with A8R8G8B8 | linear in its format byte. */
constexpr uint32_t ICON_FORMAT_ARGB = (ICON_FORMAT_DXT5 & ~FORMAT_BYTE_MASK) | TEX_A8R8G8B8_LINEAR << FORMAT_BYTE_SHIFT;
constexpr uint32_t PITCH_MASK = 0x000FFFFFu;   /* the low bits of the last word; the game keeps 1 << 20 above them */

/* RSX local memory is guest 0xC0000000 + offset, and cellGcm tells the game it has 249 MB of it (0x0F900000): the
 * rest of the 256 MB window up to the PPU stack at 0xD0000000 is never handed out, and the draw engine resolves local
 * offsets without a bound. The images go there. */
constexpr uint32_t RSX_LOCAL_EA = 0xC0000000u;
constexpr uint32_t GLYPH_LOCAL_BEGIN = 0x0FC00000u, GLYPH_LOCAL_END = 0x10000000u;
constexpr uint32_t GLYPH_ALIGN = 256;

constexpr auto POLL_PERIOD = std::chrono::milliseconds(20);

/* Not pad buttons: the two sticks have icons of their own. */
constexpr unsigned STICK_LEFT = 0x01000000u, STICK_RIGHT = 0x02000000u;

/* The game's icons 0..17 in its order, with the size of its own image (from a dump of the pack). */
struct IconSlot { unsigned button; uint16_t w, h; };
constexpr IconSlot SLOTS[r3::ICON_COUNT] = {
    {CELL_PAD_CTRL_CIRCLE, 52, 52}, {CELL_PAD_CTRL_SQUARE, 52, 52}, {CELL_PAD_CTRL_CROSS, 52, 52},
    {CELL_PAD_CTRL_TRIANGLE, 52, 52}, {CELL_PAD_CTRL_SELECT, 116, 52}, {CELL_PAD_CTRL_START, 116, 52},
    {CELL_PAD_CTRL_L1, 84, 52}, {CELL_PAD_CTRL_L2, 84, 52}, {CELL_PAD_CTRL_L3, 52, 48}, {STICK_LEFT, 52, 48},
    {CELL_PAD_CTRL_R1, 84, 52}, {CELL_PAD_CTRL_R2, 84, 52}, {CELL_PAD_CTRL_R3, 52, 48}, {STICK_RIGHT, 52, 48},
    {CELL_PAD_CTRL_UP, 52, 52}, {CELL_PAD_CTRL_DOWN, 52, 52}, {CELL_PAD_CTRL_LEFT, 52, 52},
    {CELL_PAD_CTRL_RIGHT, 52, 52},
};

/* U+2423 OPEN BOX, the space bar's symbol; drawn as lines, not from the font. */
constexpr const char* OPEN_BOX = "\xE2\x90\xA3";

/* Shorter labels than controls.txt's names, where those do not fit on a key. */
constexpr const char* LABELS[][2] = {
    {"Space", OPEN_BOX}, {"Return", "Enter"}, {"Escape", "Esc"}, {"Backspace", "Bksp"}, {"ForwardDelete", "Del"},
    {"CapsLock", "Caps"}, {"PageUp", "PgUp"}, {"PageDown", "PgDn"}, {"RightShift", "RShift"}, {"RightCtrl", "RCtrl"},
    {"RightAlt", "RAlt"},
    {"Up", "\xE2\x86\x91"}, {"Down", "\xE2\x86\x93"}, {"Left", "\xE2\x86\x90"}, {"Right", "\xE2\x86\x92"},
    {"Minus", "-"}, {"Equals", "="}, {"LeftBracket", "["}, {"RightBracket", "]"}, {"Semicolon", ";"},
    {"Quote", "'"}, {"Comma", ","}, {"Period", "."}, {"Slash", "/"}, {"Backslash", "\\"}, {"Grave", "`"},
};

std::string key_label(int code)
{
    const std::string name = r3::key_name(code);
    for (const auto& l : LABELS) if (name == l[0]) return l[1];
    if (name.compare(0, 3, "key") == 0) return "#" + name.substr(3);
    return name;
}

r3::Glyph glyph_of(const PadBinding& b)
{
    r3::Glyph g;
    if (b.src == PAD_SRC_KEY) {
        g.text = key_label(b.code);
    } else if (b.src == PAD_SRC_MOUSE) {
        g.kind = r3::Glyph::MOUSE;
        g.mouse = (unsigned)b.code;
    } else {
        g.kind = r3::Glyph::MOUSE;
        g.wheel = true;
        g.text = b.code > 0 ? "\xE2\x86\x91" : b.code < 0 ? "\xE2\x86\x93" : "";
    }
    return g;
}

const PadBinding* first_bound(const std::vector<PadBinding>& table, unsigned action, bool keys_only = false)
{
    for (const PadBinding& b : table)
        if (b.action == action && (!keys_only || b.src == PAD_SRC_KEY)) return &b;
    return nullptr;
}

size_t codepoints(const std::string& s)
{
    size_t n = 0;
    for (const char c : s) n += ((unsigned char)c & 0xC0) != 0x80;
    return n;
}

}  // namespace

namespace r3 {

std::optional<Glyph> icon_glyph(const std::vector<PadBinding>& table, int index)
{
    if (index < 0 || index >= ICON_COUNT) return std::nullopt;
    const unsigned button = SLOTS[index].button;
    if (button == STICK_RIGHT) {   /* the right stick looks around: the mouse does that */
        Glyph g;
        g.kind = Glyph::MOUSE;
        return g;
    }
    if (button == STICK_LEFT) {    /* the walking keys, in the order W A S D */
        std::vector<std::string> parts;
        for (const unsigned a : {PAD_ACT_MOVE_UP, PAD_ACT_MOVE_LEFT, PAD_ACT_MOVE_DOWN, PAD_ACT_MOVE_RIGHT})
            if (const PadBinding* b = first_bound(table, a, true)) parts.push_back(key_label(b->code));
        if (parts.empty()) return std::nullopt;
        bool single = true;
        for (const std::string& p : parts) single = single && codepoints(p) == 1;
        Glyph g;
        for (const std::string& p : parts) g.text += (g.text.empty() || single ? "" : " ") + p;
        return g;
    }
    const PadBinding* b = first_bound(table, button);
    /* Cross, circle and start may have no key of their own: the menu keys press them (pad_keymap.h). */
    if (!b && button == CELL_PAD_CTRL_CROSS) b = first_bound(table, PAD_ACT_CONFIRM);
    if (!b && button == CELL_PAD_CTRL_CIRCLE) b = first_bound(table, PAD_ACT_BACK);
    if (!b && button == CELL_PAD_CTRL_START) b = first_bound(table, PAD_ACT_CONFIRM);
    if (!b) return std::nullopt;
    return glyph_of(*b);
}

#ifdef __APPLE__

namespace {

/* Colours of the drawing (sRGB, straight alpha): a dark key with a light rim, like the game's own icons. */
constexpr CGFloat FACE[4] = {0.10, 0.10, 0.11, 0.92};
constexpr CGFloat RIM[4] = {0.88, 0.88, 0.88, 1.0};
constexpr CGFloat INK[4] = {1.0, 1.0, 1.0, 1.0};
constexpr CGFloat LIT[4] = {0.98, 0.62, 0.12, 1.0};   /* the orange of the game's d-pad icons */

/* Proportions, of the image height. */
constexpr CGFloat MARGIN = 0.08, RIM_WIDTH = 0.05, CORNER = 0.18;
constexpr CGFloat KEY_TEXT = 0.40, KEY_PADDING = 0.55;
/* A lone symbol (the space bar's open box, arrows, punctuation) is drawn larger and centred by its ink: it has no cap
 * height to centre by, and the open box sits on the baseline. */
constexpr CGFloat KEY_SYMBOL_TEXT = 0.56;
constexpr CGFloat OPEN_BOX_W = 0.46, OPEN_BOX_H = 0.16, OPEN_BOX_LINE = 0.075;
/* A label too long for its icon's width shrinks until it fits, with less room around it: small beats cut off. */
constexpr CGFloat KEY_PADDING_MIN = 0.30;
constexpr CGFloat MOUSE_W = 0.56, MOUSE_H = 0.84, MOUSE_SPLIT = 0.42, MOUSE_IMAGE_W = 0.78;
constexpr CGFloat WHEEL_W = 0.18, WHEEL_H = 0.24, MARK_TEXT = 0.30;

bool is_symbol(const std::string& text)
{
    return codepoints(text) == 1 && !std::isalnum((unsigned char)text[0]);
}

struct Line {
    CTLineRef line = nullptr;
    double width = 0, cap = 0;
    bool by_ink = false;   /* centred by the bounds of its ink, else by its advance and cap height */
    CGRect ink = CGRectZero;
    Line() = default;
    Line(const Line&) = delete;              /* owns `line` */
    Line& operator=(const Line&) = delete;
    ~Line() { if (line) CFRelease(line); }
};

void make_line(Line* out, const std::string& text, CGFloat size)
{
    if (out->line) CFRelease(out->line);
    CTFontRef font = CTFontCreateUIFontForLanguage(kCTFontUIFontEmphasizedSystem, size, nullptr);
    CGColorRef ink = CGColorCreateSRGB(INK[0], INK[1], INK[2], INK[3]);
    CFStringRef str = CFStringCreateWithCString(nullptr, text.c_str(), kCFStringEncodingUTF8);
    const void* keys[] = {kCTFontAttributeName, kCTForegroundColorAttributeName};
    const void* vals[] = {font, ink};
    CFDictionaryRef attrs = CFDictionaryCreate(nullptr, keys, vals, 2, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef as = CFAttributedStringCreate(nullptr, str ? str : CFSTR("?"), attrs);
    out->line = CTLineCreateWithAttributedString(as);
    out->width = CTLineGetTypographicBounds(out->line, nullptr, nullptr, nullptr);
    out->cap = CTFontGetCapHeight(font);
    out->by_ink = is_symbol(text);
    if (out->by_ink) out->ink = CTLineGetImageBounds(out->line, nullptr);
    CFRelease(as);
    CFRelease(attrs);
    if (str) CFRelease(str);
    CGColorRelease(ink);
    CFRelease(font);
}

void draw_line_at(CGContextRef ctx, const Line& l, CGFloat cx, CGFloat cy)
{
    if (l.by_ink && !CGRectIsEmpty(l.ink))
        CGContextSetTextPosition(ctx, cx - CGRectGetMidX(l.ink), cy - CGRectGetMidY(l.ink));
    else
        CGContextSetTextPosition(ctx, cx - l.width / 2, cy - l.cap / 2);
    CTLineDraw(l.line, ctx);
}

void fill_and_rim(CGContextRef ctx, CGPathRef path, CGFloat rim)
{
    CGContextSetRGBFillColor(ctx, FACE[0], FACE[1], FACE[2], FACE[3]);
    CGContextAddPath(ctx, path);
    CGContextFillPath(ctx);
    CGContextSetRGBStrokeColor(ctx, RIM[0], RIM[1], RIM[2], RIM[3]);
    CGContextSetLineWidth(ctx, rim);
    CGContextAddPath(ctx, path);
    CGContextStrokePath(ctx);
}

/* The space bar's open box, drawn as lines: the font's U+2423 is too thin and narrow to read at icon size. */
void draw_open_box(CGContextRef ctx, CGFloat cx, CGFloat cy, CGFloat h)
{
    const CGFloat a = h * OPEN_BOX_W / 2, b = h * OPEN_BOX_H / 2;
    CGContextSetRGBStrokeColor(ctx, INK[0], INK[1], INK[2], INK[3]);
    CGContextSetLineWidth(ctx, h * OPEN_BOX_LINE);
    CGContextSetLineCap(ctx, kCGLineCapRound);
    CGContextSetLineJoin(ctx, kCGLineJoinRound);
    CGContextMoveToPoint(ctx, cx - a, cy + b);
    CGContextAddLineToPoint(ctx, cx - a, cy - b);
    CGContextAddLineToPoint(ctx, cx + a, cy - b);
    CGContextAddLineToPoint(ctx, cx + a, cy + b);
    CGContextStrokePath(ctx);
}

void draw_key(CGContextRef ctx, const std::string& text, const Line& l, CGFloat w, CGFloat h)
{
    const CGFloat m = h * MARGIN;
    CGPathRef cap = CGPathCreateWithRoundedRect(CGRectMake(m, m, w - 2 * m, h - 2 * m), h * CORNER, h * CORNER, nullptr);
    fill_and_rim(ctx, cap, h * RIM_WIDTH);
    CGPathRelease(cap);
    if (text == OPEN_BOX)
        draw_open_box(ctx, w / 2, h / 2, h);
    else
        draw_line_at(ctx, l, w / 2, h / 2);
}

/* A mouse seen from above, its lit buttons orange; CG's y grows upward. */
void draw_mouse(CGContextRef ctx, const Glyph& g, CGFloat w, CGFloat h)
{
    const CGFloat bw = h * MOUSE_W, bh = h * MOUSE_H, x0 = (w - bw) / 2, y0 = (h - bh) / 2, top = y0 + bh;
    const CGFloat split = top - bh * MOUSE_SPLIT, cx = w / 2;
    const CGFloat r = bw * 0.45;
    CGPathRef body = CGPathCreateWithRoundedRect(CGRectMake(x0, y0, bw, bh), r, r, nullptr);
    CGContextSetRGBFillColor(ctx, FACE[0], FACE[1], FACE[2], FACE[3]);
    CGContextAddPath(ctx, body);
    CGContextFillPath(ctx);

    CGContextSaveGState(ctx);
    CGContextAddPath(ctx, body);
    CGContextClip(ctx);
    CGContextSetRGBFillColor(ctx, LIT[0], LIT[1], LIT[2], LIT[3]);
    if (g.mouse & PAD_MOUSE_LEFT) CGContextFillRect(ctx, CGRectMake(x0, split, bw / 2, top - split));
    if (g.mouse & PAD_MOUSE_RIGHT) CGContextFillRect(ctx, CGRectMake(cx, split, bw / 2, top - split));
    CGContextRestoreGState(ctx);

    CGContextSetRGBStrokeColor(ctx, RIM[0], RIM[1], RIM[2], RIM[3]);
    CGContextSetLineWidth(ctx, h * RIM_WIDTH);
    CGContextMoveToPoint(ctx, x0, split);
    CGContextAddLineToPoint(ctx, x0 + bw, split);
    CGContextMoveToPoint(ctx, cx, split);
    CGContextAddLineToPoint(ctx, cx, top);
    CGContextStrokePath(ctx);

    const CGFloat ww = h * WHEEL_W, wh = h * WHEEL_H;
    CGPathRef wheel = CGPathCreateWithRoundedRect(CGRectMake(cx - ww / 2, split + (top - split - wh) / 2, ww, wh),
                                                  ww * 0.45, ww * 0.45, nullptr);
    const bool lit = g.wheel || (g.mouse & PAD_MOUSE_MIDDLE);
    const CGFloat* wheel_fill = lit ? LIT : FACE;   /* filled either way: it covers the line between the buttons */
    CGContextSetRGBFillColor(ctx, wheel_fill[0], wheel_fill[1], wheel_fill[2], 1.0);
    CGContextAddPath(ctx, wheel);
    CGContextFillPath(ctx);
    CGContextAddPath(ctx, wheel);
    CGContextStrokePath(ctx);
    CGPathRelease(wheel);

    CGContextAddPath(ctx, body);
    CGContextStrokePath(ctx);
    CGPathRelease(body);

    if (!g.text.empty()) {   /* which way the wheel turns */
        Line mark;
        make_line(&mark, g.text, h * MARK_TEXT);
        draw_line_at(ctx, mark, cx, (y0 + split) / 2);
    }
}

}  // namespace

GlyphImage draw_glyph(const Glyph& g, int height, int max_width)
{
    GlyphImage img;
    if (height <= 0) return img;
    const CGFloat h = height;
    Line label;
    int width;
    if (g.kind == Glyph::KEY) {
        const CGFloat size = h * (is_symbol(g.text) ? KEY_SYMBOL_TEXT : KEY_TEXT);
        make_line(&label, g.text, size);
        width = std::max(height, (int)std::ceil(label.width + h * KEY_PADDING));
        if (max_width > 0 && width > max_width && label.width > 0) {
            const CGFloat room = max_width - h * KEY_PADDING_MIN;
            make_line(&label, g.text, size * room / label.width);
            width = max_width;
        }
    } else {
        width = (int)std::ceil(h * MOUSE_IMAGE_W);
    }
    width += width & 1;
    if (max_width > 0) width = std::min(width, max_width);

    std::vector<uint8_t> px((size_t)width * height * 4, 0);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    /* Premultiplied ARGB, bytes A R G B: the RSX's A8R8G8B8 once the alpha is divided out below. */
    CGContextRef ctx = CGBitmapContextCreate(px.data(), (size_t)width, (size_t)height, 8, (size_t)width * 4, cs,
                                             (uint32_t)kCGImageAlphaPremultipliedFirst | (uint32_t)kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(cs);
    if (!ctx) return img;
    CGContextSetShouldAntialias(ctx, true);
    if (g.kind == Glyph::KEY) draw_key(ctx, g.text, label, width, h);
    else draw_mouse(ctx, g, width, h);
    CGContextRelease(ctx);

    for (size_t i = 0; i < px.size(); i += 4) {
        const unsigned a = px[i];
        if (a == 0 || a == 255) continue;
        for (int c = 1; c < 4; c++) px[i + c] = (uint8_t)std::min(255u, (px[i + c] * 255u + a / 2) / a);
    }
    img.w = width;
    img.h = height;
    img.argb = std::move(px);
    return img;
}

#else

GlyphImage draw_glyph(const Glyph&, int, int) { return {}; }   /* TODO: draw the glyphs on hosts other than macOS */

#endif

IconPatcher::IconPatcher(const std::vector<PadBinding>& table)
{
    uint32_t next = GLYPH_LOCAL_BEGIN;
    for (int i = 0; i < ICON_COUNT; i++) {
        const std::optional<Glyph> g = icon_glyph(table, i);
        if (!g) continue;
        /* No wider than the game's icon: its button bars put the text a fixed distance after the icon. */
        GlyphImage img = draw_glyph(*g, SLOTS[i].h, SLOTS[i].w);
        if (img.argb.empty()) continue;
        const uint32_t size = (uint32_t)img.argb.size();
        if (next + size > GLYPH_LOCAL_END) break;
        Placed& p = placed_[i];
        p.used = true;
        p.offset = next;
        p.w = (uint16_t)img.w;
        p.h = (uint16_t)img.h;
        images_[i] = std::move(img.argb);
        next += (size + GLYPH_ALIGN - 1) / GLYPH_ALIGN * GLYPH_ALIGN;
    }
}

int IconPatcher::prepared() const
{
    int n = 0;
    for (const Placed& p : placed_) n += p.used;
    return n;
}

int IconPatcher::patch()
{
    if (!vm_base) return 0;
    if (!written_) {   /* the pixels go in once the guest memory is there; the host copy is not needed after that */
        for (int i = 0; i < ICON_COUNT; i++) {
            if (placed_[i].used)
                std::memcpy(vm_base + RSX_LOCAL_EA + placed_[i].offset, images_[i].data(), images_[i].size());
            std::vector<uint8_t>().swap(images_[i]);
        }
        written_ = true;
    }
    const uint32_t base = load_be32(ICON_TABLE), count = load_be32(ICON_TABLE + 4);
    if (base == 0 || count < (uint32_t)ICON_COUNT || base > RSX_LOCAL_EA - ICON_COUNT * ICON_ENTRY_SIZE) return 0;
    int changed = 0;
    for (int i = 0; i < ICON_COUNT; i++) {
        const Placed& p = placed_[i];
        if (!p.used) continue;
        const uint32_t e = base + (uint32_t)i * ICON_ENTRY_SIZE;
        const uint32_t format = load_be32(e + ENTRY_FORMAT), wh = load_be32(e + ENTRY_SIZE_WH);
        const uint32_t ours_wh = (uint32_t)p.w << 16 | p.h, game_wh = (uint32_t)SLOTS[i].w << 16 | SLOTS[i].h;
        /* The game's button icon, ours, or a mix of the two while the game is still writing the entry (it may write any
         * field after us); anything else in that place is not the table we know. */
        if ((format != ICON_FORMAT_DXT5 && format != ICON_FORMAT_ARGB) || (wh != game_wh && wh != ours_wh)) continue;
        const uint32_t pitch = load_be32(e + ENTRY_PITCH), ours_pitch = (pitch & ~PITCH_MASK) | (uint32_t)p.w * 4;
        if (format == ICON_FORMAT_ARGB && wh == ours_wh && pitch == ours_pitch && load_be32(e + ENTRY_OFFSET) == p.offset)
            continue;
        store_be32(e + ENTRY_OFFSET, p.offset);
        store_be32(e + ENTRY_SIZE_WH, ours_wh);
        store_be32(e + ENTRY_PITCH, ours_pitch);
        /* The format last, and seen after the rest by the PPU thread that reads the entry. */
        std::atomic_thread_fence(std::memory_order_release);
        store_be32(e + ENTRY_FORMAT, ICON_FORMAT_ARGB);
        changed++;
    }
    return changed;
}

}  // namespace r3

extern "C" void r3_glyphs_register(void)
{
    const char* e = std::getenv("R3_KEY_GLYPHS");
    if (e && e[0] == '0') return;
    const bool forced = e && e[0] == '1';
    if (std::getenv("PAD_NO_KEYBOARD") && !forced) return;
    const char* layout = std::getenv("PAD_KEYBOARD_LAYOUT");
    if (!layout || std::strcmp(layout, "fps") != 0) return;

    auto patcher = std::make_shared<r3::IconPatcher>(r3::controls_table());
    if (!patcher->prepared()) {
        std::printf("[r3] key glyphs: none drawn, the game's button icons stay\n");
        return;
    }
    std::printf("[r3] key glyphs: %d button icons drawn from the controls\n", patcher->prepared());
    /* The game fills its table at boot and may load the pack again later (a language change): keep watching. */
    std::thread([patcher] {
        for (;;) {
            if (const int n = patcher->patch()) std::printf("[r3] key glyphs: %d of the game's button icons now show keys\n", n);
            std::this_thread::sleep_for(POLL_PERIOD);
        }
    }).detach();
}
