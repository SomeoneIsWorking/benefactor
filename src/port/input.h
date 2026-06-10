/* pc_input.h — JSON-configurable input bindings (keyboard + game controller).
 *
 * Maps physical keys/buttons to logical game ACTIONS via benefactor.json string
 * values. Keyboard keys live under "bind_*", controller buttons under "pad_*":
 *
 *   "bind_drop":     "X+Down, C, RShift",
 *   "bind_hop":      "Up, B",
 *   "pad_fire":      "B, RightTrigger"
 *
 * A binding value is a comma-separated list of CHORDS; a chord is one or more keys
 * joined by '+' that must all be held. Any chord satisfied → the action is active.
 * Keyboard names: Up/Down/Left/Right, Space, Return/Enter, LShift/RShift,
 * LCtrl/Ctrl/RCtrl, Tab, single letters/digits, plus any SDL key name
 * (SDL_GetKeyFromName). Controller names: A/B/X/Y, DPUp/DPDown/DPLeft/DPRight,
 * Start/Back, LB/RB (shoulders), LeftTrigger/RightTrigger, and analog-stick
 * directions LeftX-/LeftX+/LeftY-/LeftY+ (RightX/RightY likewise).
 *
 * The two devices are tracked separately (pc_input_active_dev) so the modern
 * control scheme can be enabled per device; pc_input_active ORs both. */
#pragma once

enum {
    PI_LEFT = 0, PI_RIGHT, PI_UP, PI_DOWN,
    PI_HOP, PI_FIRE, PI_INTERACT, PI_DROP,
    PI_FFWD,                              /* hold-to-fast-forward (5x) */
    PI_FREECAM,                           /* toggle the detachable free camera */
    PI_NUM
};

enum { PI_DEV_KB = 0, PI_DEV_PAD = 1, PI_NUM_DEV = 2 };

void pc_input_load(void);             /* parse bind_ + pad_ keys from config (idempotent) */
void pc_input_reload(void);           /* force re-parse (after a rebind/persist) */
void pc_input_key(int sym, int down); /* raw SDL keysym up/down (keyboard device) */
int  pc_input_active(int action);     /* 1 if held on ANY device */
int  pc_input_active_dev(int dev, int action);  /* 1 if held on this device */
void pc_input_release_all(void);      /* clear all held keys/buttons (focus loss) */

/* ── Controller channel ─────────────────────────────────────────────────────
 * A pad "code" is an SDL_GameControllerButton (0..N), or for analog directions
 * 100 + axis*2 + (1 if positive direction). hw.c translates SDL controller
 * events (incl. axis threshold crossings) into these codes. */
#define PI_PAD_AXIS_CODE(axis, positive) (100 + (axis) * 2 + ((positive) ? 1 : 0))
void pc_input_pad_button(int code, int down);
void pc_input_pad_clear(void);        /* controller unplugged: release everything */

/* ── Rebinding / menu support ──────────────────────────────────────────────── */
const char *pc_input_action_name(int action);            /* "LEFT", "FIRE", ... */
/* Current binding as a display string ("Z, LCtrl"); returns buf. */
const char *pc_input_binding_str(int dev, int action, char *buf, int cap);
/* Replace the action's binding with a single key/button and persist it to the
 * config file. dev=PI_DEV_KB: code is an SDL keysym; dev=PI_DEV_PAD: pad code. */
void pc_input_rebind(int dev, int action, int code);
/* Human-readable name for a single key/button code (for the capture UI). */
const char *pc_input_code_name(int dev, int code);
