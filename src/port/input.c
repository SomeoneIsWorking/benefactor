/* pc_input.c — JSON-configurable action bindings, keyboard + controller (see
 * pc_input.h). Uses only SDL name-lookup helpers (no window/event calls), so it
 * links in the harness too. */
#include "port/input.h"
#include "port/config.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#define MAX_CHORDS         6
#define MAX_KEYS_PER_CHORD 3
typedef struct { int n; int sym[MAX_KEYS_PER_CHORD]; } Chord;
typedef struct { int n; Chord chord[MAX_CHORDS]; } Binding;

static Binding s_bind[PI_NUM_DEV][PI_NUM];

#define MAX_HELD 24
static int s_held[PI_NUM_DEV][MAX_HELD];
static int s_nheld[PI_NUM_DEV];
static int s_loaded = 0;

/* ── Keyboard key names ─────────────────────────────────────────────────────── */

static const struct { const char *name; int sym; } k_names[] = {
    {"Up",SDLK_UP},{"Down",SDLK_DOWN},{"Left",SDLK_LEFT},{"Right",SDLK_RIGHT},
    {"Space",SDLK_SPACE},{"Return",SDLK_RETURN},{"Enter",SDLK_RETURN},
    {"LShift",SDLK_LSHIFT},{"RShift",SDLK_RSHIFT},
    {"LCtrl",SDLK_LCTRL},{"Ctrl",SDLK_LCTRL},{"RCtrl",SDLK_RCTRL},
    {"Tab",SDLK_TAB},{"Esc",SDLK_ESCAPE},
    {NULL,0}
};

static int key_from_name(const char *buf)
{
    if (!buf[0]) return 0;
    for (int k = 0; k_names[k].name; k++)
        if (!strcasecmp(buf, k_names[k].name)) return k_names[k].sym;
    if (!buf[1]) {  /* single char: letter or digit */
        char c = (char)tolower((unsigned char)buf[0]);
        if (c >= 'a' && c <= 'z') return SDLK_a + (c - 'a');
        if (c >= '0' && c <= '9') return SDLK_0 + (c - '0');
    }
    /* Anything else: SDL's own key-name table ("F1", "Backspace", ...). */
    SDL_Keycode kc = SDL_GetKeyFromName(buf);
    return (kc != SDLK_UNKNOWN) ? (int)kc : 0;
}

static const char *key_name(int sym, char *buf, int cap)
{
    for (int k = 0; k_names[k].name; k++)
        if (k_names[k].sym == sym && strcasecmp(k_names[k].name, "Enter")
                                  && strcasecmp(k_names[k].name, "Ctrl")) {
            snprintf(buf, cap, "%s", k_names[k].name);
            return buf;
        }
    snprintf(buf, cap, "%s", SDL_GetKeyName((SDL_Keycode)sym));
    return buf;
}

/* ── Controller button/axis names ───────────────────────────────────────────
 * Pad code = SDL_GameControllerButton, or PI_PAD_AXIS_CODE(axis, dir) for an
 * analog direction. */

static const struct { const char *name; int code; } p_names[] = {
    {"A", SDL_CONTROLLER_BUTTON_A}, {"B", SDL_CONTROLLER_BUTTON_B},
    {"X", SDL_CONTROLLER_BUTTON_X}, {"Y", SDL_CONTROLLER_BUTTON_Y},
    {"DPUp",    SDL_CONTROLLER_BUTTON_DPAD_UP},
    {"DPDown",  SDL_CONTROLLER_BUTTON_DPAD_DOWN},
    {"DPLeft",  SDL_CONTROLLER_BUTTON_DPAD_LEFT},
    {"DPRight", SDL_CONTROLLER_BUTTON_DPAD_RIGHT},
    {"Start",   SDL_CONTROLLER_BUTTON_START},
    {"Back",    SDL_CONTROLLER_BUTTON_BACK},
    {"LB",      SDL_CONTROLLER_BUTTON_LEFTSHOULDER},
    {"RB",      SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
    {"LStick",  SDL_CONTROLLER_BUTTON_LEFTSTICK},
    {"RStick",  SDL_CONTROLLER_BUTTON_RIGHTSTICK},
    {"LeftTrigger",  PI_PAD_AXIS_CODE(SDL_CONTROLLER_AXIS_TRIGGERLEFT, 1)},
    {"RightTrigger", PI_PAD_AXIS_CODE(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 1)},
    {"LeftX-",  PI_PAD_AXIS_CODE(SDL_CONTROLLER_AXIS_LEFTX, 0)},
    {"LeftX+",  PI_PAD_AXIS_CODE(SDL_CONTROLLER_AXIS_LEFTX, 1)},
    {"LeftY-",  PI_PAD_AXIS_CODE(SDL_CONTROLLER_AXIS_LEFTY, 0)},
    {"LeftY+",  PI_PAD_AXIS_CODE(SDL_CONTROLLER_AXIS_LEFTY, 1)},
    {"RightX-", PI_PAD_AXIS_CODE(SDL_CONTROLLER_AXIS_RIGHTX, 0)},
    {"RightX+", PI_PAD_AXIS_CODE(SDL_CONTROLLER_AXIS_RIGHTX, 1)},
    {"RightY-", PI_PAD_AXIS_CODE(SDL_CONTROLLER_AXIS_RIGHTY, 0)},
    {"RightY+", PI_PAD_AXIS_CODE(SDL_CONTROLLER_AXIS_RIGHTY, 1)},
    {NULL, 0}
};

static int pad_from_name(const char *buf)
{
    if (!buf[0]) return -1;
    for (int k = 0; p_names[k].name; k++)
        if (!strcasecmp(buf, p_names[k].name)) return p_names[k].code;
    return -1;
}

static const char *pad_name(int code, char *buf, int cap)
{
    for (int k = 0; p_names[k].name; k++)
        if (p_names[k].code == code) { snprintf(buf, cap, "%s", p_names[k].name); return buf; }
    snprintf(buf, cap, "PAD%d", code);
    return buf;
}

const char *pc_input_code_name(int dev, int code)
{
    static char buf[32];
    return (dev == PI_DEV_PAD) ? pad_name(code, buf, sizeof buf)
                               : key_name(code, buf, sizeof buf);
}

/* ── Binding parse ──────────────────────────────────────────────────────────── */

/* Parse "X+Down, C, RShift" → chords (token resolver per device). */
static void parse_binding(int dev, const char *p, Binding *b)
{
    b->n = 0;
    while (*p && b->n < MAX_CHORDS) {
        Chord c; c.n = 0;
        while (*p && *p != ',') {
            char tok[24]; int ti = 0;
            while (*p == ' ') p++;
            while (*p && *p != '+' && *p != ',' && ti < 23) tok[ti++] = *p++;
            while (ti > 0 && tok[ti - 1] == ' ') ti--;       /* trim trailing */
            tok[ti] = 0;
            int code = (dev == PI_DEV_PAD) ? pad_from_name(tok) : key_from_name(tok);
            int ok   = (dev == PI_DEV_PAD) ? (code >= 0) : (code != 0);
            if (ok && c.n < MAX_KEYS_PER_CHORD) c.sym[c.n++] = code;
            if (*p == '+') p++;
        }
        if (c.n) b->chord[b->n++] = c;
        if (*p == ',') p++;
    }
}

/* Defaults. Pad: A = fire (primary button, like the Amiga stick). The control
 * set is deliberately minimal — ONE fire, ONE interact; DROP is always
 * interact+Down (no binding) and HOP has no dedicated key (Up/DPad-up jumps,
 * as on hardware). The hop/drop ACTIONS still parse from JSON (bind_hop /
 * bind_drop / pad_…) for power users, they just have no defaults. */
static const struct { int act; const char *key[PI_NUM_DEV]; const char *def[PI_NUM_DEV]; } k_defaults[] = {
    {PI_LEFT,  {"bind_left",  "pad_left"},  {"Left",  "DPLeft, LeftX-"}},
    {PI_RIGHT, {"bind_right", "pad_right"}, {"Right", "DPRight, LeftX+"}},
    {PI_UP,    {"bind_up",    "pad_up"},    {"Up",    "DPUp, LeftY-"}},
    {PI_DOWN,  {"bind_down",  "pad_down"},  {"Down",  "DPDown, LeftY+"}},
    {PI_HOP,   {"bind_hop",   "pad_hop"},   {"",      ""}},
    {PI_FIRE,  {"bind_fire",  "pad_fire"},  {"Z, LCtrl, Space, Return", "A, B"}},
    {PI_INTERACT, {"bind_interact", "pad_interact"}, {"X, LShift", "X"}},
    {PI_DROP,  {"bind_drop",  "pad_drop"},  {"",      ""}},
    {PI_FFWD,  {"bind_ffwd",  "pad_ffwd"},  {"Tab",   "RightTrigger"}},
    {PI_FREECAM, {"bind_freecam", "pad_freecam"}, {"C", "Back"}},
};

static const char *binding_cfg_key(int dev, int action)
{
    for (int i = 0; i < (int)(sizeof k_defaults / sizeof k_defaults[0]); i++)
        if (k_defaults[i].act == action) return k_defaults[i].key[dev];
    return NULL;
}

void pc_input_load(void)
{
    if (s_loaded) return;
    s_loaded = 1;
    pc_config_load();
    for (int d = 0; d < PI_NUM_DEV; d++)
        for (int i = 0; i < (int)(sizeof k_defaults / sizeof k_defaults[0]); i++) {
            char buf[160];
            const char *s = pc_cfg_show(k_defaults[i].key[d], buf, sizeof buf, NULL) && buf[0]
                                ? buf : k_defaults[i].def[d];
            parse_binding(d, s, &s_bind[d][k_defaults[i].act]);
        }
}

void pc_input_reload(void) { s_loaded = 0; pc_input_load(); }

/* ── Held state ─────────────────────────────────────────────────────────────── */

static void dev_press(int dev, int code, int down)
{
    if (down) {
        for (int i = 0; i < s_nheld[dev]; i++) if (s_held[dev][i] == code) return;
        if (s_nheld[dev] < MAX_HELD) s_held[dev][s_nheld[dev]++] = code;
    } else {
        for (int i = 0; i < s_nheld[dev]; i++)
            if (s_held[dev][i] == code) { s_held[dev][i] = s_held[dev][--s_nheld[dev]]; return; }
    }
}

void pc_input_key(int sym, int down)        { dev_press(PI_DEV_KB, sym, down); }
void pc_input_pad_button(int code, int down){ dev_press(PI_DEV_PAD, code, down); }
void pc_input_pad_clear(void)               { s_nheld[PI_DEV_PAD] = 0; }
void pc_input_release_all(void)             { s_nheld[PI_DEV_KB] = s_nheld[PI_DEV_PAD] = 0; }

static int held(int dev, int code)
{
    for (int i = 0; i < s_nheld[dev]; i++) if (s_held[dev][i] == code) return 1;
    return 0;
}

int pc_input_active_dev(int dev, int action)
{
    if (action < 0 || action >= PI_NUM || dev < 0 || dev >= PI_NUM_DEV) return 0;
    const Binding *b = &s_bind[dev][action];
    for (int ci = 0; ci < b->n; ci++) {
        const Chord *c = &b->chord[ci];
        int all = 1;
        for (int k = 0; k < c->n; k++) if (!held(dev, c->sym[k])) { all = 0; break; }
        if (c->n && all) return 1;
    }
    return 0;
}

int pc_input_active(int action)
{
    return pc_input_active_dev(PI_DEV_KB, action) || pc_input_active_dev(PI_DEV_PAD, action);
}

/* ── Menu / rebinding support ───────────────────────────────────────────────── */

const char *pc_input_action_name(int action)
{
    static const char *names[PI_NUM] = {
        "LEFT", "RIGHT", "UP", "DOWN", "JUMP", "FIRE", "INTERACT", "DROP",
        "FAST FORWARD", "FREE CAM"
    };
    return (action >= 0 && action < PI_NUM) ? names[action] : "?";
}

const char *pc_input_binding_str(int dev, int action, char *buf, int cap)
{
    if (cap > 0) buf[0] = 0;
    const Binding *b = &s_bind[dev][action];
    int n = 0;
    for (int ci = 0; ci < b->n && n < cap - 1; ci++) {
        if (ci) n += snprintf(buf + n, cap - n, ", ");
        for (int k = 0; k < b->chord[ci].n && n < cap - 1; k++) {
            char nm[32];
            if (k) n += snprintf(buf + n, cap - n, "+");
            n += snprintf(buf + n, cap - n, "%s",
                          (dev == PI_DEV_PAD) ? pad_name(b->chord[ci].sym[k], nm, sizeof nm)
                                              : key_name(b->chord[ci].sym[k], nm, sizeof nm));
        }
    }
    return buf;
}

/* Replace the binding with a single key/button, persist to the config file, and
 * re-apply live. (Chords/multi-bindings stay editable in benefactor.json.) */
void pc_input_rebind(int dev, int action, int code)
{
    const char *key = binding_cfg_key(dev, action);
    if (!key) return;
    char nm[32], json[40];
    if (dev == PI_DEV_PAD) pad_name(code, nm, sizeof nm);
    else                   key_name(code, nm, sizeof nm);
    snprintf(json, sizeof json, "\"%s\"", nm);
    pc_cfg_persist(key, json);
    parse_binding(dev, nm, &s_bind[dev][action]);
}
