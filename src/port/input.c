/* pc_input.c — JSON-configurable action bindings (see pc_input.h). Header-only SDL
 * (SDLK_* constants); no SDL functions, so it links in the harness too. */
#include "port/input.h"
#include "port/config.h"
#include <SDL2/SDL.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#define MAX_CHORDS         6
#define MAX_KEYS_PER_CHORD 3
typedef struct { int n; int sym[MAX_KEYS_PER_CHORD]; } Chord;
typedef struct { int n; Chord chord[MAX_CHORDS]; } Binding;

static Binding s_bind[PI_NUM];

#define MAX_HELD 24
static int s_held[MAX_HELD];
static int s_nheld = 0;
static int s_loaded = 0;

static const struct { const char *name; int sym; } k_names[] = {
    {"Up",SDLK_UP},{"Down",SDLK_DOWN},{"Left",SDLK_LEFT},{"Right",SDLK_RIGHT},
    {"Space",SDLK_SPACE},{"Return",SDLK_RETURN},{"Enter",SDLK_RETURN},
    {"LShift",SDLK_LSHIFT},{"RShift",SDLK_RSHIFT},
    {"LCtrl",SDLK_LCTRL},{"Ctrl",SDLK_LCTRL},{"RCtrl",SDLK_RCTRL},
    {"Tab",SDLK_TAB},{"Esc",SDLK_ESCAPE},
    {NULL,0}
};

static int key_from_name(const char *tok)
{
    while (*tok == ' ') tok++;
    char buf[24]; int i = 0;
    while (tok[i] && tok[i] != ' ' && i < 23) { buf[i] = tok[i]; i++; }
    buf[i] = 0;
    if (!buf[0]) return 0;
    for (int k = 0; k_names[k].name; k++)
        if (!strcasecmp(buf, k_names[k].name)) return k_names[k].sym;
    if (!buf[1]) {  /* single char: letter or digit */
        char c = (char)tolower((unsigned char)buf[0]);
        if (c >= 'a' && c <= 'z') return SDLK_a + (c - 'a');
        if (c >= '0' && c <= '9') return SDLK_0 + (c - '0');
    }
    return 0;
}

/* Parse "X+Down, C, RShift" → chords. */
static void parse_binding(const char *p, Binding *b)
{
    b->n = 0;
    while (*p && b->n < MAX_CHORDS) {
        Chord c; c.n = 0;
        while (*p && *p != ',') {
            char tok[24]; int ti = 0;
            while (*p == ' ') p++;
            while (*p && *p != '+' && *p != ',' && ti < 23) tok[ti++] = *p++;
            tok[ti] = 0;
            int sym = key_from_name(tok);
            if (sym && c.n < MAX_KEYS_PER_CHORD) c.sym[c.n++] = sym;
            if (*p == '+') p++;
        }
        if (c.n) b->chord[b->n++] = c;
        if (*p == ',') p++;
    }
}

static const struct { int act; const char *key; const char *def; } k_defaults[] = {
    {PI_LEFT,     "bind_left",     "Left"},
    {PI_RIGHT,    "bind_right",    "Right"},
    {PI_UP,       "bind_up",       "Up"},
    {PI_DOWN,     "bind_down",     "Down"},
    {PI_HOP,      "bind_hop",      "Up"},                       /* Up also hops (vanilla) */
    {PI_FIRE,     "bind_fire",     "Z, LCtrl, Space, Return"},
    {PI_INTERACT, "bind_interact", "X, LShift"},
    {PI_DROP,     "bind_drop",     "X+Down, C, RShift"},
};

void pc_input_load(void)
{
    if (s_loaded) return;
    s_loaded = 1;
    pc_config_load();
    for (int i = 0; i < (int)(sizeof k_defaults / sizeof k_defaults[0]); i++) {
        char buf[160];
        const char *s = pc_config_str(k_defaults[i].key, buf, sizeof buf)
                            ? buf : k_defaults[i].def;
        parse_binding(s, &s_bind[k_defaults[i].act]);
    }
}

void pc_input_key(int sym, int down)
{
    if (down) {
        for (int i = 0; i < s_nheld; i++) if (s_held[i] == sym) return;
        if (s_nheld < MAX_HELD) s_held[s_nheld++] = sym;
    } else {
        for (int i = 0; i < s_nheld; i++)
            if (s_held[i] == sym) { s_held[i] = s_held[--s_nheld]; return; }
    }
}

void pc_input_release_all(void) { s_nheld = 0; }

static int held(int sym) { for (int i = 0; i < s_nheld; i++) if (s_held[i] == sym) return 1; return 0; }

int pc_input_active(int action)
{
    if (action < 0 || action >= PI_NUM) return 0;
    const Binding *b = &s_bind[action];
    for (int ci = 0; ci < b->n; ci++) {
        const Chord *c = &b->chord[ci];
        int all = 1;
        for (int k = 0; k < c->n; k++) if (!held(c->sym[k])) { all = 0; break; }
        if (c->n && all) return 1;
    }
    return 0;
}
