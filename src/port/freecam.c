/* pc_freecam.c — detachable FREE CAMERA for the widescreen (BenRen) renderer.
 *
 * Toggled by the PI_FREECAM binding (default: keyboard C / pad Back), gameplay
 * only. While active the camera follow-point is s_x instead of the engine
 * camera — ws_view_left (native_renderer.c) substitutes it, so the wide bg,
 * every sprite pass AND the object-cull overrides all pan together; the level
 * clamps there still apply, so the cam can never leave the world. Toggling off
 * simply reverts ws_view_left to the engine camera (instant snap back).
 *
 * Input while active: LEFT/RIGHT pan the camera (hw.c routes them here and
 * zeroes the engine joystick/buttons so the player stands idle). By default
 * the game KEEPS RUNNING; the "freecam_pause" knob (OPTIONS menu) instead
 * freezes the game like the pause menu while panning (pc_step consults
 * pc_freecam_paused()).
 *
 * Free cam needs the wide BenRen view: with the vanilla 352px renderer the
 * playfield is the engine's own blit and cannot show other world columns, so
 * the toggle refuses with a toast there. Horizontal only — the engine camera
 * ($57FDBA) is X-only; levels do not scroll vertically. */
#include <stdint.h>
#include "port/config.h"
#include "port/input.h"
#include "render/engine_view.h"
#include "common/game_state.h"   /* g_gameplay_active */

extern int  hw_output_width(void);
extern void pc_toast_show(const char *msg, int is_error);
extern int  ws_view_left(int ow);   /* for HW_DISPLAY_W reference only */

#define FREECAM_STEP      6     /* px per frame */
#define FREECAM_STEP_FAST 16    /* px per frame with fast-forward held */

static int s_active = 0;
static int s_x = 0;             /* camera follow-point (world X, like cam+16) */

int pc_freecam_active(void) { return s_active; }
int pc_freecam_x(void)      { return s_x; }

/* Game frozen while panning? Only when active AND the user picked pause mode. */
int pc_freecam_paused(void)
{
    return s_active && pc_cfg_bool("freecam_pause", 0);
}

void pc_freecam_toggle(void)
{
    if (s_active) { s_active = 0; return; }      /* off: snap back to engine cam */
    if (!g_gameplay_active) return;
    if (hw_output_width() <= 352) {              /* vanilla view can't pan */
        pc_toast_show("FREE CAM NEEDS WIDESCREEN", 1);
        return;
    }
    EngineView ev;
    if (!engine_view_capture(&ev)) return;
    s_x = ev.camera + 16;                        /* start at the engine's follow-point */
    s_active = 1;
}

/* Per-frame pan, driven from the held direction actions (any device). Called
 * from hw_present_frame so it also runs while freecam_pause freezes pc_step. */
void pc_freecam_tick(void)
{
    if (!s_active) return;
    if (!g_gameplay_active) { s_active = 0; return; }   /* left gameplay: drop out */
    int step = pc_input_active(PI_FFWD) ? FREECAM_STEP_FAST : FREECAM_STEP;
    if (pc_input_active(PI_LEFT))  s_x -= step;
    if (pc_input_active(PI_RIGHT)) s_x += step;
    /* Clamp to the engine's own camera range (same bounds ws_view_left uses),
     * so the icon never points at void. */
    EngineView ev;
    if (engine_view_capture(&ev)) {
        int lo = (ev.level_lo < 0 ? 0 : ev.level_lo) + 16;
        int hi = ev.level_hi + 304;   /* level_hi + 320 - 16: highest follow-point */
        if (s_x < lo) s_x = lo;
        if (s_x > hi) s_x = hi;
    }
}

/* REPL support (harness `fcam`): force-enable + nudge, for headless testing. */
void pc_freecam_debug(int on, int dx)
{
    if (on && !s_active) { pc_freecam_toggle(); }
    if (!on && s_active) { s_active = 0; }
    s_x += dx;
}
