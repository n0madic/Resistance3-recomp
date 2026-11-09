/*
 * Resistance 3 -- controls.txt: the keyboard and mouse bindings the player can change.
 *
 * cellPad's fps layout (patches/input/0008) reads its keys, mouse buttons and wheel through a table of bindings; this
 * is the file that edits it. One line per action, "action = key, key, ...": an action the file lists gets exactly those
 * keys in place of its defaults (an empty list unbinds it), one it leaves out keeps its defaults. The idea is the
 * Resistance 2 port's controls.txt (MIT, github.com/miroslavdanutalexandru-alt/Resistance2Port); the format is ours.
 */
#ifndef R3_CONTROLS_H
#define R3_CONTROLS_H

#include <string>
#include <vector>

#include "pad_keymap.h"

namespace r3 {

/* Applies the file's text to `table`: each action it lists has its bindings replaced by the listed ones. What is wrong
 * with the text ("line 12: unknown key 'foo'") goes to `problems`, and the rest of it still counts. Returns the number
 * of actions the text listed. */
int apply_controls(const std::string& text, std::vector<PadBinding>* table, std::vector<std::string>* problems);

/* The text of a controls file that says what `table` does, with each action's meaning beside it. */
std::string render_controls(const std::vector<PadBinding>& table);

/* The name the file uses for a key code ("Space", "Shift", ...; "keyNN" for one without a name). */
std::string key_name(int code);

/* The table cellPad was given by r3_controls_register, or cellPad's built-in one when it gave none. */
std::vector<PadBinding> controls_table();

}  // namespace r3

/* Reads the controls file (R3_CONTROLS, else controls.txt in the working directory) and gives cellPad the table; writes
 * the default file when there is none. Does nothing in a scripted run (PAD_NO_KEYBOARD). */
extern "C" void r3_controls_register(void);

#endif /* R3_CONTROLS_H */
