/* pc_input.h — JSON-configurable input bindings (keyboard now; gamepad later).
 *
 * Maps physical keys/combos to logical game ACTIONS via benefactor.json "bind_*"
 * string values, e.g.:
 *
 *   "bind_drop":     "X+Down, C, RShift",
 *   "bind_hop":      "Up, B",
 *   "bind_interact": "X, LShift"
 *
 * A binding value is a comma-separated list of CHORDS; a chord is one or more keys
 * joined by '+' that must all be held. Any chord satisfied → the action is active.
 * Key names: Up/Down/Left/Right, Space, Return/Enter, LShift/RShift, LCtrl/Ctrl/RCtrl,
 * Tab, single letters/digits (X, C, Z, 1...). Defaults reproduce the built-in layout.
 *
 * hw_handle_key feeds raw key events here; hw derives the engine input (joystick/fire/
 * interact/drop) from the resolved actions. A gamepad layer later just feeds the same
 * pc_input_key()/actions — bindings and translation are device-agnostic. */
#pragma once

enum {
    PI_LEFT = 0, PI_RIGHT, PI_UP, PI_DOWN,
    PI_HOP, PI_FIRE, PI_INTERACT, PI_DROP,
    PI_NUM
};

void pc_input_load(void);             /* parse bind_* from config (idempotent; defaults otherwise) */
void pc_input_key(int sym, int down); /* raw SDL keysym up/down */
int  pc_input_active(int action);     /* 1 if any binding for the action is fully held */
void pc_input_release_all(void);      /* clear all held keys (focus loss) */
