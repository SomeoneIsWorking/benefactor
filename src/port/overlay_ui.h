/* overlay_ui.h — the host's on-screen UI, drawn over the guest's framebuffer.
 *
 * The pause menu, the level picker, the toast line, the HUD icons and the free
 * camera are all host-owned overlays: the guest never knows they exist. They
 * share one contract — an `_overlay(uint32_t *fb)` entry point called by the
 * present path with the composed ARGB framebuffer, plus whatever input and
 * visibility queries that overlay needs.
 *
 * Every one of these used to be re-declared `extern` inside the function that
 * called it, in five different translation units. A declaration that lives next
 * to its caller cannot be checked against its definition, so this header is the
 * single place they are spelled.
 */
#pragma once

#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ── Overlay draw entry points (present path) ───────────────────────────── */
void pc_pause_menu_overlay(uint32_t *fb);
void pc_level_select_overlay(uint32_t *fb);
void pc_menu_subtext_overlay(uint32_t *fb);
void pc_hud_icons_overlay(uint32_t *fb);
void pc_toast_overlay(uint32_t *fb);

/* ── Overlay target size, in framebuffer pixels ─────────────────────────── */
void pc_overlay_set_dims(int w, int h);
int pc_overlay_w(void);
int pc_overlay_h(void);

/* ── Pause menu ─────────────────────────────────────────────────────────── */
int pc_pause_active(void);
void pc_pause_toggle(void);
void pc_pause_escape(void);
void pc_pause_tick(void);
void pc_pause_open_options(void);
void pc_pause_input_up(void);
void pc_pause_input_down(void);
void pc_pause_input_left(void);
void pc_pause_input_right(void);
void pc_pause_input_select(void);
/* Rebinding a control: the menu swallows the next raw device code. */
int pc_pause_capture_active(void);
void pc_pause_capture_code(int dev, int code);
/* Deferred flow actions the menu asks the host to perform at a safe boundary. */
void pc_request_level_restart(void);
extern int g_pc_restart_reinit;

/* ── Level picker and the main-menu subtext line ────────────────────────── */
extern int g_level_select_visible;
extern int g_pc_menu_visible;
extern int g_menu_continue_x, g_menu_continue_y;

/* ── Toast (a transient one-line message) ───────────────────────────────── */
void pc_toast_show(const char *msg, int is_error);
int pc_toast_visible(void);

/* ── Free camera ────────────────────────────────────────────────────────── */
int pc_freecam_active(void);
void pc_freecam_toggle(void);
void pc_freecam_tick(void);
int pc_freecam_x(void);
int pc_freecam_fade_alpha(void);
int pc_freecam_paused(void); /* free cam configured to freeze the game */
#ifdef __cplusplus
} /* extern "C" */
#endif
