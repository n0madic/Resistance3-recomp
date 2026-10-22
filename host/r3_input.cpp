/*
 * Resistance 3 -- what the keyboard layout needs to know about the game (cellPad's fps layout, patches/input).
 *
 * Enter and Esc confirm and go back in a menu but press START in the game (cellPad_set_menu_query). The game keeps
 * two flags for it in its front-end state (EU BCES01118 addresses), found by diffing memory between screens and
 * confirmed by logging them through boot, menus, the intro movie, a cutscene, gameplay and the pause menu:
 *   R3_UI_MENU_FLAG  (byte) 1 while a menu is up: main and campaign menus, the pause menu; 0 in the game,
 *                    movies and cutscenes.
 *   R3_PRESS_START_FLAG (byte) 1 from boot to the "Press START" screen, which takes START only; the menu flag is
 *                    already 1 there.
 *
 * The mouse turns the view directly (cellPad_set_mouse_look) instead of through the right stick, which the game
 * caps at about 206 degrees/s across and 60 up and down. The view is the hero's orientation matrix: rows right, up
 * and forward (3 big-endian floats, 16 bytes apart) at R3_HERO_ROWS in the hero object, found through the camera
 * matrix (R3_CAMERA) that is copied from it. The game keeps no yaw or pitch angles beside it: it turns the matrix
 * by the stick, writes it back each frame and goes on from whatever is there, so turning the rows turns the view,
 * up and down too. The game clamps its own pitch at about 81 degrees. The look is taken only while the matrix is
 * a sane rotation and the camera sits at the hero's eyes (not in a cutscene or a menu); otherwise the stick does it.
 */

#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>

extern "C" {
extern uint8_t* vm_base;
void cellPad_set_menu_query(int (*in_menu)(void));
void cellPad_set_mouse_look(int (*look)(double yaw_deg, double pitch_deg));
}

namespace {

constexpr uint32_t R3_UI_MENU_FLAG     = 0x00F09020u;
constexpr uint32_t R3_PRESS_START_FLAG = 0x00F08EBCu;

constexpr uint32_t R3_HERO_PTR   = 0x0112336Cu;   /* the player's hero object */
constexpr uint32_t R3_HERO_ROWS  = 0x370u;        /* right, up, forward rows in it */
constexpr uint32_t R3_ROW_STRIDE = 0x10u;
constexpr uint32_t R3_CAMERA     = 0x0105B4A0u;   /* camera matrix: right, up, forward, position rows */
constexpr uint32_t R3_HERO_FEET  = 0x01069950u;   /* copy of the hero's feet position */

constexpr double R3_PITCH_LIMIT_DEG = 80.0;       /* inside the game's own clamp */
/* How far the matrix may be from a rotation (unit rows, right angles, upright, no roll). */
constexpr double R3_ROW_TOLERANCE   = 0.02;
/* The camera is at the hero's eyes: this close across and this far above the feet (crouching to standing)... */
constexpr double R3_EYE_MAX_ACROSS  = 1.5;
constexpr double R3_EYE_MIN_ABOVE   = -0.5;
constexpr double R3_EYE_MAX_ABOVE   = 2.5;
/* ...and looks the hero's way, unless the hero's forward is still the one written here last: the pad is polled
 * several times a frame, and the camera takes up the turn only on the next one. */
constexpr double R3_EYE_MIN_COS     = 0.5;

struct Vec3 { double x, y, z; };

Vec3 s_written_f;   /* the forward row r3_mouse_look stored last, as stored (pad thread only) */

double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 add(const Vec3& a, const Vec3& b, double k) { return { a.x + b.x * k, a.y + b.y * k, a.z + b.z * k }; }
Vec3 scale(const Vec3& a, double k) { return { a.x * k, a.y * k, a.z * k }; }
Vec3 normalized(const Vec3& a) { return scale(a, 1.0 / std::sqrt(dot(a, a))); }

uint32_t load_be32(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

void store_be32(uint32_t ea, uint32_t v)
{
    uint8_t* p = vm_base + ea;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

Vec3 load_vec3(uint32_t ea)
{
    float f[3];
    for (int i = 0; i < 3; i++) {
        const uint32_t w = load_be32(ea + 4 * i);
        std::memcpy(&f[i], &w, sizeof w);
    }
    return { f[0], f[1], f[2] };
}

void store_vec3(uint32_t ea, const Vec3& v)
{
    const float f[3] = { (float)v.x, (float)v.y, (float)v.z };
    for (int i = 0; i < 3; i++) {
        uint32_t w;
        std::memcpy(&w, &f[i], sizeof w);
        store_be32(ea + 4 * i, w);
    }
}

int r3_in_menu(void)
{
    return vm_base[R3_UI_MENU_FLAG] != 0 && vm_base[R3_PRESS_START_FLAG] == 0;
}

/* Reads the hero's matrix; returns where its rows are, or 0 when the view is not the hero's to turn now. */
uint32_t r3_look_rows(Vec3* r, Vec3* u, Vec3* f)
{
    if (r3_in_menu()) return 0;
    const uint32_t hero = load_be32(R3_HERO_PTR);
    if (!hero || hero % 16 || hero > UINT32_MAX - (R3_HERO_ROWS + 3 * R3_ROW_STRIDE)) return 0;
    const uint32_t rows = hero + R3_HERO_ROWS;
    *r = load_vec3(rows);
    *u = load_vec3(rows + R3_ROW_STRIDE);
    *f = load_vec3(rows + 2 * R3_ROW_STRIDE);
    const double t = R3_ROW_TOLERANCE;
    for (const Vec3* v : { r, u, f })
        if (!(std::fabs(dot(*v, *v) - 1.0) < t)) return 0;   /* also rejects NaN */
    if (std::fabs(dot(*r, *u)) > t || std::fabs(dot(*r, *f)) > t || std::fabs(dot(*u, *f)) > t) return 0;
    if (u->y <= 0 || std::fabs(r->y) > t) return 0;

    const Vec3 cam_f = load_vec3(R3_CAMERA + 2 * R3_ROW_STRIDE);
    const Vec3 cam_p = load_vec3(R3_CAMERA + 3 * R3_ROW_STRIDE);
    const Vec3 feet  = load_vec3(R3_HERO_FEET);
    const double above = cam_p.y - feet.y;
    if (std::hypot(cam_p.x - feet.x, cam_p.z - feet.z) > R3_EYE_MAX_ACROSS) return 0;
    if (!(above >= R3_EYE_MIN_ABOVE && above <= R3_EYE_MAX_ABOVE)) return 0;
    const bool ours = f->x == s_written_f.x && f->y == s_written_f.y && f->z == s_written_f.z;
    if (!ours && !(dot(cam_f, *f) >= R3_EYE_MIN_COS)) return 0;
    return rows;
}

/* Turns the hero: yaw about the world's up (the game's yaw is atan2(f.x, f.z) and turning right lowers it), then
 * pitch within the hero's forward/up plane, clamped. */
int r3_mouse_look(double yaw_deg, double pitch_deg)
{
    Vec3 r, u, f;
    const uint32_t rows = r3_look_rows(&r, &u, &f);
    if (!rows) return 0;

    const double yaw = -yaw_deg * M_PI / 180.0, cy = std::cos(yaw), sy = std::sin(yaw);
    for (Vec3* v : { &r, &u, &f }) *v = { v->x * cy + v->z * sy, v->y, v->z * cy - v->x * sy };

    const double limit = R3_PITCH_LIMIT_DEG * M_PI / 180.0;
    const double pitch = std::asin(std::fmax(-1.0, std::fmin(1.0, f.y)));
    const double to = std::fmax(-limit, std::fmin(limit, pitch + pitch_deg * M_PI / 180.0));
    const double d = to - pitch, cp = std::cos(d), sp = std::sin(d);
    const Vec3 f2 = add(scale(f, cp), u, sp);
    const Vec3 u2 = add(scale(u, cp), f, -sp);

    /* Keep it a rotation: float rounding would otherwise pile up over many small turns. */
    f = normalized(f2);
    r = normalized(add(r, f, -dot(r, f)));
    u = normalized(add(add(u2, f, -dot(u2, f)), r, -dot(u2, r)));
    store_vec3(rows, r);
    store_vec3(rows + R3_ROW_STRIDE, u);
    store_vec3(rows + 2 * R3_ROW_STRIDE, f);
    s_written_f = load_vec3(rows + 2 * R3_ROW_STRIDE);
    return 1;
}

}  // namespace

extern "C" void r3_input_register(void)
{
    cellPad_set_menu_query(r3_in_menu);
    cellPad_set_mouse_look(r3_mouse_look);
}
