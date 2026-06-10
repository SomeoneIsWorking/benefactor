/* pc_pause_menu.c — ESC-triggered in-game pause menu + OPTIONS submenus.
 *
 * Drawn into the framebuffer AFTER native_render_frame composes the game's
 * frame, same hook point as pc_level_select_overlay (called from
 * hw_present_frame in hw.c).
 *
 * Pages:
 *   MAIN     — RESUME / OPTIONS / RETRY / EXIT TO MAIN MENU / QUIT TO DESKTOP
 *   OPTIONS  — widescreen (disabled|16:9|ultrawide|auto), interact range,
 *              modern controls per device, links to the two bindings pages.
 *              Every change is applied LIVE and persisted to benefactor.json
 *              (pc_cfg_persist).
 *   BINDINGS — one page per device (keyboard / controller). Selecting a row
 *              enters CAPTURE: the next key/button pressed on that device
 *              becomes the binding (single key; chords stay JSON-editable).
 *              With modern controls ON for the device, the single FIRE row is
 *              split into JUMP and INTERACT (+ FIRE (THROW) and DROP).
 *
 * Pause is only available DURING gameplay (g_gameplay_active=1). Outside
 * gameplay ESC keeps its old "quit immediately" behaviour. Inside the menu,
 * ESC (or pad B/Start) backs out one page; on the main page it resumes.
 *
 * Key/button routing lives in hw.c (hw_handle_key / hw_handle_pad_code): while
 * paused, arrows/dpad navigate, fire/A selects, left-right cycles values, and
 * during capture every press is fed to pc_pause_capture_code instead.
 *
 * pc_step (in pc.c) consults pc_pause_active() — when set it skips the
 * coroutine swap so the game freezes. The frame is still rendered + presented
 * so the overlay stays visible. Deferred actions (Retry/Exit/Quit) fire from
 * pc_pause_tick(), called at the top of pc_step. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include "common/game_state.h"   /* g_state + g_gameplay_active / g_credits_active /
                           * g_enter_gameplay / g_gameplay_entry macros */
#include "port/config.h"
#include "port/input.h"
#include <SDL2/SDL.h>            /* SDLK_/SDL_CONTROLLER_ constants only */

extern void pc_fill_rect(uint32_t *fb, int x0, int y0, int w, int h, uint32_t argb);
extern int  pc_draw_text(uint32_t *fb, int x, int y, const char *s, int scale, uint32_t argb);
extern void hw_widescreen_refresh(void);
extern int  hw_pad_count(void);

#define FB_W 352
#define FB_H 282

/* ── State ─────────────────────────────────────────────────────────────────── */

enum { PG_MAIN = 0, PG_OPTIONS, PG_MORE, PG_BIND_KB, PG_BIND_PAD };

static int s_paused = 0;
static int s_page   = PG_MAIN;
static int s_cursor = 0;                   /* per current page */
/* Opened from OUTSIDE gameplay (title/menu/intro, via ESC/Start): the menu
 * starts on the OPTIONS page, the MAIN page (Resume/Retry/Exit) is never
 * shown, and a QUIT row is appended (ESC no longer quits directly there). */
static int s_title_mode = 0;

enum { OPT_RESUME = 0, OPT_OPTIONS, OPT_RETRY, OPT_EXIT_TO_MENU, OPT_QUIT, NUM_MAIN };
/* OPTIONS-page row ids (rows are built per-mode by options_rows). */
enum { OO_WIDESCREEN = 0, OO_SPEED, OO_FREECAM, OO_INTERACT, OO_MODERN_KB,
       OO_MODERN_PAD, OO_BIND_KB, OO_BIND_PAD, OO_MORE, OO_BACK, OO_QUIT };
/* MORE-page row ids. */
enum { MO_SKIP_INTRO = 0, MO_UNLOCK_ALL, MO_BACK };

/* Bindings capture: which device/action the next press is assigned to. */
static int s_capture = 0, s_capture_dev = 0, s_capture_action = 0;

/* Deferred action — applied at the top of the next pc_step() so we never
 * tear down the coroutine while we're inside its yield handler. */
enum { ACT_NONE = 0, ACT_RESUME, ACT_RETRY, ACT_EXIT_TO_MENU, ACT_QUIT };
static int s_pending_action = ACT_NONE;

int pc_pause_active(void)
{
    return s_paused;
}

void pc_pause_toggle(void)
{
    if (s_paused) {
        s_pending_action = ACT_RESUME;
    } else {
        s_paused = 1;
        s_title_mode = 0;
        s_page   = PG_MAIN;
        s_cursor = OPT_RESUME;
        s_capture = 0;
        s_pending_action = ACT_NONE;
    }
}

/* ESC/Start OUTSIDE gameplay: open straight into the OPTIONS page (the game/
 * attract loop freezes exactly like the in-game pause; closing resumes it). */
void pc_pause_open_options(void)
{
    if (s_paused) return;
    s_paused = 1;
    s_title_mode = 1;
    s_page   = PG_OPTIONS;
    s_cursor = 0;
    s_capture = 0;
    s_pending_action = ACT_NONE;
}

/* ── Bindings-page row model ───────────────────────────────────────────────────
 * ONE FIRE and (with modern controls) ONE INTERACT — nothing else. There is no
 * DROP row: dropping the carried item/man is always interact+Down. The hop and
 * drop ACTIONS still exist for JSON power users but get no menu row and no
 * defaults. Last row is BACK (action = -1). */
static int bind_rows(int dev, int *actions /* >= 14 */)
{
    int n = 0;
    int modern = (dev == PI_DEV_PAD) ? pc_modern_pad() : pc_modern_kb();
    actions[n++] = PI_LEFT; actions[n++] = PI_RIGHT;
    actions[n++] = PI_UP;   actions[n++] = PI_DOWN;
    actions[n++] = PI_FIRE;
    if (modern) actions[n++] = PI_INTERACT;
    actions[n++] = PI_FFWD;           /* hold-to-fast-forward, both schemes */
    actions[n++] = PI_FREECAM;        /* free-cam toggle */
    actions[n++] = -1;                /* BACK */
    return n;
}

static const char *bind_row_label(int dev, int action)
{
    (void)dev;
    if (action < 0) return "BACK";
    return pc_input_action_name(action);
}

/* OPTIONS-page rows, built per mode: title mode appends QUIT (there is no MAIN
 * page outside gameplay, and ESC no longer quits directly — it opens this). */
static int options_rows(int *rows /* >= 14 */)
{
    int n = 0;
    rows[n++] = OO_WIDESCREEN; rows[n++] = OO_SPEED;     rows[n++] = OO_FREECAM;
    rows[n++] = OO_INTERACT;   rows[n++] = OO_MODERN_KB; rows[n++] = OO_MODERN_PAD;
    rows[n++] = OO_BIND_KB;    rows[n++] = OO_BIND_PAD;  rows[n++] = OO_MORE;
    rows[n++] = OO_BACK;
    if (s_title_mode) rows[n++] = OO_QUIT;
    return n;
}

static int more_rows(int *rows /* >= 14 */)
{
    int n = 0;
    rows[n++] = MO_SKIP_INTRO;
    rows[n++] = MO_UNLOCK_ALL;
    rows[n++] = MO_BACK;
    return n;
}

static int page_rows(int page)
{
    int acts[14];
    switch (page) {
        case PG_MAIN:    return NUM_MAIN;
        case PG_OPTIONS: return options_rows(acts);
        case PG_MORE:    return more_rows(acts);
        default:         return bind_rows(page == PG_BIND_PAD, acts);
    }
}

static void enter_page(int page)
{
    s_page = page;
    s_cursor = 0;
}

/* ── Option values (resolved live, persisted on change) ───────────────────────── */

static const char *k_ws_modes[4]  = { "disabled", "16:9", "ultrawide", "auto" };
static const char *k_ws_labels[4] = { "DISABLED", "16:9", "ULTRAWIDE", "AUTO" };

static int ws_mode_index(void)
{
    char buf[24];
    if (!pc_cfg_show("widescreen_mode", buf, sizeof buf, NULL) || !buf[0]) return 0;
    for (int i = 0; i < 4; i++) if (!strcmp(buf, k_ws_modes[i])) return i;
    return 0;
}

static void ws_mode_set(int idx)
{
    char json[24];
    snprintf(json, sizeof json, "\"%s\"", k_ws_modes[(idx % 4 + 4) % 4]);
    pc_cfg_persist("widescreen_mode", json);
    hw_widescreen_refresh();
}

/* Game speed: normal / turbo (=1.2x) / hyper (=1.5x). Audio/music stay
 * real-time regardless; the hold-to-fast-forward binding (5x) is separate. */
#define NUM_SPEEDS 3
static const char *k_speed_vals[NUM_SPEEDS]   = { "normal", "turbo", "hyper" };
static const char *k_speed_labels[NUM_SPEEDS] = { "NORMAL", "TURBO (1.2X)", "HYPER (1.5X)" };

static int speed_index(void)
{
    char buf[16];
    if (!pc_cfg_show("game_speed", buf, sizeof buf, NULL) || !buf[0]) return 0;
    for (int i = 1; i < NUM_SPEEDS; i++)
        if (!strcasecmp(buf, k_speed_vals[i])) return i;
    return 0;
}

static void speed_set(int idx)
{
    extern void hw_speed_refresh(void);
    char json[16];
    snprintf(json, sizeof json, "\"%s\"",
             k_speed_vals[(idx % NUM_SPEEDS + NUM_SPEEDS) % NUM_SPEEDS]);
    pc_cfg_persist("game_speed", json);
    hw_speed_refresh();
}

/* "Extend interaction range": disabled / enabled (enabled = 5 px). */
#define INTERACT_EXTEND_ON 5
static int  interact_enabled(void) { return pc_cfg_int("interact_extend", 0) > 0; }
static void interact_set(int on)   { pc_cfg_persist("interact_extend", on ? "5" : "0"); }

static void modern_set(int dev, int on)
{
    pc_cfg_persist(dev == PI_DEV_PAD ? "modern_controls_controller"
                                     : "modern_controls_keyboard",
                   on ? "true" : "false");
}

/* Toggle a persisted bool knob. */
static void bool_knob_toggle(const char *key)
{
    pc_cfg_persist(key, pc_cfg_bool(key, 0) ? "false" : "true");
}

/* Cycle an OPTIONS row's value by `dir` (+1 right/select, -1 left). */
static void options_cycle(int row, int dir)
{
    switch (row) {
        case OO_WIDESCREEN: ws_mode_set(ws_mode_index() + dir);        break;
        case OO_SPEED:      speed_set(speed_index() + dir);            break;
        case OO_FREECAM:    bool_knob_toggle("freecam_pause");         break;
        case OO_INTERACT:   interact_set(!interact_enabled());         break;
        case OO_MODERN_KB:  modern_set(PI_DEV_KB,  !pc_modern_kb());   break;
        case OO_MODERN_PAD: modern_set(PI_DEV_PAD, !pc_modern_pad());  break;
        default: break;
    }
}

static void more_cycle(int row)
{
    switch (row) {
        case MO_SKIP_INTRO: bool_knob_toggle("skip_intro");        break;
        case MO_UNLOCK_ALL: bool_knob_toggle("unlock_all_levels"); break;
        default: break;
    }
}

/* ── Navigation (called from hw.c while paused) ───────────────────────────────── */

void pc_pause_input_up(void)
{
    if (!s_paused || s_capture) return;
    int n = page_rows(s_page);
    s_cursor = (s_cursor + n - 1) % n;
}

void pc_pause_input_down(void)
{
    if (!s_paused || s_capture) return;
    int n = page_rows(s_page);
    s_cursor = (s_cursor + 1) % n;
}

void pc_pause_input_left(void)
{
    if (!s_paused || s_capture) return;
    if (s_page == PG_OPTIONS) {
        int rows[14]; int n = options_rows(rows);
        if (s_cursor < n) options_cycle(rows[s_cursor], -1);
    } else if (s_page == PG_MORE) {
        int rows[14]; int n = more_rows(rows);
        if (s_cursor < n) more_cycle(rows[s_cursor]);
    }
}

void pc_pause_input_right(void)
{
    if (!s_paused || s_capture) return;
    if (s_page == PG_OPTIONS) {
        int rows[14]; int n = options_rows(rows);
        if (s_cursor < n) options_cycle(rows[s_cursor], +1);
    } else if (s_page == PG_MORE) {
        int rows[14]; int n = more_rows(rows);
        if (s_cursor < n) more_cycle(rows[s_cursor]);
    }
}

/* Cursor restore helper: position the OPTIONS cursor on a given row id. */
static void enter_options_at(int row_id)
{
    int rows[14]; int n = options_rows(rows);
    enter_page(PG_OPTIONS);
    for (int i = 0; i < n; i++) if (rows[i] == row_id) { s_cursor = i; break; }
}

void pc_pause_input_select(void)
{
    if (!s_paused || s_capture) return;
    switch (s_page) {
    case PG_MAIN:
        switch (s_cursor) {
            case OPT_RESUME:        s_pending_action = ACT_RESUME;        break;
            case OPT_OPTIONS:       enter_page(PG_OPTIONS);               break;
            case OPT_RETRY:         s_pending_action = ACT_RETRY;         break;
            case OPT_EXIT_TO_MENU:  s_pending_action = ACT_EXIT_TO_MENU;  break;
            case OPT_QUIT:          s_pending_action = ACT_QUIT;          break;
        }
        break;
    case PG_OPTIONS: {
        int rows[14]; int n = options_rows(rows);
        if (s_cursor >= n) s_cursor = n - 1;
        switch (rows[s_cursor]) {
            case OO_BIND_KB:  enter_page(PG_BIND_KB);  break;
            case OO_BIND_PAD: enter_page(PG_BIND_PAD); break;
            case OO_MORE:     enter_page(PG_MORE);     break;
            case OO_BACK:
                if (s_title_mode) s_pending_action = ACT_RESUME;   /* close */
                else { enter_page(PG_MAIN); s_cursor = OPT_OPTIONS; }
                break;
            case OO_QUIT:     s_pending_action = ACT_QUIT;        break;
            default:          options_cycle(rows[s_cursor], +1);  break;
        }
        break;
    }
    case PG_MORE: {
        int rows[14]; int n = more_rows(rows);
        if (s_cursor >= n) s_cursor = n - 1;
        if (rows[s_cursor] == MO_BACK) enter_options_at(OO_MORE);
        else                           more_cycle(rows[s_cursor]);
        break;
    }
    case PG_BIND_KB:
    case PG_BIND_PAD: {
        int acts[14];
        int dev = (s_page == PG_BIND_PAD) ? PI_DEV_PAD : PI_DEV_KB;
        int n = bind_rows(dev, acts);
        if (s_cursor >= n) s_cursor = n - 1;
        if (acts[s_cursor] < 0) {                       /* BACK */
            enter_options_at(dev == PI_DEV_PAD ? OO_BIND_PAD : OO_BIND_KB);
        } else {
            s_capture = 1;
            s_capture_dev = dev;
            s_capture_action = acts[s_cursor];
        }
        break;
    }
    }
}

/* ESC / pad B: cancel capture, back out one page, or resume from the main page.
 * In title mode (opened via ESC/Start outside gameplay) OPTIONS is the root —
 * backing out of it closes the menu. */
void pc_pause_escape(void)
{
    if (!s_paused) return;
    if (s_capture) { s_capture = 0; return; }
    switch (s_page) {
        case PG_BIND_KB:  enter_options_at(OO_BIND_KB);   break;
        case PG_BIND_PAD: enter_options_at(OO_BIND_PAD);  break;
        case PG_MORE:     enter_options_at(OO_MORE);      break;
        case PG_OPTIONS:
            if (s_title_mode) s_pending_action = ACT_RESUME;
            else { enter_page(PG_MAIN); s_cursor = OPT_OPTIONS; }
            break;
        default:          s_pending_action = ACT_RESUME;  break;
    }
}

/* ── Bindings capture ─────────────────────────────────────────────────────────── */

int pc_pause_capture_active(void) { return s_paused && s_capture; }

void pc_pause_capture_code(int dev, int code)
{
    if (!pc_pause_capture_active()) return;
    if (dev == PI_DEV_KB && code == SDLK_ESCAPE) { s_capture = 0; return; }
    if (dev != s_capture_dev) return;          /* press must come from the device being bound */
    if (dev == PI_DEV_PAD && code == SDL_CONTROLLER_BUTTON_START) return;  /* reserved: pause */
    pc_input_rebind(dev, s_capture_action, code);
    s_capture = 0;
}

/* ── Action handlers (called from pc_pause_tick, on the main loop) ──────── */

/* g_gameplay_active / g_overlay_active / g_credits_active / g_enter_gameplay /
 * g_gameplay_entry are all g_state members — macro-aliased via game_state.h. */

/* Forward declarations from pc.c — needed for "Exit to main menu" soft-reset.
 * We rebuild the game coroutine at $3000 (cold-start), which requires:
 *   1. clearing transient game state (pc_state_reset_defaults — declared
 *      static in pc.c, so we re-implement minimal clearing here);
 *   2. re-decrunching the boot loader at $3000 in case the overlay loader
 *      (d0=0/2/3) overwrote it; and
 *   3. swapping the coroutine to point at game_coro_entry.
 *
 * To keep this file free of pc.c internals, exposes one helper. */
extern void pc_request_cold_restart(void);

/* Restart the CURRENT level from the top (→ level card → cavern). Same
 * mechanism as the REPL `goto N` command: keep $20.w at its current value (so
 * the level table dispatcher picks this level), point gameplay_entry at $577000,
 * and ask pc_step_threaded to respawn the game thread there. Only sets flags —
 * the actual thread teardown/respawn happens on the main loop in
 * pc_step_threaded — so this is safe to call from inside the game thread too
 * (e.g. the native game-over transition). Exposed for pc_overrides_gameplay.c. */
void pc_request_level_restart(void)
{
    extern int g_pc_restart_reinit;
    g_gameplay_entry = 0x577000u;
    g_enter_gameplay = 1;
    g_pc_restart_reinit = 1;   /* re-decrunch overlay + re-pin card sentinels on restart */
    /* We're restarting into gameplay; set the screen so dispatch routes to the
     * gpl bank even if we somehow came from credits (the restart re-confirms it). */
    g_pc_screen = PC_SCR_GAMEPLAY;
}

static void do_retry_current_level(void)
{
    pc_request_level_restart();
}

/* Called at the TOP of pc_step. If there's a pending menu action, perform it
 * now — main-loop context, never from inside the game coroutine. */
void pc_pause_tick(void)
{
    if (s_pending_action == ACT_NONE) return;
    int act = s_pending_action;
    s_pending_action = ACT_NONE;
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

/* ── Overlay rendering ─────────────────────────────────────────────────── */

static void draw_panel(uint32_t *fb, int px, int py, int pw, int ph, const char *title)
{
    pc_fill_rect(fb, px, py, pw, ph, 0xFF101830);
    pc_fill_rect(fb, px,          py,          pw, 1, 0xFFFFD040);
    pc_fill_rect(fb, px,          py + ph - 1, pw, 1, 0xFFFFD040);
    pc_fill_rect(fb, px,          py,           1, ph, 0xFFFFD040);
    pc_fill_rect(fb, px + pw - 1, py,           1, ph, 0xFFFFD040);
    pc_draw_text(fb, px + 8, py + 6, title, 1, 0xFFFFE070);
}

static void draw_row(uint32_t *fb, int px, int y, int selected,
                     const char *label, const char *value)
{
    uint32_t colour = selected ? 0xFFFFFFFF : 0xFFB0B0C0;
    if (selected) pc_draw_text(fb, px + 6, y, ">", 1, 0xFFFFE070);
    pc_draw_text(fb, px + 16, y, label, 1, colour);
    if (value) pc_draw_text(fb, px + 150, y, value, 1, selected ? 0xFFFFE070 : 0xFF90A0D0);
}

void pc_pause_menu_overlay(uint32_t *fb)
{
    if (!s_paused) return;

    /* Dim the background by overlaying ~50%-black across the whole frame. Use the live
     * overlay target size (the wide output), so the dim spans the full widescreen view. */
    extern int pc_overlay_w(void), pc_overlay_h(void);
    const int ow = pc_overlay_w(), oh = pc_overlay_h();
    for (int i = 0; i < ow * oh; i++) {
        uint32_t p = fb[i];
        uint32_t r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
        r >>= 1; g >>= 1; b >>= 1;
        fb[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
    }

    const int row_h = 11;

    if (s_page == PG_MAIN) {
        /* Panel — sized for the longest option label "EXIT TO MAIN MENU" (17ch). */
        const int pw = 160;
        const int ph = 22 + NUM_MAIN * row_h + 8;
        const int px = (ow - pw) / 2, py = (oh - ph) / 2;
        draw_panel(fb, px, py, pw, ph, "PAUSED");
        static const char *labels[NUM_MAIN] = {
            "RESUME", "OPTIONS", "RETRY", "EXIT TO MAIN MENU", "QUIT TO DESKTOP",
        };
        for (int i = 0; i < NUM_MAIN; i++)
            draw_row(fb, px, py + 22 + i * row_h, i == s_cursor, labels[i], NULL);
        return;
    }

    if (s_page == PG_OPTIONS) {
        int rows[14]; int n = options_rows(rows);
        if (s_cursor >= n) s_cursor = n - 1;
        const int pw = 230;
        const int ph = 22 + n * row_h + 8;
        const int px = (ow - pw) / 2, py = (oh - ph) / 2;
        draw_panel(fb, px, py, pw, ph, "OPTIONS");
        for (int i = 0; i < n; i++) {
            const char *label = "", *value = NULL;
            char vbuf[24];
            switch (rows[i]) {
            case OO_WIDESCREEN:
                label = "WIDESCREEN";
                value = k_ws_labels[ws_mode_index()];
                break;
            case OO_SPEED:
                label = "GAME SPEED";
                value = k_speed_labels[speed_index()];
                break;
            case OO_FREECAM:
                label = "FREE CAM";
                value = pc_cfg_bool("freecam_pause", 0) ? "PAUSED" : "REALTIME";
                break;
            case OO_INTERACT:
                label = "INTERACT RANGE";
                value = interact_enabled() ? "EXTENDED" : "VANILLA";
                break;
            case OO_MODERN_KB:
                label = "MODERN KEYBOARD";
                value = pc_modern_kb() ? "ON" : "OFF";
                break;
            case OO_MODERN_PAD:
                label = "MODERN CONTROLLER";
                value = pc_modern_pad() ? "ON" : "OFF";
                break;
            case OO_BIND_KB:
                label = "KEYBOARD BINDINGS";
                break;
            case OO_BIND_PAD:
                label = "CONTROLLER BINDINGS";
                if (hw_pad_count() == 0) { value = "NONE FOUND"; }
                else { snprintf(vbuf, sizeof vbuf, "%d PAD%s", hw_pad_count(),
                                hw_pad_count() > 1 ? "S" : ""); value = vbuf; }
                break;
            case OO_MORE:
                label = "MORE";
                break;
            case OO_BACK:
                label = "BACK";
                break;
            case OO_QUIT:
                label = "QUIT TO DESKTOP";
                break;
            }
            draw_row(fb, px, py + 22 + i * row_h, i == s_cursor, label, value);
        }
        return;
    }

    if (s_page == PG_MORE) {
        int rows[14]; int n = more_rows(rows);
        if (s_cursor >= n) s_cursor = n - 1;
        const int pw = 230;
        const int ph = 22 + n * row_h + 8;
        const int px = (ow - pw) / 2, py = (oh - ph) / 2;
        draw_panel(fb, px, py, pw, ph, "MORE");
        for (int i = 0; i < n; i++) {
            const char *label = "", *value = NULL;
            switch (rows[i]) {
            case MO_SKIP_INTRO:
                label = "SKIP INTRO";
                value = pc_cfg_bool("skip_intro", 0) ? "ON" : "OFF";
                break;
            case MO_UNLOCK_ALL:
                label = "UNLOCK ALL LEVELS";
                value = pc_cfg_bool("unlock_all_levels", 0) ? "ON" : "OFF";
                break;
            case MO_BACK:
                label = "BACK";
                break;
            }
            draw_row(fb, px, py + 22 + i * row_h, i == s_cursor, label, value);
        }
        return;
    }

    /* Bindings page (keyboard / controller). */
    {
        int dev = (s_page == PG_BIND_PAD) ? PI_DEV_PAD : PI_DEV_KB;
        int acts[14];
        int n = bind_rows(dev, acts);
        if (s_cursor >= n) s_cursor = n - 1;   /* modern toggle may shrink the list */
        const int pw = 300;   /* room for multi-chord defaults ("Z, LCtrl, Space, Return") */
        const int ph = 22 + n * row_h + 8;
        const int px = (ow - pw) / 2, py = (oh - ph) / 2;
        draw_panel(fb, px, py, pw, ph,
                   dev == PI_DEV_PAD ? "CONTROLLER BINDINGS" : "KEYBOARD BINDINGS");
        pc_input_load();
        for (int i = 0; i < n; i++) {
            char val[64];
            const char *value = NULL;
            if (acts[i] >= 0) {
                if (s_capture && i == s_cursor)
                    value = dev == PI_DEV_PAD ? "PRESS A BUTTON..." : "PRESS A KEY...";
                else
                    value = pc_input_binding_str(dev, acts[i], val, sizeof val);
            }
            draw_row(fb, px, py + 22 + i * row_h, i == s_cursor,
                     bind_row_label(dev, acts[i]), value);
        }
    }
}
