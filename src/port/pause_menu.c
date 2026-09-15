/* src/port/pause_menu.c — ESC-triggered in-game pause menu + OPTIONS submenus.
 *
 * This file is the menu itself: which page is open, where the cursor is, what
 * each row is worth, and what a keypress does to any of that. The pixels are
 * src/port/pause_menu_draw.c's, which gets a PcPauseView snapshot built at the
 * bottom of this file (port/pause_menu_view.h) and never sees the statics.
 *
 * Pages:
 *   MAIN     — RESUME / OPTIONS / RETRY / EXIT TO MAIN MENU / QUIT TO DESKTOP
 *   OPTIONS  — game speed, jump physics, free-cam, and links to the GRAPHICS,
 *              CONTROLS and EXTRA submenus. Every change is applied LIVE and
 *              persisted to benefactor.json (pc_cfg_persist).
 *   GRAPHICS — renderer, aspect ratio, fullscreen, plus the GPU EFFECTS
 *              toggles (AMBIENT DARKNESS / DROP SHADOW), which are greyed out
 *              unless the HARDWARE renderer is active.
 *   CONTROLS — interact range, modern controls per device, links to the two
 *              bindings pages.
 *   EXTRA    — skip intro, unlock all levels, fall damage.
 *   BINDINGS — one page per device (keyboard / controller). Selecting a row
 *              enters CAPTURE: the next key/button pressed on that device
 *              becomes the binding (single key; chords stay JSON-editable).
 *              With modern controls ON for the device, an INTERACT row is
 *              added next to FIRE (no DROP row — drop is always interact+Down).
 *
 * Pause is only available DURING gameplay (g_gameplay_active=1). Outside
 * gameplay ESC keeps its old "quit immediately" behaviour. Inside the menu,
 * ESC (or pad B/Start) backs out one page; on the main page it resumes.
 *
 * Key/button routing lives in hw.c (hw_handle_key / hw_handle_pad_code): while
 * paused, arrows/dpad navigate, fire/A selects, left-right cycles values, and
 * during capture every press is fed to pc_pause_capture_code instead.
 *
 * pc_step (in src/port/game_loop.c) consults pc_pause_active() — when set it skips the
 * coroutine swap so the game freezes. The frame is still rendered + presented
 * so the overlay stays visible. Deferred actions (Retry/Exit/Quit) fire from
 * pc_pause_tick(), called at the top of pc_step. */

#include "common/game_state.h" /* g_state + g_gameplay_active / g_credits_active /
                           * g_enter_gameplay / g_gameplay_entry macros */
#include "common/log.h"
#include "engine/hw.h"
#include "port/config.h"
#include "port/input.h"
#include "port/overlay_ui.h"
#include "port/pause_menu_view.h"
#include "port/update_check.h"
#include <SDL3/SDL.h> /* SDLK_/SDL_GAMEPAD_ constants only */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */

extern void hw_widescreen_refresh(void);
extern int hw_pad_count(void);

/* ── State ─────────────────────────────────────────────────────────────────── */

/* The pages themselves (PG_*) are named in port/pause_menu_view.h, which the
 * drawing half shares. */

static int s_paused = 0;
static int s_page = PG_MAIN;
static int s_cursor = 0; /* per current page */
/* Opened from OUTSIDE gameplay (title/menu/intro, via ESC/Start): the menu
 * starts on the OPTIONS page, the MAIN page (Resume/Retry/Exit) is never
 * shown, and a QUIT row is appended (ESC no longer quits directly there). */
static int s_title_mode = 0;

enum { OPT_RESUME = 0, OPT_OPTIONS, OPT_RETRY, OPT_EXIT_TO_MENU, OPT_QUIT, NUM_MAIN };
/* OPTIONS-page row ids (rows are built per-mode by options_rows). */
enum {
    OO_GRAPHICS = 0,
    OO_SPEED,
    OO_MATCH_DISPLAY,
    OO_PHYSICS,
    OO_FREECAM,
    OO_CONTROLS,
    OO_EXTRA,
    OO_BACK,
    OO_QUIT
};
/* GRAPHICS-page row ids. EFFECTS rows (AMBIENT/SHADOW) are flattened in here —
 * they apply only on the HARDWARE renderer and are greyed out otherwise. */
enum { RR_RENDERER = 0, RR_ASPECT, RR_FULLSCREEN, RR_AMBIENT, RR_SHADOW, RR_BACK };
/* CONTROLS-page row ids (input/binding settings, grouped out of OPTIONS). */
enum { CT_INTERACT = 0, CT_MODERN_KB, CT_MODERN_PAD, CT_BIND_KB, CT_BIND_PAD, CT_BACK };
/* EXTRA-page row ids. */
enum { EX_SKIP_INTRO = 0, EX_UNLOCK_ALL, EX_FALL_DMG, EX_UPDATE, EX_BACK };

/* Bindings capture: which device/action the next press is assigned to. */
static int s_capture = 0, s_capture_dev = 0, s_capture_action = 0;

/* Deferred action — applied at the top of the next pc_step() so we never
 * tear down the coroutine while we're inside its yield handler. */
enum { ACT_NONE = 0, ACT_RESUME, ACT_RETRY, ACT_EXIT_TO_MENU, ACT_QUIT };
static int s_pending_action = ACT_NONE;

int pc_pause_active(void) {
    return s_paused;
}

void pc_pause_toggle(void) {
    if (s_paused) {
        s_pending_action = ACT_RESUME;
    } else {
        s_paused = 1;
        s_title_mode = 0;
        s_page = PG_MAIN;
        s_cursor = OPT_RESUME;
        s_capture = 0;
        s_pending_action = ACT_NONE;
    }
}

/* ESC/Start OUTSIDE gameplay: open straight into the OPTIONS page (the game/
 * attract loop freezes exactly like the in-game pause; closing resumes it). */
void pc_pause_open_options(void) {
    if (s_paused) {
        return;
    }
    s_paused = 1;
    s_title_mode = 1;
    s_page = PG_OPTIONS;
    s_cursor = 0;
    s_capture = 0;
    s_pending_action = ACT_NONE;
}

/* ── Bindings-page row model ───────────────────────────────────────────────────
 * ONE FIRE and (with modern controls) ONE INTERACT — nothing else. There is no
 * DROP row: dropping the carried item/man is always interact+Down. The hop and
 * drop ACTIONS still exist for JSON power users but get no menu row and no
 * defaults. Last row is BACK (action = -1). */
static int bind_rows(int dev, int *actions /* >= 14 */) {
    int n = 0;
    int modern = (dev == PI_DEV_PAD) ? pc_modern_pad() : pc_modern_kb();
    actions[n++] = PI_LEFT;
    actions[n++] = PI_RIGHT;
    actions[n++] = PI_UP;
    actions[n++] = PI_DOWN;
    actions[n++] = PI_FIRE; /* jump/confirm */
    if (modern) {
        actions[n++] = PI_INTERACT;
    }
    actions[n++] = PI_FFWD;    /* hold-to-fast-forward, both schemes */
    actions[n++] = PI_FREECAM; /* free-cam toggle */
    actions[n++] = -1;         /* BACK */
    return n;
}

static const char *bind_row_label(int dev, int action) {
    (void)dev;
    if (action < 0) {
        return "BACK";
    }
    return pc_input_action_name(action);
}

/* OPTIONS-page rows, built per mode: title mode appends QUIT (there is no MAIN
 * page outside gameplay, and ESC no longer quits directly — it opens this). */
static int options_rows(int *rows /* >= 14 */) {
    int n = 0;
    rows[n++] = OO_GRAPHICS;
    rows[n++] = OO_SPEED;
    rows[n++] = OO_MATCH_DISPLAY;
    rows[n++] = OO_PHYSICS;
    rows[n++] = OO_FREECAM;
    rows[n++] = OO_CONTROLS;
    rows[n++] = OO_EXTRA;
    rows[n++] = OO_BACK;
    if (s_title_mode) {
        rows[n++] = OO_QUIT;
    }
    return n;
}

/* GRAPHICS submenu rows (with the flattened-in EFFECTS rows). */
static int graphics_rows(int *rows /* >= 8 */) {
    int n = 0;
    rows[n++] = RR_RENDERER;
    rows[n++] = RR_ASPECT;
    rows[n++] = RR_FULLSCREEN;
    rows[n++] = RR_AMBIENT;
    rows[n++] = RR_SHADOW;
    rows[n++] = RR_BACK;
    return n;
}

/* CONTROLS submenu rows. */
static int controls_rows(int *rows /* >= 8 */) {
    int n = 0;
    rows[n++] = CT_INTERACT;
    rows[n++] = CT_MODERN_KB;
    rows[n++] = CT_MODERN_PAD;
    rows[n++] = CT_BIND_KB;
    rows[n++] = CT_BIND_PAD;
    rows[n++] = CT_BACK;
    return n;
}

static int extra_rows(int *rows /* >= 14 */) {
    int n = 0;
    rows[n++] = EX_SKIP_INTRO;
    rows[n++] = EX_UNLOCK_ALL;
    rows[n++] = EX_FALL_DMG;
    rows[n++] = EX_UPDATE;
    rows[n++] = EX_BACK;
    return n;
}

static int page_rows(int page) {
    int acts[14];
    switch (page) {
    case PG_MAIN:
        return NUM_MAIN;
    case PG_OPTIONS:
        return options_rows(acts);
    case PG_GRAPHICS:
        return graphics_rows(acts);
    case PG_CONTROLS:
        return controls_rows(acts);
    case PG_EXTRA:
        return extra_rows(acts);
    default:
        return bind_rows(page == PG_BIND_PAD, acts);
    }
}

static void enter_page(int page) {
    s_page = page;
    s_cursor = 0;
}

/* ── Option values (resolved live, persisted on change) ───────────────────────── */

/* RENDERER row (GRAPHICS submenu) — three renderers, driving "renderer"
 * (vanilla|benren) + "present" (sdl|vulkan):
 *   VANILLA  — Amiga-blit-faithful copper render (vanilla + sdl)
 *   SOFTWARE — BenRen, the CPU per-sprite renderer (benren + sdl)
 *   HARDWARE — BenRen VK, the GPU per-sprite renderer (benren + vulkan)
 * Aspect ratio is now a SEPARATE row (widescreen_mode), independent of renderer. */
#define NUM_RENDERERS 3
static const char *k_rend_labels[NUM_RENDERERS] = {"VANILLA", "SOFTWARE", "HARDWARE"};

static int renderer_index(void) {
    char r[24] = "", p[24] = "";
    pc_cfg_show("renderer", r, sizeof r, NULL);
    pc_cfg_show("present", p, sizeof p, NULL);
    if (strcasecmp(r, "benren") != 0) {
        return 0; /* VANILLA (or unset) */
    }
    return (!strcasecmp(p, "vulkan")) ? 2 : 1; /* HARDWARE : SOFTWARE */
}

static void renderer_set(int idx) {
    idx = (idx % NUM_RENDERERS + NUM_RENDERERS) % NUM_RENDERERS;
    char json[24];
    snprintf(json, sizeof json, "\"%s\"", idx == 0 ? "vanilla" : "benren");
    pc_cfg_persist("renderer", json);
    snprintf(json, sizeof json, "\"%s\"", idx == 2 ? "vulkan" : "sdl");
    pc_cfg_persist("present", json);
    /* No backend/window swap: Vulkan owns the window for every renderer; the
     * Software<->Hardware switch is read live each frame (hw_scene_render_enabled). */
    hw_widescreen_refresh();
}

/* ASPECT row — STOCK 4:3 / 16:9 / AUTO (follows the window). Drives
 * "widescreen_mode". (ultrawide stays reachable via the raw cfg knob.) */
#define NUM_ASPECTS 3
static const char *k_aspect_vals[NUM_ASPECTS] = {"disabled", "16:9", "auto"};
static const char *k_aspect_labels[NUM_ASPECTS] = {"STOCK 4:3", "16:9", "AUTO"};

static int aspect_index(void) {
    char w[24] = "";
    pc_cfg_show("widescreen_mode", w, sizeof w, NULL);
    if (!strcasecmp(w, "16:9") || !strcasecmp(w, "ultrawide")) {
        return 1;
    }
    if (!strcasecmp(w, "auto")) {
        return 2;
    }
    return 0; /* disabled / unset */
}

static void aspect_set(int idx) {
    char json[24];
    snprintf(json, sizeof json, "\"%s\"",
             k_aspect_vals[(idx % NUM_ASPECTS + NUM_ASPECTS) % NUM_ASPECTS]);
    pc_cfg_persist("widescreen_mode", json);
    hw_widescreen_refresh();
}

/* GPU effects apply only on the HARDWARE renderer (index 2). */
static int hardware_active(void) {
    return renderer_index() == 2;
}

/* The flattened EFFECTS rows map to the fx_* bool knobs the Vulkan lighting
 * pass reads. They are inert (greyed out) unless the HARDWARE renderer is on. */
static const char *fx_knob(int row) {
    switch (row) {
    case RR_AMBIENT:
        return "fx_ambient";
    case RR_SHADOW:
        return "fx_shadow";
    default:
        return NULL;
    }
}

static void graphics_cycle(int row, int dir) {
    switch (row) {
    case RR_RENDERER:
        renderer_set(renderer_index() + dir);
        break;
    case RR_ASPECT:
        aspect_set(aspect_index() + dir);
        break;
    case RR_FULLSCREEN: {
        pc_cfg_persist("fullscreen", pc_cfg_bool("fullscreen", 0) ? "false" : "true");
        hw_fullscreen_refresh();
        break;
    }
    case RR_AMBIENT:
    case RR_SHADOW: {
        if (!hardware_active()) {
            break; /* greyed: HARDWARE only */
        }
        const char *k = fx_knob(row); /* bool toggles */
        if (k) {
            pc_cfg_persist(k, pc_cfg_bool(k, 0) ? "false" : "true");
        }
        break;
    }
    default:
        break;
    }
}

/* Game speed: normal / turbo (=1.2x) / hyper (=1.5x). Audio/music stay
 * real-time regardless; the hold-to-fast-forward binding (5x) is separate. */
#define NUM_SPEEDS 3
static const char *k_speed_vals[NUM_SPEEDS] = {"normal", "turbo", "hyper"};
static const char *k_speed_labels[NUM_SPEEDS] = {"NORMAL", "TURBO (1.2X)", "HYPER (1.5X)"};

static int speed_index(void) {
    char buf[16];
    if (!pc_cfg_show("game_speed", buf, sizeof buf, NULL) || !buf[0]) {
        return 0;
    }
    for (int i = 1; i < NUM_SPEEDS; i++) {
        if (!strcasecmp(buf, k_speed_vals[i])) {
            return i;
        }
    }
    return 0;
}

static void speed_set(int idx) {
    char json[16];
    snprintf(json, sizeof json, "\"%s\"",
             k_speed_vals[(idx % NUM_SPEEDS + NUM_SPEEDS) % NUM_SPEEDS]);
    pc_cfg_persist("game_speed", json);
    hw_speed_refresh();
}

/* Matching the display: off / when it is free / whatever it costs.
 *
 * A game frame is almost never a whole number of the display's refreshes — 50
 * frames a second on a 120 Hz panel is 2.4 of them — so frames are held for
 * uneven lengths of time and slow steady motion wobbles however exactly they
 * are paced. Rounding the frame to a whole number of refreshes fixes that, and
 * costs the difference between the speed asked for and the nearest one the
 * display can hold steady. On a 144 Hz panel that difference is about four
 * percent and IF FREE takes it by itself; on a 60 or 120 Hz one it is twenty,
 * so ALWAYS is there to be chosen rather than assumed. */
#define NUM_MATCH 3
static const char *k_match_vals[NUM_MATCH] = {"off", "free", "always"};
static const char *k_match_labels[NUM_MATCH] = {"OFF", "IF FREE", "ALWAYS"};

static int match_index(void) {
    char buf[16];
    if (!pc_cfg_show("pace_to_display", buf, sizeof buf, NULL) || !buf[0]) {
        return 1; /* the default: take the free matches, leave the costly ones */
    }
    for (int i = 0; i < NUM_MATCH; i++) {
        if (!strcasecmp(buf, k_match_vals[i])) {
            return i;
        }
    }
    return 1;
}

static void match_set(int idx) {
    char json[16];
    snprintf(json, sizeof json, "\"%s\"", k_match_vals[(idx % NUM_MATCH + NUM_MATCH) % NUM_MATCH]);
    pc_cfg_persist("pace_to_display", json);
}

/* "Extend interaction range": disabled / enabled (enabled = 5 px). */
#define INTERACT_EXTEND_ON 5
static int interact_enabled(void) {
    return pc_cfg_int("interact_extend", 0) > 0;
}
static void interact_set(int on) {
    pc_cfg_persist("interact_extend", on ? "5" : "0");
}

static void modern_set(int dev, int on) {
    pc_cfg_persist(dev == PI_DEV_PAD ? "modern_controls_controller" : "modern_controls_keyboard",
                   on ? "true" : "false");
    pc_input_reload(); /* JUMP/FIRE defaults are scheme-dependent */
}

/* Toggle a persisted bool knob. */
static void bool_knob_toggle(const char *key) {
    pc_cfg_persist(key, pc_cfg_bool(key, 0) ? "false" : "true");
}

/* Cycle an OPTIONS row's value by `dir` (+1 right/select, -1 left). */
static void options_cycle(int row, int dir) {
    switch (row) {
    /* OO_GRAPHICS / OO_CONTROLS / OO_EXTRA are submenu links (entered on
     * select), not value cycles. */
    case OO_SPEED:
        speed_set(speed_index() + dir);
        break;
    case OO_MATCH_DISPLAY:
        match_set(match_index() + dir);
        break;
    case OO_PHYSICS:
        bool_knob_toggle("platformer_physics");
        break;
    case OO_FREECAM:
        bool_knob_toggle("freecam_pause");
        break;
    default:
        break;
    }
}

/* Cycle a CONTROLS row's value (bindings rows are submenu links). */
static void controls_cycle(int row, int dir) {
    (void)dir;
    switch (row) {
    case CT_INTERACT:
        interact_set(!interact_enabled());
        break;
    case CT_MODERN_KB:
        modern_set(PI_DEV_KB, !pc_modern_kb());
        break;
    case CT_MODERN_PAD:
        modern_set(PI_DEV_PAD, !pc_modern_pad());
        break;
    default:
        break;
    }
}

/* Fall damage: vanilla / light (half) / none — consumed by the $579F86 +
 * terrain-pass wrappers in src/port/overrides/platformer.c. */
#define NUM_FALL_DMG 3
static const char *k_fall_dmg_vals[NUM_FALL_DMG] = {"vanilla", "light", "none"};
static const char *k_fall_dmg_labels[NUM_FALL_DMG] = {"VANILLA", "LIGHT", "NONE"};

static int fall_dmg_index(void) {
    char buf[16];
    if (!pc_cfg_show("fall_damage", buf, sizeof buf, NULL) || !buf[0]) {
        return 0;
    }
    for (int i = 1; i < NUM_FALL_DMG; i++) {
        if (!strcasecmp(buf, k_fall_dmg_vals[i])) {
            return i;
        }
    }
    return 0;
}

static void fall_dmg_set(int idx) {
    char json[16];
    snprintf(json, sizeof json, "\"%s\"",
             k_fall_dmg_vals[(idx % NUM_FALL_DMG + NUM_FALL_DMG) % NUM_FALL_DMG]);
    pc_cfg_persist("fall_damage", json);
}

static void extra_cycle(int row) {
    switch (row) {
    case EX_SKIP_INTRO:
        bool_knob_toggle("skip_intro");
        break;
    case EX_UNLOCK_ALL:
        bool_knob_toggle("unlock_all_levels");
        break;
    case EX_FALL_DMG:
        fall_dmg_set(fall_dmg_index() + 1);
        break;
    case EX_UPDATE:
        bool_knob_toggle("update_check");
        break;
    default:
        break;
    }
}

/* ── Navigation (called from hw.c while paused) ───────────────────────────────── */

void pc_pause_input_up(void) {
    if (!s_paused || s_capture) {
        return;
    }
    int n = page_rows(s_page);
    s_cursor = (s_cursor + n - 1) % n;
}

void pc_pause_input_down(void) {
    if (!s_paused || s_capture) {
        return;
    }
    int n = page_rows(s_page);
    s_cursor = (s_cursor + 1) % n;
}

void pc_pause_input_left(void) {
    if (!s_paused || s_capture) {
        return;
    }
    if (s_page == PG_OPTIONS) {
        int rows[14];
        int n = options_rows(rows);
        if (s_cursor < n) {
            options_cycle(rows[s_cursor], -1);
        }
    } else if (s_page == PG_GRAPHICS) {
        int rows[14];
        int n = graphics_rows(rows);
        if (s_cursor < n) {
            graphics_cycle(rows[s_cursor], -1);
        }
    } else if (s_page == PG_CONTROLS) {
        int rows[14];
        int n = controls_rows(rows);
        if (s_cursor < n) {
            controls_cycle(rows[s_cursor], -1);
        }
    } else if (s_page == PG_EXTRA) {
        int rows[14];
        int n = extra_rows(rows);
        if (s_cursor < n) {
            extra_cycle(rows[s_cursor]);
        }
    }
}

void pc_pause_input_right(void) {
    if (!s_paused || s_capture) {
        return;
    }
    if (s_page == PG_OPTIONS) {
        int rows[14];
        int n = options_rows(rows);
        if (s_cursor < n) {
            options_cycle(rows[s_cursor], +1);
        }
    } else if (s_page == PG_GRAPHICS) {
        int rows[14];
        int n = graphics_rows(rows);
        if (s_cursor < n) {
            graphics_cycle(rows[s_cursor], +1);
        }
    } else if (s_page == PG_CONTROLS) {
        int rows[14];
        int n = controls_rows(rows);
        if (s_cursor < n) {
            controls_cycle(rows[s_cursor], +1);
        }
    } else if (s_page == PG_EXTRA) {
        int rows[14];
        int n = extra_rows(rows);
        if (s_cursor < n) {
            extra_cycle(rows[s_cursor]);
        }
    }
}

/* Cursor restore helper: position the OPTIONS cursor on a given row id. */
static void enter_options_at(int row_id) {
    int rows[14];
    int n = options_rows(rows);
    enter_page(PG_OPTIONS);
    for (int i = 0; i < n; i++) {
        if (rows[i] == row_id) {
            s_cursor = i;
            break;
        }
    }
}

static void enter_controls_at(int row_id) {
    int rows[14];
    int n = controls_rows(rows);
    enter_page(PG_CONTROLS);
    for (int i = 0; i < n; i++) {
        if (rows[i] == row_id) {
            s_cursor = i;
            break;
        }
    }
}

void pc_pause_input_select(void) {
    if (!s_paused || s_capture) {
        return;
    }
    switch (s_page) {
    case PG_MAIN:
        switch (s_cursor) {
        case OPT_RESUME:
            s_pending_action = ACT_RESUME;
            break;
        case OPT_OPTIONS:
            enter_page(PG_OPTIONS);
            break;
        case OPT_RETRY:
            s_pending_action = ACT_RETRY;
            break;
        case OPT_EXIT_TO_MENU:
            s_pending_action = ACT_EXIT_TO_MENU;
            break;
        case OPT_QUIT:
            s_pending_action = ACT_QUIT;
            break;
        }
        break;
    case PG_OPTIONS: {
        int rows[14];
        int n = options_rows(rows);
        if (s_cursor >= n) {
            s_cursor = n - 1;
        }
        switch (rows[s_cursor]) {
        case OO_GRAPHICS:
            enter_page(PG_GRAPHICS);
            break;
        case OO_CONTROLS:
            enter_page(PG_CONTROLS);
            break;
        case OO_EXTRA:
            enter_page(PG_EXTRA);
            break;
        case OO_BACK:
            if (s_title_mode) {
                s_pending_action = ACT_RESUME; /* close */
            } else {
                enter_page(PG_MAIN);
                s_cursor = OPT_OPTIONS;
            }
            break;
        case OO_QUIT:
            s_pending_action = ACT_QUIT;
            break;
        default:
            options_cycle(rows[s_cursor], +1);
            break;
        }
        break;
    }
    case PG_GRAPHICS: {
        int rows[14];
        int n = graphics_rows(rows);
        if (s_cursor >= n) {
            s_cursor = n - 1;
        }
        if (rows[s_cursor] == RR_BACK) {
            enter_options_at(OO_GRAPHICS);
        } else {
            graphics_cycle(rows[s_cursor], +1);
        }
        break;
    }
    case PG_CONTROLS: {
        int rows[14];
        int n = controls_rows(rows);
        if (s_cursor >= n) {
            s_cursor = n - 1;
        }
        switch (rows[s_cursor]) {
        case CT_BIND_KB:
            enter_page(PG_BIND_KB);
            break;
        case CT_BIND_PAD:
            enter_page(PG_BIND_PAD);
            break;
        case CT_BACK:
            enter_options_at(OO_CONTROLS);
            break;
        default:
            controls_cycle(rows[s_cursor], +1);
            break;
        }
        break;
    }
    case PG_EXTRA: {
        int rows[14];
        int n = extra_rows(rows);
        if (s_cursor >= n) {
            s_cursor = n - 1;
        }
        if (rows[s_cursor] == EX_BACK) {
            enter_options_at(OO_EXTRA);
        } else {
            extra_cycle(rows[s_cursor]);
        }
        break;
    }
    case PG_BIND_KB:
    case PG_BIND_PAD: {
        int acts[14];
        int dev = (s_page == PG_BIND_PAD) ? PI_DEV_PAD : PI_DEV_KB;
        int n = bind_rows(dev, acts);
        if (s_cursor >= n) {
            s_cursor = n - 1;
        }
        if (acts[s_cursor] < 0) { /* BACK */
            enter_controls_at(dev == PI_DEV_PAD ? CT_BIND_PAD : CT_BIND_KB);
        } else {
            s_capture = 1;
            s_capture_dev = dev;
            s_capture_action = acts[s_cursor];
        }
        break;
    }
    }
}

/* Only the paths that really close the menu say so: backing out of a submenu
 * leaves it open, and a log line that cannot be told from the other case is
 * worse than none. */
static void close_from_escape(void) {
    benefactor_log_write(BENEFACTOR_LOG_INFO, "menu", "escape closed the pause menu");
    s_pending_action = ACT_RESUME;
}

/* ESC / pad B: cancel capture, back out one page, or resume from the main page.
 * In title mode (opened via ESC/Start outside gameplay) OPTIONS is the root —
 * backing out of it closes the menu. */
void pc_pause_escape(void) {
    if (!s_paused) {
        return;
    }
    if (s_capture) {
        s_capture = 0;
        return;
    }
    benefactor_log_write(BENEFACTOR_LOG_DEBUG, "menu", "escape stepped back from page %d", s_page);
    switch (s_page) {
    case PG_BIND_KB:
        enter_controls_at(CT_BIND_KB);
        break;
    case PG_BIND_PAD:
        enter_controls_at(CT_BIND_PAD);
        break;
    case PG_GRAPHICS:
        enter_options_at(OO_GRAPHICS);
        break;
    case PG_CONTROLS:
        enter_options_at(OO_CONTROLS);
        break;
    case PG_EXTRA:
        enter_options_at(OO_EXTRA);
        break;
    case PG_OPTIONS:
        if (s_title_mode) {
            close_from_escape();
        } else {
            enter_page(PG_MAIN);
            s_cursor = OPT_OPTIONS;
        }
        break;
    default:
        close_from_escape();
        break;
    }
}

/* ── Bindings capture ─────────────────────────────────────────────────────────── */

int pc_pause_capture_active(void) {
    return s_paused && s_capture;
}

void pc_pause_capture_code(int dev, int code) {
    if (!pc_pause_capture_active()) {
        return;
    }
    if (dev == PI_DEV_KB && code == SDLK_ESCAPE) {
        s_capture = 0;
        return;
    }
    if (dev != s_capture_dev) {
        return; /* press must come from the device being bound */
    }
    if (dev == PI_DEV_PAD && code == SDL_GAMEPAD_BUTTON_START) {
        return; /* reserved: pause */
    }
    pc_input_rebind(dev, s_capture_action, code);
    s_capture = 0;
}

/* ── Action handlers (called from pc_pause_tick, on the main loop) ──────── */

/* g_gameplay_active / g_overlay_active / g_credits_active / g_enter_gameplay /
 * g_gameplay_entry are all g_state members — macro-aliased via game_state.h. */

/* Game-loop API needed for the "Exit to main menu" soft reset.
 * We rebuild the game coroutine at $3000 (cold-start), which requires:
 *   1. clearing transient game state (pc_state_reset_defaults is private to
 *      src/port/game_loop.c, so we re-implement minimal clearing here);
 *   2. re-decrunching the boot loader at $3000 in case the overlay loader
 *      (d0=0/2/3) overwrote it; and
 *   3. swapping the coroutine to point at game_coro_entry.
 *
 * The game-loop module exposes one helper so this file does not depend on its internals. */
extern void pc_request_cold_restart(void);

/* Restart the CURRENT level from the top (→ level card → cavern). Same
 * mechanism as the REPL `goto N` command: keep $20.w at its current value (so
 * the level table dispatcher picks this level), point gameplay_entry at $577000,
 * and ask pc_step_threaded to respawn the game thread there. Only sets flags —
 * the actual thread teardown/respawn happens on the main loop in
 * pc_step_threaded — so this is safe to call from inside the game thread too
 * (e.g. the native game-over transition). Exposed for src/port/overrides/gameplay.c. */
void pc_request_level_restart(void) {
    g_gameplay_entry = 0x577000u;
    g_enter_gameplay = 1;
    g_pc_restart_reinit = 1; /* re-decrunch overlay + re-pin card sentinels on restart */
    /* We're restarting into gameplay; set the screen so dispatch routes to the
     * gpl bank even if we somehow came from credits (the restart re-confirms it). */
    g_pc_screen = PC_SCR_GAMEPLAY;
}

static void do_retry_current_level(void) {
    pc_request_level_restart();
}

/* Called at the TOP of pc_step. If there's a pending menu action, perform it
 * now — main-loop context, never from inside the game coroutine. */
void pc_pause_tick(void) {
    if (s_pending_action == ACT_NONE) {
        return;
    }
    int act = s_pending_action;
    s_pending_action = ACT_NONE;
    benefactor_log_write(BENEFACTOR_LOG_INFO, "menu", "deferred menu action %d", act);
    switch (act) {
    case ACT_RESUME:
        s_paused = 0;
        break;
    case ACT_RETRY:
        s_paused = 0;
        do_retry_current_level();
        break;
    case ACT_EXIT_TO_MENU:
        s_paused = 0;
        pc_request_cold_restart();
        break;
    case ACT_QUIT:
        exit(0);
        break;
    }
}

/* ── View snapshot ─────────────────────────────────────────────────────────
 * Resolving a row into the words on screen is row-model work — it reads the
 * same config knobs and index helpers the cycling above writes — so it happens
 * here, and src/port/pause_menu_draw.c is handed the result. Each builder
 * fills the page's rows and its panel heading and returns the row count. */

static void view_row(PcPauseRow *row, const char *label, const char *value) {
    snprintf(row->label, sizeof row->label, "%s", label);
    if (value != NULL) {
        snprintf(row->value, sizeof row->value, "%s", value);
        row->has_value = 1;
    }
}

static void view_title(PcPauseView *view, const char *title) {
    snprintf(view->title, sizeof view->title, "%s", title);
}

static int build_main(PcPauseView *view) {
    static const char *k_main_labels[NUM_MAIN] = {
        "RESUME", "OPTIONS", "RETRY", "EXIT TO MAIN MENU", "QUIT TO DESKTOP",
    };
    view_title(view, "PAUSED");
    for (int i = 0; i < NUM_MAIN; i++) {
        view_row(&view->rows[i], k_main_labels[i], NULL);
    }
    return NUM_MAIN;
}

static int build_options(PcPauseView *view) {
    int rows[14];
    int n = options_rows(rows);
    view_title(view, "OPTIONS");
    for (int i = 0; i < n; i++) {
        PcPauseRow *row = &view->rows[i];
        switch (rows[i]) {
        case OO_GRAPHICS:
            view_row(row, "GRAPHICS", NULL); /* renderer/aspect/fullscreen/effects live inside */
            row->submenu = 1;
            break;
        case OO_SPEED:
            view_row(row, "GAME SPEED", k_speed_labels[speed_index()]);
            break;
        case OO_MATCH_DISPLAY:
            view_row(row, "MATCH DISPLAY", k_match_labels[match_index()]);
            break;
        case OO_PHYSICS:
            view_row(row, "JUMP PHYSICS",
                     pc_cfg_bool("platformer_physics", 0) ? "PLATFORMER" : "CLASSIC");
            break;
        case OO_FREECAM:
            view_row(row, "FREE CAM", pc_cfg_bool("freecam_pause", 0) ? "PAUSED" : "REALTIME");
            break;
        case OO_CONTROLS:
            view_row(row, "CONTROLS", NULL);
            row->submenu = 1;
            break;
        case OO_EXTRA:
            view_row(row, "EXTRA", NULL);
            row->submenu = 1;
            break;
        case OO_BACK:
            view_row(row, "BACK", NULL);
            break;
        case OO_QUIT:
            view_row(row, "QUIT TO DESKTOP", NULL);
            break;
        }
    }
    return n;
}

static int build_graphics(PcPauseView *view) {
    int rows[14];
    int n = graphics_rows(rows);
    const int hw = hardware_active();
    view_title(view, "GRAPHICS");
    for (int i = 0; i < n; i++) {
        PcPauseRow *row = &view->rows[i];
        switch (rows[i]) {
        case RR_RENDERER:
            view_row(row, "RENDERER", k_rend_labels[renderer_index()]);
            break;
        case RR_ASPECT:
            view_row(row, "ASPECT RATIO", k_aspect_labels[aspect_index()]);
            break;
        case RR_FULLSCREEN:
            view_row(row, "FULLSCREEN", pc_cfg_bool("fullscreen", 0) ? "ON" : "OFF");
            break;
        case RR_AMBIENT:
            view_row(row, "AMBIENT DARKNESS", pc_cfg_bool("fx_ambient", 0) ? "ON" : "OFF");
            row->disabled = !hw;
            break;
        case RR_SHADOW:
            view_row(row, "DROP SHADOW", pc_cfg_bool("fx_shadow", 0) ? "ON" : "OFF");
            row->disabled = !hw;
            break;
        case RR_BACK:
            view_row(row, "BACK", NULL);
            break;
        }
    }
    return n;
}

static int build_controls(PcPauseView *view) {
    int rows[14];
    int n = controls_rows(rows);
    view_title(view, "CONTROLS");
    for (int i = 0; i < n; i++) {
        PcPauseRow *row = &view->rows[i];
        char vbuf[24];
        switch (rows[i]) {
        case CT_INTERACT:
            view_row(row, "INTERACT RANGE", interact_enabled() ? "EXTENDED" : "VANILLA");
            break;
        case CT_MODERN_KB:
            view_row(row, "MODERN KEYBOARD", pc_modern_kb() ? "ON" : "OFF");
            break;
        case CT_MODERN_PAD:
            view_row(row, "MODERN CONTROLLER", pc_modern_pad() ? "ON" : "OFF");
            break;
        case CT_BIND_KB:
            view_row(row, "KEYBOARD BINDINGS", NULL);
            row->submenu = 1;
            break;
        case CT_BIND_PAD:
            if (hw_pad_count() == 0) {
                view_row(row, "CONTROLLER BINDINGS", "NONE FOUND");
            } else {
                snprintf(vbuf, sizeof vbuf, "%d PAD%s", hw_pad_count(),
                         hw_pad_count() > 1 ? "S" : "");
                view_row(row, "CONTROLLER BINDINGS", vbuf);
            }
            row->submenu = 1;
            break;
        case CT_BACK:
            view_row(row, "BACK", NULL);
            break;
        }
    }
    return n;
}

static int build_extra(PcPauseView *view) {
    int rows[14];
    int n = extra_rows(rows);
    view_title(view, "EXTRA");
    for (int i = 0; i < n; i++) {
        PcPauseRow *row = &view->rows[i];
        switch (rows[i]) {
        case EX_SKIP_INTRO:
            view_row(row, "SKIP INTRO", pc_cfg_bool("skip_intro", 0) ? "ON" : "OFF");
            break;
        case EX_UNLOCK_ALL:
            view_row(row, "UNLOCK ALL LEVELS", pc_cfg_bool("unlock_all_levels", 0) ? "ON" : "OFF");
            break;
        case EX_FALL_DMG:
            view_row(row, "FALL DAMAGE", k_fall_dmg_labels[fall_dmg_index()]);
            break;
        case EX_UPDATE: {
            const PcUpdateState state = pc_update_state();
            const char *value;
            if (state == PC_UPDATE_AVAILABLE) {
                value = pc_update_latest();
            } else if (state == PC_UPDATE_CHECKING) {
                value = "CHECKING";
            } else if (state == PC_UPDATE_FAILED) {
                value = "FAILED";
            } else {
                value = pc_cfg_bool("update_check", 1) ? "ON" : "OFF";
            }
            view_row(row, "UPDATE CHECK", value);
            break;
        }
        case EX_BACK:
            view_row(row, "BACK", NULL);
            break;
        }
    }
    return n;
}

/* Bindings page (keyboard / controller). The row being captured shows the
 * prompt in place of its current binding. */
static int build_bindings(PcPauseView *view) {
    int dev = (s_page == PG_BIND_PAD) ? PI_DEV_PAD : PI_DEV_KB;
    int acts[14];
    int n = bind_rows(dev, acts);
    view_title(view, dev == PI_DEV_PAD ? "CONTROLLER BINDINGS" : "KEYBOARD BINDINGS");
    pc_input_load();
    /* The capture prompt belongs to the row under the cursor, which the
     * clamping below may still move; clamp here so both agree. */
    int cursor = (s_cursor >= n) ? n - 1 : s_cursor; /* modern toggle may shrink the list */
    for (int i = 0; i < n; i++) {
        char val[64];
        const char *value = NULL;
        if (acts[i] >= 0) {
            if (s_capture && i == cursor) {
                value = dev == PI_DEV_PAD ? "PRESS A BUTTON..." : "PRESS A KEY...";
            } else {
                value = pc_input_binding_str(dev, acts[i], val, sizeof val);
            }
        }
        view_row(&view->rows[i], bind_row_label(dev, acts[i]), value);
    }
    return n;
}

int pc_pause_view_build(PcPauseView *view) {
    if (!s_paused) {
        return 0;
    }
    memset(view, 0, sizeof *view);
    view->page = s_page;
    switch (s_page) {
    case PG_MAIN:
        view->row_count = build_main(view);
        break;
    case PG_OPTIONS:
        view->row_count = build_options(view);
        break;
    case PG_GRAPHICS:
        view->row_count = build_graphics(view);
        break;
    case PG_CONTROLS:
        view->row_count = build_controls(view);
        break;
    case PG_EXTRA:
        view->row_count = build_extra(view);
        break;
    default:
        view->row_count = build_bindings(view);
        break;
    }
    /* A page whose row list shrank under the cursor (the modern-controls
     * toggle does this) leaves it past the end; pull it back, on the state
     * itself, so the next keypress moves from where the player sees it. */
    if (s_cursor >= view->row_count) {
        s_cursor = view->row_count - 1;
    }
    view->cursor = s_cursor;
    return 1;
}
