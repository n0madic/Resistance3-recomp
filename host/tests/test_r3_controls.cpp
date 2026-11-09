/*
 * Test of host/r3_controls.cpp: the controls file's parser and writer, and its registration with cellPad (faked here:
 * the test provides the two cellPad calls the file makes, with a built-in table of its own).
 *
 *   cmake --build build/host --target test_r3_controls && build/host/test_r3_controls
 */
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <tuple>
#include <unistd.h>
#include <vector>

#include "../r3_controls.h"

#include "cellPad.h"

namespace {

/* The macOS key codes the table is made of (the same as the host's file knows). */
constexpr int K_A = 0, K_S = 1, K_D = 2, K_G = 5, K_C = 8, K_V = 9, K_Q = 12, K_W = 13, K_X = 7, K_RETURN = 36,
              K_SPACE = 49, K_BACKSPACE = 51, K_ESCAPE = 53, K_CTRL = 59;

std::vector<PadBinding> fake_defaults()
{
    return {
        {PAD_SRC_KEY, K_W, PAD_ACT_MOVE_UP}, {PAD_SRC_KEY, K_S, PAD_ACT_MOVE_DOWN},
        {PAD_SRC_KEY, K_A, PAD_ACT_MOVE_LEFT}, {PAD_SRC_KEY, K_D, PAD_ACT_MOVE_RIGHT},
        {PAD_SRC_KEY, K_SPACE, CELL_PAD_CTRL_CROSS},
        {PAD_SRC_KEY, K_C, CELL_PAD_CTRL_CIRCLE}, {PAD_SRC_KEY, K_CTRL, CELL_PAD_CTRL_CIRCLE},
        {PAD_SRC_KEY, K_BACKSPACE, CELL_PAD_CTRL_CIRCLE},
        {PAD_SRC_KEY, K_Q, CELL_PAD_CTRL_TRIANGLE}, {PAD_SRC_WHEEL, 0, CELL_PAD_CTRL_TRIANGLE},
        {PAD_SRC_KEY, K_G, CELL_PAD_CTRL_L2}, {PAD_SRC_KEY, K_V, CELL_PAD_CTRL_R3},
        {PAD_SRC_MOUSE, (int)PAD_MOUSE_LEFT, CELL_PAD_CTRL_R1}, {PAD_SRC_MOUSE, (int)PAD_MOUSE_RIGHT, CELL_PAD_CTRL_L1},
        {PAD_SRC_MOUSE, (int)PAD_MOUSE_MIDDLE, CELL_PAD_CTRL_R3},
        {PAD_SRC_KEY, K_RETURN, PAD_ACT_CONFIRM}, {PAD_SRC_KEY, K_ESCAPE, PAD_ACT_BACK},
    };
}

/* What cellPad would be given. */
int g_set_calls;
std::vector<PadBinding> g_set;

int g_checks, g_failed;

void check(bool ok, const char* what)
{
    g_checks++;
    if (ok) return;
    g_failed++;
    std::printf("FAIL: %s\n", what);
}

using Key = std::tuple<int, int, unsigned>;
std::vector<Key> keys_of(const std::vector<PadBinding>& t)
{
    std::vector<Key> v;
    for (const PadBinding& b : t) v.emplace_back(b.src, b.code, b.action);
    std::sort(v.begin(), v.end());
    return v;
}

std::vector<std::pair<int, int>> sources_of(const std::vector<PadBinding>& t, unsigned action)
{
    std::vector<std::pair<int, int>> v;
    for (const PadBinding& b : t) if (b.action == action) v.emplace_back(b.src, b.code);
    std::sort(v.begin(), v.end());
    return v;
}

using Src = std::pair<int, int>;
bool mentions(const std::vector<std::string>& problems, const char* text)
{
    return std::any_of(problems.begin(), problems.end(), [&](const std::string& p) { return p.find(text) != std::string::npos; });
}

void test_replace_and_keep()
{
    auto t = fake_defaults();
    std::vector<std::string> problems;
    const int n = r3::apply_controls("fire = F\ncrouch = X\n", &t, &problems);
    check(n == 2, "two actions listed");
    check(problems.empty(), "no problems");
    check(sources_of(t, CELL_PAD_CTRL_R1) == std::vector<Src>{{PAD_SRC_KEY, 3}}, "fire has only F");
    check(sources_of(t, CELL_PAD_CTRL_CIRCLE) == std::vector<Src>{{PAD_SRC_KEY, K_X}}, "crouch has only X");
    check(sources_of(t, CELL_PAD_CTRL_CROSS) == std::vector<Src>{{PAD_SRC_KEY, K_SPACE}}, "jump kept its key");
    check(sources_of(t, CELL_PAD_CTRL_L1).size() == 1, "aim kept its button");

    t = fake_defaults();
    r3::apply_controls("# nothing\n\n   \n", &t, &problems);
    check(keys_of(t) == keys_of(fake_defaults()), "comments and blanks change nothing");
}

void test_spelling()
{
    auto t = fake_defaults();
    std::vector<std::string> problems;
    const int n = r3::apply_controls("\xEF\xBB\xBF  CROSS =  enter , SPACE ,esc   # a comment\r\n"
                                     "Weapons=mouse3,WheelUp\r\n"
                                     "move_forward = Up ; ignored words are keys too\r\n", &t, &problems);
    check(n == 3, "three actions on lines with a BOM, CRLF, odd spacing and case");
    check(sources_of(t, CELL_PAD_CTRL_CROSS) == (std::vector<Src>{{PAD_SRC_KEY, K_RETURN}, {PAD_SRC_KEY, K_SPACE}, {PAD_SRC_KEY, K_ESCAPE}}),
          "cross (the pad's name for jump) takes Enter, Space and Esc");
    check(sources_of(t, CELL_PAD_CTRL_TRIANGLE) ==
              (std::vector<Src>{{PAD_SRC_MOUSE, (int)PAD_MOUSE_MIDDLE}, {PAD_SRC_WHEEL, 1}}),
          "weapons takes mouse3 and WheelUp");
    check(mentions(problems, "line 3: unknown key ';'"), "a stray word is named with its line");
}

void test_unbind_and_problems()
{
    auto t = fake_defaults();
    std::vector<std::string> problems;
    r3::apply_controls("melee =\n", &t, &problems);
    check(sources_of(t, CELL_PAD_CTRL_R3).empty(), "an empty list unbinds");
    check(problems.empty(), "...without a problem");

    t = fake_defaults();
    problems.clear();
    const int n = r3::apply_controls("fire = Banana, F\nfly = Q\nno equals sign\njump = Space\n", &t, &problems);
    check(n == 2, "the valid actions still count");
    check(mentions(problems, "line 1: unknown key 'Banana'"), "unknown key, line 1");
    check(mentions(problems, "line 2: unknown action 'fly'"), "unknown action, line 2");
    check(mentions(problems, "line 3: expected"), "a line without = , line 3");
    check(sources_of(t, CELL_PAD_CTRL_R1) == std::vector<Src>{{PAD_SRC_KEY, 3}}, "the good key of a line with a bad one is kept");

    t = fake_defaults();
    problems.clear();
    r3::apply_controls("fire = F\ncircle = G\ncrouch = X\nr1 = D\n", &t, &problems);
    check(mentions(problems, "line 3: 'crouch' is listed again (first on line 2)"), "an action named twice (by its pad name)");
    check(mentions(problems, "line 4: 'r1' is listed again (first on line 1)"), "...and by the other name");
    check(sources_of(t, CELL_PAD_CTRL_CIRCLE) == std::vector<Src>{{PAD_SRC_KEY, K_X}}, "the last line wins");
    check(sources_of(t, CELL_PAD_CTRL_R1) == std::vector<Src>{{PAD_SRC_KEY, K_D}}, "...for fire too");

    /* A line whose keys are all unknown is a slip, not a request to unbind: the action keeps its defaults. */
    t = fake_defaults();
    problems.clear();
    const int m = r3::apply_controls("jump = Spce\nfire = F\n", &t, &problems);
    check(m == 1, "a line with only unknown keys does not count as listed");
    check(mentions(problems, "line 1: none of the keys of 'jump' is known: it keeps its defaults"), "...and says so");
    check(sources_of(t, CELL_PAD_CTRL_CROSS) == std::vector<Src>{{PAD_SRC_KEY, K_SPACE}}, "...and jump is not left unbound");

    t = fake_defaults();
    problems.clear();
    r3::apply_controls("jump = Spce\njump = X\n", &t, &problems);
    check(!mentions(problems, "listed again"), "a skipped line does not make the next one a repeat");
    check(sources_of(t, CELL_PAD_CTRL_CROSS) == std::vector<Src>{{PAD_SRC_KEY, K_X}}, "...and the next one binds");
}

void test_key_names()
{
    auto t = fake_defaults();
    std::vector<std::string> problems;
    r3::apply_controls("crouch = Delete\njump = ForwardDelete\nuse = key110, KEY7\nmelee = key256, key\n", &t, &problems);
    check(sources_of(t, CELL_PAD_CTRL_CIRCLE) == std::vector<Src>{{PAD_SRC_KEY, K_BACKSPACE}},
          "Delete is the Mac's key of that name, which backs up");
    check(sources_of(t, CELL_PAD_CTRL_CROSS) == std::vector<Src>{{PAD_SRC_KEY, 117}}, "ForwardDelete is the other one");
    check(sources_of(t, CELL_PAD_CTRL_SQUARE) == (std::vector<Src>{{PAD_SRC_KEY, K_X}, {PAD_SRC_KEY, 110}}),
          "keyNN is a key by its code, in any case");
    check(mentions(problems, "unknown key 'key256'") && mentions(problems, "unknown key 'key'"),
          "a code past what a key has, or no code, is no key");
    check(sources_of(t, CELL_PAD_CTRL_R3).size() == 2, "...and melee, with nothing known, keeps its defaults");
}

void test_conflicts()
{
    auto t = fake_defaults();
    std::vector<std::string> problems;
    r3::apply_controls("jump = V\n", &t, &problems);   /* V is melee's */
    check(mentions(problems, "V is bound to both"), "a key on two actions is reported");
    check(sources_of(t, CELL_PAD_CTRL_CROSS) == std::vector<Src>{{PAD_SRC_KEY, K_V}}, "...and still bound");

    t = fake_defaults();
    problems.clear();
    r3::apply_controls("weapons = Wheel, WheelUp\n", &t, &problems);
    check(problems.empty(), "wheel entries overlapping each other are not conflicts");

    t = fake_defaults();
    problems.clear();
    r3::apply_controls("aim = WheelUp\n", &t, &problems);   /* weapons keeps Wheel, which is either way */
    check(mentions(problems, "Wheel is bound to both weapons and aim"), "WheelUp overlaps another action's Wheel");

    t = fake_defaults();
    problems.clear();
    r3::apply_controls("weapons = Q\naim = WheelUp\ngrenade = WheelDown\n", &t, &problems);
    check(problems.empty(), "WheelUp and WheelDown on two actions are two inputs");
}

void test_table_limit()
{
    std::vector<PadBinding> t;
    for (int i = 0; i < PAD_BINDINGS_MAX - 1; i++) t.push_back({PAD_SRC_KEY, 200 + i, CELL_PAD_CTRL_SQUARE});
    std::vector<std::string> problems;
    r3::apply_controls("fire = F\n", &t, &problems);         /* exactly PAD_BINDINGS_MAX */
    check(problems.empty(), "a table that fills cellPad's is not too big");
    r3::apply_controls("jump = Space\n", &t, &problems);     /* one more */
    check(mentions(problems, "too many bindings (129)"), "one past it is reported");
}

void test_render_roundtrip()
{
    const auto base = fake_defaults();
    const std::string text = r3::render_controls(base);
    check(text.find("move_forward") != std::string::npos && text.find("# fire (R1)") != std::string::npos, "the file names actions and what they do");
    check(text.find("fire         = MouseLeft") != std::string::npos, "fire reads as MouseLeft");

    std::vector<PadBinding> t;
    std::vector<std::string> problems;
    const int n = r3::apply_controls(text, &t, &problems);
    check(problems.empty(), "the rendered file reads without a problem");
    check(n == 22, "it lists every action");
    check(keys_of(t) == keys_of(base), "and says what the table did");

    auto odd = base;
    odd.push_back({PAD_SRC_KEY, 110, CELL_PAD_CTRL_SQUARE});   /* a key the file has no name for */
    const std::string odd_text = r3::render_controls(odd);
    check(odd_text.find("key110") != std::string::npos, "a key without a name is written by its code");
    std::vector<PadBinding> t2;
    problems.clear();
    r3::apply_controls(odd_text, &t2, &problems);
    check(problems.empty() && keys_of(t2) == keys_of(odd), "...and read back");
}

/* The registration: the file is made when missing and loaded when there. */
std::string read_all(const std::string& p) { std::ifstream f(p); std::ostringstream s; s << f.rdbuf(); return s.str(); }

void test_register()
{
    char dir[] = "/tmp/r3_controls_test.XXXXXX";
    check(mkdtemp(dir) != nullptr, "temp dir");
    const std::string path = std::string(dir) + "/controls.txt";
    setenv("R3_CONTROLS", path.c_str(), 1);

    unsetenv("PAD_NO_KEYBOARD");
    setenv("PAD_KEYBOARD_LAYOUT", "fps", 1);
    g_set_calls = 0;
    r3_controls_register();
    check(access(path.c_str(), F_OK) == 0, "a missing file is written");
    check(g_set_calls == 0, "...and the built-in table stays");
    check(read_all(path) == r3::render_controls(fake_defaults()), "...with the defaults");

    r3_controls_register();                                  /* the default file is read back: same table */
    check(g_set_calls == 1 && keys_of(g_set) == keys_of(fake_defaults()), "the default file gives the default table");

    { std::ofstream f(path); f << "fire = MouseLeft, F\nstart = P\n"; }
    r3_controls_register();
    check(g_set_calls == 2, "an edited file is given to cellPad");
    check(sources_of(g_set, CELL_PAD_CTRL_R1).size() == 2, "fire has two keys");
    check(sources_of(g_set, CELL_PAD_CTRL_START) == std::vector<Src>{{PAD_SRC_KEY, 35}}, "start has P");
    check(sources_of(g_set, CELL_PAD_CTRL_CROSS).size() == 1, "the rest is as before");

    /* A file that is there but cannot be read is left alone, not written over with the defaults. */
    const std::string edited = read_all(path);
    chmod(path.c_str(), 0200);
    if (access(path.c_str(), R_OK) != 0) {                   /* (as root it would read anyway) */
        const int calls = g_set_calls;
        r3_controls_register();
        check(g_set_calls == calls, "an unreadable file gives cellPad nothing");
        chmod(path.c_str(), 0600);
        check(read_all(path) == edited, "...and is not written over");
    }
    chmod(path.c_str(), 0600);

    setenv("PAD_KEYBOARD_LAYOUT", "pad", 1);                 /* the pad layout does not read the table */
    std::remove(path.c_str());
    r3_controls_register();
    check(access(path.c_str(), F_OK) != 0 && g_set_calls == 2, "another layout leaves it alone");
    setenv("PAD_KEYBOARD_LAYOUT", "fps", 1);

    setenv("PAD_NO_KEYBOARD", "1", 1);                       /* a scripted run: not touched */
    std::remove(path.c_str());
    r3_controls_register();
    check(access(path.c_str(), F_OK) != 0 && g_set_calls == 2, "PAD_NO_KEYBOARD leaves it alone");
    unsetenv("PAD_NO_KEYBOARD");
    rmdir(dir);
}

}  // namespace

extern "C" {
int cellPad_default_key_bindings(PadBinding* out, int max)
{
    const auto t = fake_defaults();
    for (int i = 0; out && i < max && i < (int)t.size(); i++) out[i] = t[(size_t)i];
    return (int)t.size();
}
void cellPad_set_key_bindings(const PadBinding* b, int n)
{
    g_set_calls++;
    g_set.assign(b, b + n);
}
}

int main()
{
    test_replace_and_keep();
    test_spelling();
    test_unbind_and_problems();
    test_key_names();
    test_conflicts();
    test_table_limit();
    test_render_roundtrip();
    test_register();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
