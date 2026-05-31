/* pc_pause_menu.c — ESC-triggered in-game pause menu.
 *
 * Drawn into the framebuffer AFTER native_render_frame composes the game's
 * frame, same hook point as pc_level_select_overlay (called from
 * hw_present_frame in hw.c).
 *
 * Menu options:
 *   RESUME              — clear pause flag, continue play
 *   RETRY               — restart current level at its title card (re-enter
 *                         gameplay_coro_entry with $20.w unchanged)
 *   EXIT TO MAIN MENU   — soft-reset back to cold-boot $3000 so the engine
 *                         walks intro/title and ends on the poster/menu
 *   QUIT TO DESKTOP     — exit(0)
 *
 * Pause is only available DURING gameplay (g_gameplay_active=1). Outside
 * gameplay ESC keeps its old "quit immediately" behaviour. Inside the
 * pause menu, ESC toggles back to Resume.
 *
 * Key-handler interactions (in src/recomp/hw.c hw_handle_key):
 *   - SDLK_ESCAPE during gameplay  → pc_pause_toggle()
 *   - SDLK_ESCAPE while paused     → pc_pause_toggle() (= Resume)
 *   - UP/DOWN/Fire while paused    → menu nav, not delivered to the game
 *
 * pc_step (in pc.c) consults pc_pause_active() — when set it skips the
 * coroutine swap so the game freezes. The frame is still rendered + presented
 * so the overlay stays visible. Deferred actions (Retry/Exit/Quit) fire from
 * pc_pause_tick(), called at the top of pc_step. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "game_state.h"   /* g_state + g_gameplay_active / g_credits_active /
                           * g_enter_gameplay / g_gameplay_entry macros */

extern void pc_fill_rect(uint32_t *fb, int x0, int y0, int w, int h, uint32_t argb);
extern int  pc_draw_text(uint32_t *fb, int x, int y, const char *s, int scale, uint32_t argb);

#define FB_W 352
#define FB_H 282

/* ── State ─────────────────────────────────────────────────────────────── */

static int s_paused = 0;
static int s_cursor = 0;                   /* 0..3 */
enum { OPT_RESUME = 0, OPT_RETRY, OPT_EXIT_TO_MENU, OPT_QUIT, NUM_OPTS = 4 };

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
        s_cursor = OPT_RESUME;
        s_pending_action = ACT_NONE;
    }
}

void pc_pause_input_up(void)   { if (s_paused) s_cursor = (s_cursor + NUM_OPTS - 1) % NUM_OPTS; }
void pc_pause_input_down(void) { if (s_paused) s_cursor = (s_cursor + 1) % NUM_OPTS; }
void pc_pause_input_select(void)
{
    if (!s_paused) return;
    switch (s_cursor) {
        case OPT_RESUME:        s_pending_action = ACT_RESUME;        break;
        case OPT_RETRY:         s_pending_action = ACT_RETRY;         break;
        case OPT_EXIT_TO_MENU:  s_pending_action = ACT_EXIT_TO_MENU;  break;
        case OPT_QUIT:          s_pending_action = ACT_QUIT;          break;
    }
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

static void do_retry_current_level(void)
{
    /* Same mechanism as the REPL `goto N` command: keep $20.w at its
     * current value (so the level table dispatcher picks this level),
     * point gameplay_entry at $577000, and ask pc_step_coro to restart
     * the coroutine. Naturally walks through level-card → cavern. */
    g_gameplay_entry = 0x577000u;
    g_enter_gameplay = 1;
    /* If the engine somehow left credits_active set (shouldn't, but be
     * defensive — restart drops us back into gameplay context). */
    g_credits_active = 0;
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

void pc_pause_menu_overlay(uint32_t *fb)
{
    if (!s_paused) return;

    /* Dim the background by overlaying ~50%-black across the whole frame.
     * Cheap "modal" feel without needing a separate compositor pass. */
    for (int i = 0; i < FB_W * FB_H; i++) {
        uint32_t p = fb[i];
        uint32_t r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
        r >>= 1; g >>= 1; b >>= 1;
        fb[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
    }

    /* Panel — sized for the longest option label "EXIT TO MAIN MENU" (17ch). */
    const int pw = 160;
    const int ph = 86;
    const int px = (FB_W - pw) / 2;
    const int py = (FB_H - ph) / 2;
    pc_fill_rect(fb, px, py, pw, ph, 0xFF101830);
    pc_fill_rect(fb, px,             py,            pw, 1, 0xFFFFD040);
    pc_fill_rect(fb, px,             py + ph - 1,   pw, 1, 0xFFFFD040);
    pc_fill_rect(fb, px,             py,             1, ph, 0xFFFFD040);
    pc_fill_rect(fb, px + pw - 1,    py,             1, ph, 0xFFFFD040);

    /* Header. */
    pc_draw_text(fb, px + 8, py + 6, "PAUSED", 1, 0xFFFFE070);

    static const char *labels[NUM_OPTS] = {
        "RESUME",
        "RETRY",
        "EXIT TO MAIN MENU",
        "QUIT TO DESKTOP",
    };

    const int row_h = 11;
    const int rows_y = py + 22;
    for (int i = 0; i < NUM_OPTS; i++) {
        int y = rows_y + i * row_h;
        uint32_t colour = (i == s_cursor) ? 0xFFFFFFFF : 0xFFB0B0C0;
        if (i == s_cursor)
            pc_draw_text(fb, px + 6, y, ">", 1, 0xFFFFE070);
        pc_draw_text(fb, px + 16, y, labels[i], 1, colour);
    }
}
