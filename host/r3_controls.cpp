/*
 * Resistance 3 -- controls.txt (see r3_controls.h).
 *
 * Key names are the macOS virtual key codes (HIToolbox kVK_*), which are what cellPad's pad_key_down takes on this host.
 * Command is left out on purpose: Cmd+Q and the other app shortcuts are the system's.
 */
#include "r3_controls.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>

#include "cellPad.h"   /* CELL_PAD_CTRL_* */

namespace {

struct KeyName { const char* name; int code; };

constexpr KeyName KEYS[] = {
    {"A", 0}, {"S", 1}, {"D", 2}, {"F", 3}, {"H", 4}, {"G", 5}, {"Z", 6}, {"X", 7}, {"C", 8}, {"V", 9}, {"B", 11},
    {"Q", 12}, {"W", 13}, {"E", 14}, {"R", 15}, {"Y", 16}, {"T", 17}, {"1", 18}, {"2", 19}, {"3", 20}, {"4", 21},
    {"6", 22}, {"5", 23}, {"Equals", 24}, {"9", 25}, {"7", 26}, {"Minus", 27}, {"8", 28}, {"0", 29},
    {"RightBracket", 30}, {"O", 31}, {"U", 32}, {"LeftBracket", 33}, {"I", 34}, {"P", 35}, {"Return", 36}, {"L", 37},
    {"J", 38}, {"Quote", 39}, {"K", 40}, {"Semicolon", 41}, {"Backslash", 42}, {"Comma", 43}, {"Slash", 44},
    {"N", 45}, {"M", 46}, {"Period", 47}, {"Tab", 48}, {"Space", 49}, {"Grave", 50}, {"Backspace", 51},
    {"Escape", 53}, {"Shift", 56}, {"CapsLock", 57}, {"Alt", 58}, {"Ctrl", 59}, {"RightShift", 60},
    {"RightAlt", 61}, {"RightCtrl", 62},
    {"F5", 96}, {"F6", 97}, {"F7", 98}, {"F3", 99}, {"F8", 100}, {"F9", 101}, {"F11", 103}, {"F10", 109},
    {"F12", 111}, {"F4", 118}, {"F2", 120}, {"F1", 122},
    {"Home", 115}, {"PageUp", 116}, {"ForwardDelete", 117}, {"End", 119}, {"PageDown", 121},
    {"Left", 123}, {"Right", 124}, {"Down", 125}, {"Up", 126},
};

/* A key without a name above is spelled keyNN (its code); source_name writes it so and parse_raw_key reads it. */
constexpr int RAW_KEY_MAX = 255;

/* Other spellings of the names above. "Delete" is the Mac's key of that name, the one that backs up (Apple's kVK_Delete). */
constexpr const char* KEY_ALIASES[][2] = {
    {"enter", "Return"}, {"esc", "Escape"}, {"control", "Ctrl"}, {"option", "Alt"}, {"spacebar", "Space"},
    {"delete", "Backspace"}, {"del", "ForwardDelete"},
};

struct Source { int src, code; };

/* What r3_controls_register gave cellPad (none: it gave nothing, cellPad keeps its own; an empty table unbinds all). */
std::optional<std::vector<PadBinding>> g_applied;

struct PointerName { const char* name; Source s; };
constexpr PointerName POINTERS[] = {
    {"MouseLeft", {PAD_SRC_MOUSE, (int)PAD_MOUSE_LEFT}},
    {"MouseRight", {PAD_SRC_MOUSE, (int)PAD_MOUSE_RIGHT}},
    {"MouseMiddle", {PAD_SRC_MOUSE, (int)PAD_MOUSE_MIDDLE}},
    {"WheelUp", {PAD_SRC_WHEEL, 1}},      /* away from the user */
    {"WheelDown", {PAD_SRC_WHEEL, -1}},
    {"Wheel", {PAD_SRC_WHEEL, 0}},        /* either way */
};
constexpr const char* POINTER_ALIASES[][2] = {
    {"mouse1", "MouseLeft"}, {"mouse2", "MouseRight"}, {"mouse3", "MouseMiddle"},
};

struct Action {
    const char* name;
    const char* alias;   /* the pad's name for it, also accepted */
    unsigned    bits;
    const char* note;    /* what it does in Resistance 3 */
};

constexpr size_t NOTE_COLUMN = 52;   /* where the note beside an action starts in the written file */

/* The order of the file. */
constexpr Action ACTIONS[] = {
    {"move_forward", nullptr, PAD_ACT_MOVE_UP, "walk forward (the left stick)"},
    {"move_back", nullptr, PAD_ACT_MOVE_DOWN, "walk back"},
    {"move_left", nullptr, PAD_ACT_MOVE_LEFT, "strafe left"},
    {"move_right", nullptr, PAD_ACT_MOVE_RIGHT, "strafe right"},
    {"jump", "cross", CELL_PAD_CTRL_CROSS, "jump (cross)"},
    {"crouch", "circle", CELL_PAD_CTRL_CIRCLE, "crouch, and back out of a screen (circle)"},
    {"use", "square", CELL_PAD_CTRL_SQUARE, "reload, use, pick up (square)"},
    {"weapons", "triangle", CELL_PAD_CTRL_TRIANGLE,
     "triangle: a tap swaps to the previous weapon, holding it opens the weapon wheel (the move keys pick)"},
    {"fire", "r1", CELL_PAD_CTRL_R1, "fire (R1)"},
    {"aim", "l1", CELL_PAD_CTRL_L1, "aim down the sights (L1)"},
    {"grenade", "l2", CELL_PAD_CTRL_L2, "grenade (L2)"},
    {"alt_fire", "r2", CELL_PAD_CTRL_R2, "secondary fire (R2)"},
    {"sprint", "l3", CELL_PAD_CTRL_L3, "sprint (L3)"},
    {"melee", "r3", CELL_PAD_CTRL_R3, "melee (R3)"},
    {"dpad_up", nullptr, CELL_PAD_CTRL_UP, "d-pad up"},
    {"dpad_right", nullptr, CELL_PAD_CTRL_RIGHT, "d-pad right"},
    {"dpad_down", nullptr, CELL_PAD_CTRL_DOWN, "d-pad down"},
    {"dpad_left", nullptr, CELL_PAD_CTRL_LEFT, "d-pad left"},
    {"select", nullptr, CELL_PAD_CTRL_SELECT, "select"},
    {"start", nullptr, CELL_PAD_CTRL_START, "start, always (pause, skip a cutscene)"},
    {"menu_confirm", nullptr, PAD_ACT_CONFIRM, "cross in a menu, start otherwise"},
    {"menu_back", nullptr, PAD_ACT_BACK, "circle in a menu, start otherwise"},
};

std::string lower(std::string s)
{
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

/* "key110": a key by its code (lower case already). */
bool parse_raw_key(const std::string& t, Source* out)
{
    if (t.size() < 4 || t.size() > 6 || t.compare(0, 3, "key") != 0 || t.find_first_not_of("0123456789", 3) != std::string::npos)
        return false;
    const int code = std::atoi(t.c_str() + 3);
    if (code > RAW_KEY_MAX) return false;
    *out = {PAD_SRC_KEY, code};
    return true;
}

bool parse_source(const std::string& token, Source* out)
{
    std::string t = lower(token);
    for (const auto& a : KEY_ALIASES) if (t == a[0]) t = lower(a[1]);
    for (const auto& a : POINTER_ALIASES) if (t == a[0]) t = lower(a[1]);
    for (const KeyName& k : KEYS)
        if (t == lower(k.name)) { *out = {PAD_SRC_KEY, k.code}; return true; }
    for (const PointerName& p : POINTERS)
        if (t == lower(p.name)) { *out = p.s; return true; }
    return parse_raw_key(t, out);
}

std::string source_name(const PadBinding& b)
{
    if (b.src == PAD_SRC_KEY) return r3::key_name(b.code);
    for (const PointerName& p : POINTERS) if (p.s.src == b.src && p.s.code == b.code) return p.name;
    return "?";
}

const Action* find_action(const std::string& name)
{
    const std::string n = lower(name);
    for (const Action& a : ACTIONS)
        if (n == a.name || (a.alias && n == a.alias)) return &a;
    return nullptr;
}

const Action* action_of(unsigned bits)
{
    for (const Action& a : ACTIONS) if (a.bits == bits) return &a;
    return nullptr;
}

/* Two sources are the same input when they are the same key or button, or when one is the wheel either way and the other
 * is the wheel. */
bool same_source(const PadBinding& a, const PadBinding& b)
{
    if (a.src != b.src) return false;
    return a.code == b.code || (a.src == PAD_SRC_WHEEL && (a.code == 0 || b.code == 0));
}

/* What the file says about one action: its keys, and the line that says it. */
struct Listing { std::vector<Source> keys; int line; };

std::vector<std::string> split_list(const std::string& s)
{
    std::vector<std::string> out;
    std::string cur;
    for (const char c : s) {
        if (c == ',' || std::isspace((unsigned char)c)) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

}  // namespace

namespace r3 {

int apply_controls(const std::string& text_in, std::vector<PadBinding>* table, std::vector<std::string>* problems)
{
    std::string text = text_in;
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);   /* a byte order mark */

    std::map<unsigned, Listing> listed;   /* action -> what the file lists for it */
    std::istringstream in(text);
    std::string line;
    int no = 0;
    auto complain = [&](int at, const std::string& what) {
        if (problems) problems->push_back("line " + std::to_string(at) + ": " + what);
    };
    while (std::getline(in, line)) {
        no++;
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        line = trim(line);
        if (line.empty()) continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) { complain(no, "expected \"action = key, key\""); continue; }
        const std::string name = trim(line.substr(0, eq));
        const Action* a = find_action(name);
        if (!a) { complain(no, "unknown action '" + name + "'"); continue; }
        const std::vector<std::string> tokens = split_list(line.substr(eq + 1));
        std::vector<Source> keys;
        for (const std::string& tok : tokens) {
            Source s;
            if (!parse_source(tok, &s)) { complain(no, "unknown key '" + tok + "'"); continue; }
            if (std::none_of(keys.begin(), keys.end(), [&](const Source& k) { return k.src == s.src && k.code == s.code; }))
                keys.push_back(s);
        }
        /* Only keys it does not know: a slip, not a wish to unbind (an empty list says that). The action keeps its own. */
        if (!tokens.empty() && keys.empty()) {
            complain(no, "none of the keys of '" + name + "' is known: it keeps its defaults");
            continue;
        }
        const auto before = listed.find(a->bits);
        if (before != listed.end())
            complain(no, "'" + name + "' is listed again (first on line " + std::to_string(before->second.line) +
                         "): this line wins");
        listed[a->bits] = { keys, no };
    }

    for (const auto& [bits, listing] : listed) {
        table->erase(std::remove_if(table->begin(), table->end(), [&](const PadBinding& b) { return b.action == bits; }),
                     table->end());
        for (const Source& s : listing.keys) table->push_back({s.src, s.code, bits});
    }

    if (problems) {
        /* A key on two actions is allowed (sprint and crouch on one key is a choice) but is more often a slip. */
        if (!listed.empty()) {
            for (size_t i = 0; i < table->size(); i++) {
                const PadBinding& b = (*table)[i];
                for (size_t j = i + 1; j < table->size(); j++) {
                    const PadBinding& c = (*table)[j];
                    if (!same_source(b, c) || c.action == b.action) continue;
                    const Action* x = action_of(b.action);
                    const Action* y = action_of(c.action);
                    problems->push_back(source_name(b) + " is bound to both " + (x ? x->name : "?") + " and " +
                                        (y ? y->name : "?"));
                }
            }
        }
        /* cellPad keeps the first PAD_BINDINGS_MAX; the actions applied last (the menu keys) are the ones that go. */
        if (table->size() > (size_t)PAD_BINDINGS_MAX)
            problems->push_back("too many bindings (" + std::to_string(table->size()) + "): cellPad takes the first " +
                                std::to_string(PAD_BINDINGS_MAX) + ", the rest do nothing");
    }
    return (int)listed.size();
}

std::string render_controls(const std::vector<PadBinding>& table)
{
    std::ostringstream out;
    out << "# Resistance 3: keyboard and mouse controls.\n"
           "#\n"
           "# One line per action: \"action = key, key, ...\". An action listed here gets exactly these keys in place of\n"
           "# its defaults (an empty list unbinds it); an action left out keeps its defaults. '#' starts a comment.\n"
           "# The mouse turns the view; its speed is PAD_MOUSE_SENS, and the game's Options > Controls (Camera Speed,\n"
           "# Invert-X/Y) apply to it.\n"
           "#\n"
           "# Keys: A-Z, 0-9, F1-F12, Space, Return, Escape, Tab, Backspace (also Delete), Shift, Ctrl, Alt, CapsLock,\n"
           "#       Home, End, PageUp, PageDown, ForwardDelete, Up, Down, Left, Right, Minus, Equals, Comma, Period,\n"
           "#       Slash, Semicolon, Quote, LeftBracket, RightBracket, Backslash, Grave, RightShift, RightCtrl,\n"
           "#       RightAlt, keyNN (a macOS virtual key code, for a key with no name here);\n"
           "#       MouseLeft, MouseRight, MouseMiddle; WheelUp, WheelDown or Wheel (either way).\n"
           "# Actions can also be named after the pad's buttons: cross, circle, square, triangle, r1, l1, l2, r2, l3, r3.\n"
           "# Written by the host when it is missing; delete it to get this back.\n\n";
    size_t width = 0;
    for (const Action& a : ACTIONS) width = std::max(width, std::strlen(a.name));
    for (const Action& a : ACTIONS) {
        std::string keys;
        for (const PadBinding& b : table) {
            if (b.action != a.bits) continue;
            keys += keys.empty() ? "" : ", ";
            keys += source_name(b);
        }
        std::string left = std::string(a.name) + std::string(width - std::strlen(a.name), ' ') + " = " + keys;
        out << left << std::string(left.size() < NOTE_COLUMN ? NOTE_COLUMN - left.size() : 1, ' ') << "# " << a.note << "\n";
    }
    return out.str();
}

std::string key_name(int code)
{
    for (const KeyName& k : KEYS) if (k.code == code) return k.name;
    return "key" + std::to_string(code);   /* parse_raw_key reads it back (up to RAW_KEY_MAX) */
}

std::vector<PadBinding> controls_table()
{
    if (g_applied) return *g_applied;
    std::vector<PadBinding> table(PAD_BINDINGS_MAX);
    table.resize((size_t)std::min(cellPad_default_key_bindings(table.data(), PAD_BINDINGS_MAX), PAD_BINDINGS_MAX));
    return table;
}

}  // namespace r3

extern "C" void r3_controls_register(void)
{
    if (std::getenv("PAD_NO_KEYBOARD")) return;
    /* Only cellPad's fps layout reads the table (the host makes it the default); say so rather than write a file that does nothing. */
    const char* layout = std::getenv("PAD_KEYBOARD_LAYOUT");
    if (!layout || std::strcmp(layout, "fps") != 0) {
        std::printf("[r3] controls.txt is for PAD_KEYBOARD_LAYOUT=fps: not used\n");
        return;
    }
    const char* e = std::getenv("R3_CONTROLS");
    const std::string path = e && *e ? e : "controls.txt";

    std::vector<PadBinding> table(PAD_BINDINGS_MAX);
    table.resize((size_t)std::min(cellPad_default_key_bindings(table.data(), PAD_BINDINGS_MAX), PAD_BINDINGS_MAX));

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        std::ofstream out(path);
        if (out && (out << r3::render_controls(table)))
            std::printf("[r3] wrote %s: the keyboard and mouse controls, edit it to remap\n", path.c_str());
        else
            std::printf("[r3] %s is missing and cannot be written: default controls\n", path.c_str());
        return;
    }
    std::ifstream in(path);
    if (!in) {   /* there, but not ours to read: leave it, do not write over it */
        std::printf("[r3] %s cannot be read: default controls\n", path.c_str());
        return;
    }
    std::ostringstream text;
    text << in.rdbuf();
    std::vector<std::string> problems;
    const int changed = r3::apply_controls(text.str(), &table, &problems);
    for (const std::string& p : problems) std::printf("[r3] %s: %s\n", path.c_str(), p.c_str());
    cellPad_set_key_bindings(table.data(), (int)table.size());
    g_applied = table;
    std::printf("[r3] %s: %d action(s) listed, %zu binding(s)\n", path.c_str(), changed, table.size());
}
