/*
 * recomp/hw.h  –  PC hardware abstraction for the recompiled game
 *
 * Replaces Amiga OCS/CIA register accesses with SDL2 equivalents:
 *
 *   Custom chip writes → SDL2 display / audio commands
 *   Custom chip reads  → synthetic beam counter, etc.
 *   CIA registers      → SDL2 timer, keyboard input
 *   Disk Load calls    → direct fread from WHDLoad disk images
 */
#pragma once
#include <stdint.h>
#include <SDL2/SDL.h>

/* ── Memory-mapped I/O (called from rt.c) ─────────────────────────────────── */

uint8_t  hw_read8 (uint32_t addr);
uint16_t hw_read16(uint32_t addr);
uint32_t hw_read32(uint32_t addr);
void     hw_write8 (uint32_t addr, uint8_t  v);
void     hw_write16(uint32_t addr, uint16_t v);
void     hw_write32(uint32_t addr, uint32_t v);

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */

int  hw_init(const char *title, const char **disk_paths, int n_disks);
void hw_request_headless(void);  /* call before hw_init: render fb but open no window */
void hw_fini(void);

/* Frame watchdog: arm before a single-frame step; if the frame hangs longer than
 * `seconds`, SIGALRM reports the likely cause (last hw register read + cop1lc)
 * and kills the app. Disarm after the frame returns. */
void hw_watchdog_arm(const char *what, int seconds);
void hw_watchdog_disarm(void);

/* Current INTENA interrupt-enable shadow ($DFF09A). */
uint16_t hw_get_intena(void);

/* ── Vsync / frame pacing ──────────────────────────────────────────────────── */

/*
 * Call once per game frame.  Pumps SDL events, presents the rendered
 * framebuffer, and sleeps until the next 50 Hz tick.
 * Returns 0 to keep running, 1 if the user closed the window.
 */
int hw_present_frame(void);

/* Get pointer to the internal ARGB8888 framebuffer (HW_DISPLAY_W × HW_DISPLAY_H pixels). */
const uint32_t *hw_get_framebuffer(void);
int hw_get_frame_num(void);

/* Render nsamples of stereo (interleaved L,R) PCM at 22050 Hz from the current
 * audio channel state. For offline capture/comparison (e.g. the harness). */
void hw_audio_render(short *buf, int nsamples);
/* Push nframes stereo samples to the audio device (queue/push output mode). */
void hw_audio_queue(const short *buf, int nframes);

/* Native SFX voice (ch 0/1): play a whole sample in one shot from the descriptor,
 * bypassing the per-frame chunk-follow that truncated/quieted SFX. See hw_audio.c. */
void hw_audio_sfx_play(int ch, uint32_t ptr, int len_bytes, int period,
                       uint8_t vol, uint32_t loop_ptr, int loop_len);
void hw_audio_sfx_stop(int ch);

/* Get/set current input state (shared between harness and hw.c). */
int  hw_get_fire(void);
int  hw_get_mouse_lmb(void);
int  hw_get_interact(void);          /* dedicated interact key (X/LShift), not fire */
void hw_set_interact(int on);
int  hw_get_drop(void);              /* dedicated DROP button — one of the drop bindings */
void hw_set_drop(int on);
int  hw_get_hop(void);               /* HOP action — separate from the Up direction */
void hw_set_hop(int on);
void hw_set_fire(int on);
void hw_set_mouse_lmb(int on);
void hw_set_joy_down(int on);        /* force the down direction (for the drop→fire+down bridge) */
void hw_set_joy_up(int on);          /* force the up direction (for the hop/up gate) */
void hw_set_no_pace(int on);
void hw_set_external_input(int on);
void hw_set_joystick(int up, int down, int left, int right, int fire);
int  hw_joy_up(void);
int  hw_joy_down(void);
int  hw_joy_left(void);
int  hw_joy_right(void);
/* Single shared keyboard→input mapper (SDL keysym). Used by both the standalone
 * and the harness so there is one input path. */
void hw_handle_key(int sym, int down);
/* Cycle the standalone's real-time speed (1x/2x/4x) — TAB key. */
void hw_cycle_speed(void);

/* ── Disk load (replaces ILLEGAL intercept) ────────────────────────────────── */

/*
 * Load `len` bytes from disk `disk` (1-based) at byte offset `offset`
 * into g_mem at Amiga address `dst`.
 */
int hw_load_disk(int disk, uint32_t offset, uint32_t len, uint32_t dst_amiga);

/* ── Framebuffer: written by the copper/display logic ─────────────────────── */

/* Width and height match PAL OCS display window */
/* Native display width. The Amiga playfield is ~320 lores px, but title/credits
 * panels and the in-game HUD extend into horizontal overscan; render ~10% wider
 * (352) so they aren't clipped. The renderer fills any area beyond the active
 * playfield with border colour, so widening never corrupts a screen. */
#define HW_DISPLAY_W  352
#define HW_DISPLAY_H  282

/* Widescreen output: the engine renders the 4:3 playfield into s_fb at
 * HW_DISPLAY_W; the OUTPUT surface may be wider (HW_OUT_MAX cap). The 352-wide
 * content is composited centered into the output, the L/R margins start as black
 * pillarbox (Phase 2) and later carry native-rendered world (Phase 3+). The
 * harness/comparison path keeps using the 352-wide s_fb directly. */
#define HW_OUT_MAX    960
int  hw_output_width(void);                 /* current output width (>=HW_DISPLAY_W) */
const uint32_t *hw_get_output_framebuffer(void); /* wide composited surface (HW_OUT × H) */

/*
 * Called by the copper emulation (or replaced copper logic) to set one
 * pixel in the output framebuffer (ARGB8888).
 */
void hw_set_pixel(int x, int y, uint32_t argb);

/* Palette: 32 entries × 12-bit Amiga colour → ARGB32 */
void hw_set_color(int idx, uint16_t amiga_rgb12);

/* Set a frame limit for auto-exit (0 = unlimited).  Useful for testing. */
void hw_set_frame_limit(int frames);

/* Global run flag – set to 0 to request graceful exit */
extern int hw_running;

/* ── Input ─────────────────────────────────────────────────────────────────── */

/* Returns JOY0DAT-style value: bits [1]=up,[3]=down,[9]=left,[11]=right,[8]=fire */
uint16_t hw_joystick(void);

/* CIA-A keyboard byte (parallel data register) – 0xFF if no key */
uint8_t hw_cia_keyboard(void);

/* Programmatic input injection for testing/REPL */
void hw_set_fire(int pressed);
void hw_set_mouse_lmb(int pressed);

/* ── Recompiler helpers ────────────────────────────────────────────────────── */

/* Read VPOSR byte (returns bit 0 = frame parity) */
uint8_t hw_vposr_read(void);

/* Return BZERO state: 1 = blit complete, 0 = blitter busy */
int hw_blitter_bzero(void);

/* Block until blitter BZERO becomes 1 (blit complete) */
void hw_blitter_sync(void);

/* Block until next VPOSR frame boundary toggle */
void hw_vsync(void);

/* Wait for vblank (Amiga VPOSR sync pair: bit0=0 then bit0=1).
 * On PC this is a no-op — frame timing is driven by SDL in hw_present_frame(). */
void hw_vblank_wait(void);

/* Yield-per-frame fire-wait — replaces the folded `tst.b $bfe001; b(mi|pl) self`
 * busy-loop. want_pressed=1: block until fire is pressed; =0: until released. */
void hw_wait_fire(int want_pressed);

/* ── Harness hooks (used only when building benefactor-harness) ───────────── */

/* Set this to a callback that fires at the end of every hw_present_frame().
 * NULL (default) = disabled.  Used by the comparison harness. */
extern void (*g_harness_frame_hook)(void);

/* Set this to a callback that fires immediately before hw_present_frame()
 * renders the scanlines.  Fires from pc.c, not from hw.c.
 * Lets the harness capture the chip RAM state the renderer will actually see. */
extern void (*g_harness_prerender_hook)(void);

/* Native disk boot: when set, hw_read of $BFE000 with the title copper active
 * invokes this (it longjmps out of the recompiled cold-start to the frame loop). */
extern void (*g_hw_boot_handoff)(void);

/* Disk-boot coroutine: when set, hw_vblank_wait() yields to the frame driver. */
extern void (*g_hw_vblank_yield)(void);

/* Set by pc.c when it owns the frame loop (presents explicitly once per frame).
 * Suppresses the VPOSR-read auto-present so presentation has a single driver. */
extern int g_hw_pc_owns_present;

/* Returns the current COP1LC ($DFF080) value last written by the game. */
uint32_t hw_get_cop1lc(void);

/* Fired when the game commits the display copper pointer (writes COP1LC). The
 * disk-boot coroutine sets this to present at the true display-swap moment. */
extern void (*g_hw_cop1lc_present)(void);

/* Snapshot current hardware state into *s (used by comparison harness).
 * Only available when HARNESS_BUILD is defined. */
#ifdef HARNESS_BUILD
#include "harness/puae_state.h"
void hw_get_snap(FrameState *s);
void hw_load_audio_sync(const char *path);
void hw_seed_sync_regs(const struct FrameState *snap);
#endif
