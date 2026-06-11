/*
 * recomp/hw.c  –  PC hardware implementation
 *
 * Maps Amiga OCS/CIA register addresses to SDL2 operations.
 * This replaces all of: custom.c, cia.c, copper.c, display.c, audio.c, input.c
 */

#include "engine/hw_private.h"
#include "render/native_renderer.h"
#include "port/port.h"   /* level/world layout accessors (single source of truth) */
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>

#ifdef HARNESS_BUILD
/* In harness mode suppress per-register-write verbose logs */
# define HW_VERBOSE 0
#else
# define HW_VERBOSE 1
#endif

/* Verbose-only log helper */
#define HW_LOG(...) GLOBAL_LOG_IF(HW_VERBOSE, __VA_ARGS__)

/* Harness trace: active when env BENEFACTOR_HW_TRACE is set */
static int s_hwtrace = -1;
static inline int _hwtrace_enabled(void) {
    if (s_hwtrace < 0) s_hwtrace = (getenv("BENEFACTOR_HW_TRACE") != NULL) ? 1 : 0;
    return s_hwtrace;
}
#define HWTRACE(...) do { if (_hwtrace_enabled()) { \
    GLOBAL_LOG("[%s] ", s_copper_writing ? "COPPER" : "GAME"); \
    GLOBAL_LOG(__VA_ARGS__); \
} } while(0)

#include <SDL2/SDL.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <stdio.h>
#include <stdlib.h>
#include "port/input.h"
#include "port/config.h"   /* pc_render_mode() — frame-renderer mode select */

/* ─────────────────────────────────────────────────────────────────────────── */
/* Amiga OCS register offsets (relative to $DFF000)                           */
/* ─────────────────────────────────────────────────────────────────────────── */

#define DMACONR   0x002
#define VPOSR     0x004
#define VHPOSR    0x006
#define JOY0DAT   0x00A
#define JOY1DAT   0x00C
#define POT0DAT   0x012
#define POT1DAT   0x014
#define POTINP    0x016
#define POTGO     0x034
#define BPLCON0   0x100
#define BPL1PTH   0x0E0
#define BPL1PTL   0x0E2
#define BPL2PTH   0x0E4
#define BPL2PTL   0x0E6
#define BPL3PTH   0x0E8
#define BPL3PTL   0x0EA
#define BPL4PTH   0x0EC
#define BPL4PTL   0x0EE
#define BPL5PTH   0x0F0
#define BPL5PTL   0x0F2
#define BPL6PTH   0x0F4
#define BPL6PTL   0x0F6
#define BPL1MOD   0x108
#define BPL2MOD   0x10A
#define DIWSTRT   0x08E
#define DIWSTOP   0x090
#define DDFSTRT   0x092
#define DDFSTOP   0x094
#define DMACON    0x096
#define INTENA    0x09A
#define INTREQ    0x09C
#define COP1LCH   0x080
#define COP1LCL   0x082
#define COPJMP1   0x088
/* Audio channel 0 */
#define AUD0LCH   0x0A0
#define AUD0LCL   0x0A2
#define AUD0LEN   0x0A4
#define AUD0PER   0x0A6
#define AUD0VOL   0x0A8
#define AUD0DAT   0x0AA
/* Audio channel 1 */
#define AUD1LCH   0x0B0
#define AUD1LCL   0x0B2
#define AUD1LEN   0x0B4
#define AUD1PER   0x0B6
#define AUD1VOL   0x0B8
#define AUD1DAT   0x0BA
/* Audio channel 2 */
#define AUD2LCH   0x0C0
#define AUD2LCL   0x0C2
#define AUD2LEN   0x0C4
#define AUD2PER   0x0C6
#define AUD2VOL   0x0C8
#define AUD2DAT   0x0CA
/* Audio channel 3 */
#define AUD3LCH   0x0D0
#define AUD3LCL   0x0D2
#define AUD3LEN   0x0D4
#define AUD3PER   0x0D6
#define AUD3VOL   0x0D8
#define AUD3DAT   0x0DA
#define BLTCON0   0x040
#define BLTCON1   0x042
#define BLTSIZE   0x058
#define BLTDPTH   0x054
#define BLTDPTL   0x056
#define BLTAPTH   0x050
#define BLTAPTL   0x052
#define BLTBPTH   0x04C
#define BLTBPTL   0x04E
#define BLTCPTH   0x048
#define BLTCPTL   0x04A
#define BLTDMOD   0x066
#define BLTAMOD   0x064
#define BLTBMOD   0x062
#define BLTCMOD   0x060
#define BLTAFWM   0x044
#define BLTALWM   0x046
#define SPR0PTH   0x120
#define SPR0PTL   0x122
#define SPR1PTH   0x124
#define SPR1PTL   0x126
#define SPR2PTH   0x128
#define SPR2PTL   0x12A
#define SPR3PTH   0x12C
#define SPR3PTL   0x12E
#define SPR4PTH   0x130
#define SPR4PTL   0x132
#define SPR5PTH   0x134
#define SPR5PTL   0x136
#define SPR6PTH   0x138
#define SPR6PTL   0x13A
#define SPR7PTH   0x13C
#define SPR7PTL   0x13E
#define COLOR00   0x180

/* CIA register indices: (addr >> 8) & 0xF */
#define CIA_PRA   0
#define CIA_PRB   1
#define CIA_DDRA  2
#define CIA_DDRB  3
#define CIA_TALO  4
#define CIA_TAHI  5
#define CIA_TBLO  6
#define CIA_TBHI  7
#define CIA_ICR   13
#define CIA_CRA   14
#define CIA_CRB   15

/* ─────────────────────────────────────────────────────────────────────────── */
/* Internal state                                                               */
/* ─────────────────────────────────────────────────────────────────────────── */

/* Present is delegated to a render backend (SDL by default, Vulkan opt-in) —
 * see src/render/present_backend.h. hw.c still composes s_out, pumps events, and
 * paces frames; the backend owns the window + how s_out reaches the screen. */
#include "render/present_backend.h"
static const PresentBackend *s_backend = NULL;

/* OCS shadow registers (s_regs/s_dmacon/s_intena/s_intreq/s_bplcon0/s_bplptr/
 * s_sprpt/s_palette/s_diwstrt/s_diwstop) all live on g_state via game_state.h
 * — see the legacy-name macros there. Their non-zero defaults are initialised
 * by pc_state_reset_defaults() in pc.c. */

/* Current interrupt-enable shadow (INTENA, $DFF09A). The IRQ-delivery code in
 * pc.c uses this to fire a level's handler ONLY when the game has that level
 * enabled — exactly as the real CPU's interrupt priority logic would, so a
 * vector left over from the previous screen (e.g. during a level load, when the
 * game has masked the interrupt) never runs. */
uint16_t hw_get_intena(void) { return s_intena; }

/* Disk images */
static char  *s_disk_paths[4];
static int    s_n_disks = 0;

/* Framebuffer 320×256 ARGB8888 */
uint32_t s_fb[HW_DISPLAY_W * HW_DISPLAY_H];

const uint32_t *hw_get_framebuffer(void) { return s_fb; }

/* ── Widescreen output surface ─────────────────────────────────────────────────
 * s_fb is always the engine's 4:3 render (352 wide). s_out is the final OUTPUT
 * surface, >= 352 wide; the 352 content is composited centered into it. Default
 * width = 352 (no change; harness/standalone identical). Wider via
 * BENEFACTOR_WIDESCREEN=<width> (or =1 for a 480 default). */
int s_hw_out_w = HW_DISPLAY_W;
static uint32_t s_out[HW_OUT_MAX * HW_DISPLAY_H];
int hw_output_width(void) { return s_hw_out_w; }
const uint32_t *hw_get_output_framebuffer(void) { return s_out; }

/* Composite the 352-wide s_fb (+ pillarbox margins) into s_out at s_hw_out_w.
 * For gameplay, native_render_wide_bg then fills the margins with the off-screen
 * terrain decoded from the level tilemap (Phase 3 widescreen). */
void native_render_wide_bg(uint32_t *out, int ow, int margin);   /* native_renderer.c */
static void hw_compose_output(void)
{
    int ow = s_hw_out_w;
    int margin = (ow - HW_DISPLAY_W) / 2;
    /* L/R bars take the scanline's COLOR00 (the border colour the vanilla
     * render shows) — black normally, but the victory fade is a COLOR00
     * curtain and hardcoded black left the margins/corners unfaded. */
    extern uint32_t native_scanline_bgcolor(int y);
    for (int y = 0; y < HW_DISPLAY_H; y++) {
        uint32_t *dst = s_out + y * ow;
        uint32_t bg = native_scanline_bgcolor(y);
        for (int x = 0; x < margin; x++) dst[x] = bg;
        memcpy(dst + margin, s_fb + y * HW_DISPLAY_W, HW_DISPLAY_W * sizeof(uint32_t));
        for (int x = margin + HW_DISPLAY_W; x < ow; x++) dst[x] = bg;
    }
    /* BenRen = the sprite-based renderer (no Amiga blit): it composes the gameplay
     * playfield + margins natively across the full output width. Vanilla leaves
     * s_fb (the copper/blitter render) pillarboxed. Mode comes from the "renderer"
     * cfg knob; AUTO (unset) picks BenRen whenever a widescreen width is requested.
     * BENEFACTOR_WS_CMP forces the BenRen compose even at 352 so it can be diffed
     * against the vanilla bitplane render (s_fb) frame-by-frame as a correctness gate. */
    static int cmp = -1;
    if (cmp < 0) cmp = getenv("BENEFACTOR_WS_CMP") ? 1 : 0;
    PcRenderMode mode = pc_render_mode();
    int benren = (mode == PC_RENDER_BENREN) || (mode == PC_RENDER_AUTO && ow > HW_DISPLAY_W) || cmp;
    { extern void native_render_scene_invalidate(void); native_render_scene_invalidate(); }
    if (benren)
        native_render_wide_bg(s_out, ow, margin);   /* BenRen native playfield (full width) */
    hw_blit_capture_reset();   /* start a fresh object-blit capture for the next frame */
}

/* Beam counter (simulated at 50 Hz) */
static int s_scanline   = 0;
static int s_frame_num  = 0;

int hw_get_frame_num(void) { return s_frame_num; }

/* Frame timing */
static uint64_t s_frame_start_ns = 0;
#define FRAME_NS  20000000ULL   /* 50 Hz = 20 ms */

/* Watchdog / engine state */
static int s_frame_watchdog_limit = 0;  /* 0 = disabled */
static int s_frame_watchdog_count = 0;
int hw_running = 1;
static int s_headless = 0;  /* 1 = no SDL video/audio (for testing) */
static int s_no_pace  = 0;  /* 1 = skip the 50Hz SDL_Delay (harness/testing) */
static int s_ext_input = 0; /* 1 = harness owns SDL events; don't poll here */
static int s_in_present_frame = 0;

/* Skip the per-frame 50Hz SDL_Delay pacing — set by the harness so test runs
 * go at full CPU speed (the interactive harness paces separately). */
void hw_set_no_pace(int on) { s_no_pace = on; }

/* Real-time speed, in PERCENT of PAL 50Hz. Two sources:
 *   - "game_speed" cfg knob ("normal" = 100 | "turbo" = 120), the persistent
 *     setting in the pause OPTIONS menu;
 *   - a HOLD-to-fast-forward action (PI_FFWD binding, default Tab /
 *     RightTrigger) = 500% while held.
 * Only the pacing target changes — per-frame game logic is untouched, the game
 * just advances faster in wall-clock time (in heavy/large levels the engine+
 * render cost may cap the real rate below the target; pacing simply stops
 * sleeping). MUSIC AND AUDIO ARE NOT SPED UP: pc_step gates its music ticks +
 * PCM render on hw_audio_frame_due(), which runs them on a wall-clock 20ms
 * cadence whenever the effective speed != 100%. */
static int s_speed_pct = 100;   /* persistent knob: normal=100, turbo=120 */
static int s_ffwd_held = 0;     /* fast-forward action currently held */
#define HW_FFWD_PCT 500

static int hw_speed_eff_pct(void) { return s_ffwd_held ? HW_FFWD_PCT : s_speed_pct; }

void hw_speed_refresh(void)
{
    int pct = 100;
    char buf[16];
    if (pc_cfg_show("game_speed", buf, sizeof buf, NULL) && buf[0]) {
        if      (!strcasecmp(buf, "turbo"))  pct = 120;
        else if (!strcasecmp(buf, "hyper"))  pct = 150;
        else if (!strcasecmp(buf, "normal")) pct = 100;
        else {                       /* numeric multiplier (legacy "1"/"2"/"4") */
            int m = atoi(buf);
            if (m >= 1 && m <= 16) pct = m * 100;
        }
    }
    if (pct == s_speed_pct) return;
    s_speed_pct = pct;
    fprintf(stderr, "[speed] %d%%\n", pct);
    fflush(stderr);
}

void hw_set_ffwd(int held) { s_ffwd_held = !!held; }
int  hw_ffwd_active(void)  { return s_ffwd_held; }   /* HUD icon */

/* True when a real-time (wall-clock 20ms) music/audio frame is due. At exactly
 * 100% this is ALWAYS true — one audio frame per game frame, the original
 * deterministic path (the harness never changes speed, so its compare runs are
 * untouched). Faster, game frames outpace real time; music ticks + PCM render
 * only fire on the 50Hz wall-clock grid, so the soundtrack keeps its normal
 * tempo and the SDL queue is fed at exactly the rate it drains. */
/* Pace one frame to PAL 50 Hz scaled by the effective speed (microsecond
 * accumulator so fractional targets like turbo 120% = 16.67ms pace exactly;
 * self-corrects and resyncs if we fall far behind). Called for EVERY game
 * frame — including the fast-forward frames whose render/present is skipped;
 * the early-skip path previously returned before pacing, which made
 * fast-forward effectively unbounded instead of 5x. */
static void hw_pace_frame(void)
{
    if (s_no_pace) return;
    static uint64_t s_next_frame_us = 0;
    uint64_t now_us = SDL_GetTicks64() * 1000ull;
    if (s_next_frame_us == 0 || now_us > s_next_frame_us + 100000ull)
        s_next_frame_us = now_us;           /* first frame or big stall: resync */
    s_next_frame_us += 2000000ull / (unsigned)hw_speed_eff_pct();
    if (now_us < s_next_frame_us)
        SDL_Delay((uint32_t)((s_next_frame_us - now_us) / 1000ull));
}

/* ── Frame-time profiler (F3 overlay) ─────────────────────────────────────────
 * Exponential moving averages (1/32) of where each frame's wall time goes, in
 * microseconds, plus a once-per-second fps counter. Sections: the game step
 * (recompiled engine + audio, measured by pc_step), the native render, the
 * BenRen compose, and the SDL present. Built to diagnose "speed X doesn't
 * reach its target on level Y / machine Z" without guessing. */
HwPerf g_hw_perf;
int g_hw_perf_overlay = 0;            /* F3 toggles */

uint64_t hw_perf_now_us(void)
{
    return SDL_GetPerformanceCounter() * 1000000ull / SDL_GetPerformanceFrequency();
}

void hw_perf_acc(uint32_t *ema_us, uint64_t t0_us)
{
    uint64_t dt = hw_perf_now_us() - t0_us;
    if (dt > 10000000ull) dt = 10000000ull;     /* clamp pathological stalls */
    int32_t d = (int32_t)dt - (int32_t)*ema_us;
    *ema_us = (uint32_t)((int32_t)*ema_us + d / 32);   /* EMA, alpha = 1/32 */
}

static void hw_perf_fps_tick(void)
{
    static uint64_t s_sec_start = 0;
    static int s_sec_frames = 0;
    s_sec_frames++;
    uint64_t now = hw_perf_now_us();
    if (s_sec_start == 0) s_sec_start = now;
    if (now - s_sec_start >= 1000000ull) {
        g_hw_perf.fps = (int)((uint64_t)s_sec_frames * 1000000ull / (now - s_sec_start));
        s_sec_start = now; s_sec_frames = 0;
    }
}

int hw_audio_frame_due(void)
{
    if (hw_speed_eff_pct() == 100) return 1;
    static uint64_t s_next_ms = 0;
    uint64_t now = SDL_GetTicks64();
    if (s_next_ms == 0 || now > s_next_ms + 200) s_next_ms = now;  /* (re)sync */
    if (now < s_next_ms) return 0;
    s_next_ms += 20;
    return 1;
}

/* Harness owns the SDL event queue (its input_poll is the sole reader). When
 * set, hw_present_frame must NOT call SDL_PollEvent — two consumers draining
 * the single event queue split KEYUP/KEYDOWN/QUIT between them, causing stuck
 * fire (missed KEYUP) and ignored presses (missed KEYDOWN). */
void hw_set_external_input(int on) { s_ext_input = on; }

/* CIA-B timer */
/* CIA-B timer state (s_ciab_*) lives on g_state via game_state.h. */

/* Input */
static uint8_t  s_joy_buttons = 0;   /* bit 0 = fire (Z/LCTRL) */
static uint8_t  s_joy_up = 0, s_joy_down = 0, s_joy_left = 0, s_joy_right = 0;
static uint8_t  s_mouse_lmb = 0;    /* SPACE / RETURN (active high) */
static uint8_t  s_fire_pressed = 0; /* any fire key pressed (active high) */
static uint8_t  s_interact = 0;     /* dedicated INTERACT key (X) — separate from fire */
static uint8_t  s_drop = 0;         /* dedicated DROP button — one of the drop bindings */
static uint8_t  s_hop  = 0;         /* HOP action (separate from the Up direction) */
static uint8_t  s_fire_vanilla = 0; /* fire held on a device with VANILLA controls */
void (*g_hw_boot_handoff)(void) = NULL;  /* native disk-boot → frame-loop hand-off */

void hw_set_joystick(int up, int down, int left, int right, int fire)
{
    s_joy_up = !!up; s_joy_down = !!down; s_joy_left = !!left; s_joy_right = !!right;
    if (fire) { s_joy_buttons |= 1; s_fire_pressed = 1; }
    else      { s_joy_buttons &= ~1; s_fire_pressed = 0; }
}
int  hw_get_fire(void)     { return s_fire_pressed; }
int  hw_get_fire_vanilla(void) { return s_fire_vanilla; }
void hw_set_fire_vanilla(int on) { s_fire_vanilla = !!on; }  /* harness forced fire */
int  hw_get_mouse_lmb(void) { return s_mouse_lmb; }
void hw_set_fire(int on)   { s_fire_pressed = on; if (on) s_joy_buttons |= 1; else s_joy_buttons &= ~1; }
void hw_set_mouse_lmb(int on) { s_mouse_lmb = on; }
int  hw_get_interact(void)  { return s_interact; }
void hw_set_interact(int on) { s_interact = !!on; }
int  hw_get_drop(void)   { return s_drop; }
void hw_set_drop(int on)  { s_drop = !!on; }
int  hw_get_hop(void)    { return s_hop; }
void hw_set_hop(int on)   { s_hop = !!on; }
void hw_set_joy_down(int on) { s_joy_down = !!on; }
void hw_set_joy_up(int on)   { s_joy_up = !!on; }
int  hw_joy_up(void)    { return s_joy_up; }
int  hw_joy_down(void)  { return s_joy_down; }
int  hw_joy_left(void)  { return s_joy_left; }
int  hw_joy_right(void) { return s_joy_right; }

/* Derive the engine input flags from the resolved (config-bound) actions. Called after
 * every gameplay key event. Up = Up direction OR a separate Hop binding — so a dedicated
 * hop key/button works while the Up direction still drives menus + ladders. (Suppressing
 * hop on the Up *direction* while grounded — so stick-up only climbs — needs a grounded/
 * on-ladder signal that isn't pinned yet; see pc_input + the input task.) */
static void apply_bound_input(void)
{
    /* Directions + fire merge across both devices. The MODERN-scheme actions
     * (hop/interact/drop) only come from devices with modern controls enabled —
     * a vanilla device keeps the authentic semantics (fire interacts, Up hops)
     * with no extra action keys, even while the other device runs modern. */
    int kbm = pc_modern_kb(), pdm = pc_modern_pad();
    s_joy_left  = pc_input_active(PI_LEFT);
    s_joy_right = pc_input_active(PI_RIGHT);
    s_joy_down  = pc_input_active(PI_DOWN);
    s_joy_up    = pc_input_active(PI_UP);   /* Up direction only; HOP is separate (s_hop) */
    s_hop       = (kbm && pc_input_active_dev(PI_DEV_KB,  PI_HOP)) ||
                  (pdm && pc_input_active_dev(PI_DEV_PAD, PI_HOP));
    int fire    = pc_input_active(PI_FIRE);
    s_fire_pressed = fire; if (fire) s_joy_buttons |= 1; else s_joy_buttons &= ~1;
    s_mouse_lmb = fire;                    /* fire also = port-0/menu select */
    /* Fire held on a VANILLA-scheme device: that fire is allowed to keep its
     * original interact/drop meaning in the overrides (hw_get_fire_vanilla). */
    s_fire_vanilla = (!kbm && pc_input_active_dev(PI_DEV_KB,  PI_FIRE)) ||
                     (!pdm && pc_input_active_dev(PI_DEV_PAD, PI_FIRE));
    s_interact  = (kbm && pc_input_active_dev(PI_DEV_KB,  PI_INTERACT)) ||
                  (pdm && pc_input_active_dev(PI_DEV_PAD, PI_INTERACT));
    s_drop      = (kbm && pc_input_active_dev(PI_DEV_KB,  PI_DROP)) ||
                  (pdm && pc_input_active_dev(PI_DEV_PAD, PI_DROP));
    s_ffwd_held = pc_input_active(PI_FFWD);   /* hold-to-fast-forward, any device */

    /* FREE CAM toggle (edge) + input redirect: while detached, the directions
     * pan the camera (pc_freecam_tick) and the player gets NO input at all, so
     * he stands idle wherever you left him. Fast-forward stays available. */
    { extern void pc_freecam_toggle(void);
      extern int  pc_freecam_active(void);
      static int prev_fc = 0;
      int fc = pc_input_active(PI_FREECAM);
      if (fc && !prev_fc) pc_freecam_toggle();
      prev_fc = fc;
      if (pc_freecam_active()) {
          s_joy_left = s_joy_right = s_joy_up = s_joy_down = 0;
          s_hop = s_interact = s_drop = 0;
          s_fire_pressed = 0; s_joy_buttons &= ~1; s_mouse_lmb = 0;
          s_fire_vanilla = 0;
      } }
}

/* Single keyboard→input-state mapper, shared by the standalone
 * (hw_present_frame) and the harness (input.c) so there is exactly one input
 * path. 'sym' is an SDL keysym; window-level events (QUIT/ESC/F11/mouse) stay
 * with each caller. Any fire key drives BOTH the joystick fire ($BFE001 bit6,
 * intro) AND the port-0/mouse button (bit7, the title/menu start check), so one
 * key advances every screen. */
void hw_handle_key(int sym, int down)
{
    /* Pause-menu interception: when the menu is up, arrows + fire navigate the
     * menu instead of being delivered to the game. ESC toggles the menu in
     * gameplay; outside gameplay it falls through to exit(). */
    {
        extern int  pc_pause_active(void);
        extern void pc_pause_toggle(void);
        extern void pc_pause_escape(void);
        extern void pc_pause_input_up(void);
        extern void pc_pause_input_down(void);
        extern void pc_pause_input_left(void);
        extern void pc_pause_input_right(void);
        extern void pc_pause_input_select(void);
        extern int  pc_pause_capture_active(void);
        extern void pc_pause_capture_code(int dev, int code);
        /* Bindings capture ("PRESS A KEY"): the next key press becomes the
         * binding. ESC cancels (handled inside pc_pause_capture_code). */
        if (pc_pause_capture_active()) {
            if (down) pc_pause_capture_code(PI_DEV_KB, sym);
            return;
        }
        if (sym == SDLK_ESCAPE) {
            if (down) {
                /* Title-menu level-select panel: ESC dismisses it without
                 * starting a level. Takes priority over the pause + quit
                 * paths because we're on the title screen, not in-game. */
                extern int g_level_select_visible;
                if (g_level_select_visible) {
                    g_level_select_visible = 0;
                    return;
                }
                if (pc_pause_active()) { pc_pause_escape(); return; }  /* back/resume */
                if (g_gameplay_active) { pc_pause_toggle(); return; }
                /* Outside gameplay (title/menu/intro): ESC opens the OPTIONS
                 * page directly — quitting moved to its QUIT TO DESKTOP row. */
                { extern void pc_pause_open_options(void);
                  pc_pause_open_options(); }
            }
            return;
        }
        if (pc_pause_active()) {
            if (!down) return;
            switch (sym) {
                case SDLK_UP:    pc_pause_input_up();    return;
                case SDLK_DOWN:  pc_pause_input_down();  return;
                case SDLK_LEFT:  pc_pause_input_left();  return;
                case SDLK_RIGHT: pc_pause_input_right(); return;
                case SDLK_z:
                case SDLK_LCTRL:
                case SDLK_SPACE:
                case SDLK_RETURN:
                                 pc_pause_input_select(); return;
                default: return;   /* swallow other keys while paused */
            }
        }
    }

    /* Title-menu LEVEL SELECT panel: arrows navigate the panel, not the game. */
    {
        extern int g_level_select_visible;
        if (down && g_level_select_visible &&
            (sym == SDLK_UP || sym == SDLK_DOWN || sym == SDLK_LEFT || sym == SDLK_RIGHT)) {
            int cur = pc_get_start_level();
            if (cur < 1) cur = 1;
            int w, liw; pc_level_split(cur, &w, &liw);
            if (w < 0 || w >= PC_NUM_WORLDS) w = 0;
            switch (sym) {   /* locked targets refuse (pc_profile_try_select) */
                case SDLK_UP:    pc_profile_try_select(cur - 1); break;
                case SDLK_DOWN:  pc_profile_try_select(cur + 1); break;
                case SDLK_LEFT:  pc_profile_try_select(pc_world_first_level(w > 0 ? w - 1 : 0)); break;
                case SDLK_RIGHT: pc_profile_try_select(pc_world_first_level(w < PC_NUM_WORLDS - 1 ? w + 1 : w)); break;
            }
            return;
        }
    }

    /* Gameplay / menu input — resolved through the config-bound action layer
     * (pc_input). Movement, hop, fire, interact and drop are all remappable. */
    pc_input_load();
    pc_input_key(sym, down);
    apply_bound_input();

    switch (sym) {
    case SDLK_l:   /* debug: force LEVEL COMPLETE (the teleport win) */
        if (down) { extern void pc_debug_complete_level(void); pc_debug_complete_level(); }
        break;
    case SDLK_o:   /* debug: force GAME OVER (death) */
        if (down) { extern void pc_debug_game_over(void); pc_debug_game_over(); }
        break;
    case SDLK_s: /* save: defer to the main-thread frame boundary in pc_step (the
                  * game thread must be parked so its M68K ctx + chip RAM are
                  * coherent; pc_savestate_allowed then gates on steady gameplay). */
        if (down) { extern int g_pc_pending_save, g_pc_pending_load;
                    g_pc_pending_save = 1; (void)g_pc_pending_load; }
        break;
    case SDLK_d: /* load: same deferral as save */
        if (down) { extern int g_pc_pending_save, g_pc_pending_load;
                    g_pc_pending_load = 1; (void)g_pc_pending_save; }
        break;
    default: break;
    }
}
/* ─────────────────────────────────────────────────────────────────────────── */
/* Game controllers (hot-pluggable) + widescreen mode                          */
/* ─────────────────────────────────────────────────────────────────────────── */

#define HW_MAX_PADS 8
static SDL_GameController *s_pads[HW_MAX_PADS];
static SDL_JoystickID      s_pad_ids[HW_MAX_PADS];
static int s_npads = 0;
/* Per (axis, direction) digital state for analog→action edge detection. */
static uint8_t s_axis_on[SDL_CONTROLLER_AXIS_MAX][2];
#define PAD_AXIS_ON_THRESH   16000
#define PAD_AXIS_OFF_THRESH   8000

int hw_pad_count(void) { return s_npads; }

static void hw_pad_open(int device_index)
{
    if (!SDL_IsGameController(device_index) || s_npads >= HW_MAX_PADS) return;
    SDL_GameController *gc = SDL_GameControllerOpen(device_index);
    if (!gc) return;
    SDL_JoystickID id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc));
    for (int i = 0; i < s_npads; i++)
        if (s_pad_ids[i] == id) { SDL_GameControllerClose(gc); return; }  /* already open */
    s_pads[s_npads] = gc; s_pad_ids[s_npads] = id; s_npads++;
    fprintf(stderr, "[pad] connected: %s (%d total)\n", SDL_GameControllerName(gc), s_npads);
}

static void hw_pad_close(SDL_JoystickID id)
{
    for (int i = 0; i < s_npads; i++) {
        if (s_pad_ids[i] != id) continue;
        fprintf(stderr, "[pad] disconnected: %s\n", SDL_GameControllerName(s_pads[i]));
        SDL_GameControllerClose(s_pads[i]);
        s_pads[i] = s_pads[--s_npads]; s_pad_ids[i] = s_pad_ids[s_npads];
        if (s_npads == 0) {            /* no pads left: release any held buttons */
            pc_input_pad_clear();
            memset(s_axis_on, 0, sizeof s_axis_on);
            apply_bound_input();
        }
        return;
    }
}

/* Route one digital pad code (button or axis-direction edge) — the controller
 * twin of hw_handle_key: pause menu navigation, bindings capture, or gameplay. */
static void hw_handle_pad_code(int code, int down)
{
    extern int  pc_pause_active(void), pc_pause_capture_active(void);
    extern void pc_pause_capture_code(int dev, int code);
    extern void pc_pause_toggle(void), pc_pause_escape(void);
    extern void pc_pause_input_up(void), pc_pause_input_down(void);
    extern void pc_pause_input_left(void), pc_pause_input_right(void);
    extern void pc_pause_input_select(void);

    if (pc_pause_capture_active()) {
        if (down) pc_pause_capture_code(PI_DEV_PAD, code);
        return;
    }
    if (code == SDL_CONTROLLER_BUTTON_START) {
        if (down) {
            if (pc_pause_active())       pc_pause_escape();
            else if (g_gameplay_active)  pc_pause_toggle();
            else { extern void pc_pause_open_options(void);
                   pc_pause_open_options(); }   /* title: straight to OPTIONS */
        }
        return;
    }
    if (pc_pause_active()) {
        if (!down) return;
        if      (code == SDL_CONTROLLER_BUTTON_DPAD_UP ||
                 code == PI_PAD_AXIS_CODE(SDL_CONTROLLER_AXIS_LEFTY, 0))  pc_pause_input_up();
        else if (code == SDL_CONTROLLER_BUTTON_DPAD_DOWN ||
                 code == PI_PAD_AXIS_CODE(SDL_CONTROLLER_AXIS_LEFTY, 1))  pc_pause_input_down();
        else if (code == SDL_CONTROLLER_BUTTON_DPAD_LEFT ||
                 code == PI_PAD_AXIS_CODE(SDL_CONTROLLER_AXIS_LEFTX, 0))  pc_pause_input_left();
        else if (code == SDL_CONTROLLER_BUTTON_DPAD_RIGHT ||
                 code == PI_PAD_AXIS_CODE(SDL_CONTROLLER_AXIS_LEFTX, 1))  pc_pause_input_right();
        else if (code == SDL_CONTROLLER_BUTTON_A)                         pc_pause_input_select();
        else if (code == SDL_CONTROLLER_BUTTON_B)                         pc_pause_escape();
        return;
    }
    pc_input_load();
    pc_input_pad_button(code, down);
    apply_bound_input();
}

/* ── Widescreen mode ("widescreen_mode": disabled | 16:9 | ultrawide | auto) ──
 * Resolves the cfg knob to an output width and applies it live: the present
 * backend re-creates its texture when the content width changes, so this works
 * at runtime from the pause menu. AUTO derives the width from the actual window
 * aspect and re-applies on every window resize. When the knob is unset the
 * legacy BENEFACTOR_WIDESCREEN pixel width (read in hw_init) stays in effect. */
static int hw_widescreen_mode(void)   /* -1 unset, 0 disabled, 1 16:9, 2 ultrawide, 3 auto */
{
    char buf[24]; const char *src;
    if (!pc_cfg_show("widescreen_mode", buf, sizeof buf, &src) || !buf[0]) return -1;
    if (!strcasecmp(buf, "disabled") || !strcasecmp(buf, "off"))   return 0;
    if (!strcmp(buf, "16:9")  || !strcasecmp(buf, "169"))          return 1;
    if (!strcasecmp(buf, "ultrawide") || !strcmp(buf, "21:9"))     return 2;
    if (!strcasecmp(buf, "auto"))                                  return 3;
    fprintf(stderr, "[widescreen] unknown widescreen_mode '%s' (disabled|16:9|ultrawide|auto)\n", buf);
    return -1;
}

static int ws_clamp(int w)
{
    if (w < HW_DISPLAY_W) w = HW_DISPLAY_W;
    if (w > HW_OUT_MAX)   w = HW_OUT_MAX;
    return w & ~1;
}

void hw_widescreen_refresh(void)
{
    int mode = hw_widescreen_mode();
    if (mode < 0) return;                       /* knob unset: legacy width stays */
    int w;
    switch (mode) {
    case 0:  w = HW_DISPLAY_W;                 break;
    case 1:  w = HW_DISPLAY_H * 16 / 9;        break;
    case 2:  w = HW_DISPLAY_H * 21 / 9;        break;
    default: {                                  /* auto: window aspect */
        SDL_Window *win = (!s_headless && s_backend) ? s_backend->window() : NULL;
        if (win) {
            int ww = 0, wh = 0;
            SDL_GetWindowSize(win, &ww, &wh);
            w = (wh > 0) ? (int)((int64_t)HW_DISPLAY_H * ww / wh) : HW_DISPLAY_H * 16 / 9;
        } else {
            w = HW_DISPLAY_H * 16 / 9;          /* no window yet (init/headless) */
        }
        break;
    }
    }
    w = ws_clamp(w);
    if (w == s_hw_out_w) return;
    s_hw_out_w = w;
    fprintf(stderr, "[widescreen] output width -> %d px\n", w);
    /* For the fixed presets, match the window to the new aspect (auto follows
     * the window instead; fullscreen scales within the display). */
    if (mode != 3 && !s_headless && s_backend) {
        SDL_Window *win = s_backend->window();
        if (win && !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP))
            SDL_SetWindowSize(win, w * 2, HW_DISPLAY_H * 2);
    }
}

/* Shared handler for non-keyboard SDL events (controller hot-plug + buttons +
 * axes, window resize). Called from BOTH event pumps — hw_present_frame's and
 * the harness input_poll — so controller support works everywhere. Returns 1
 * if the event was consumed. */
int hw_handle_sdl_event(const SDL_Event *ev)
{
    switch (ev->type) {
    case SDL_CONTROLLERDEVICEADDED:
        hw_pad_open(ev->cdevice.which);          /* hot-plug */
        return 1;
    case SDL_CONTROLLERDEVICEREMOVED:
        hw_pad_close(ev->cdevice.which);
        return 1;
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
        hw_handle_pad_code(ev->cbutton.button, ev->type == SDL_CONTROLLERBUTTONDOWN);
        return 1;
    case SDL_CONTROLLERAXISMOTION: {
        int axis = ev->caxis.axis;
        if (axis < 0 || axis >= SDL_CONTROLLER_AXIS_MAX) return 1;
        int v = ev->caxis.value;
        for (int dir = 0; dir < 2; dir++) {      /* 0 = negative, 1 = positive */
            int mag = dir ? v : -v;
            int on  = s_axis_on[axis][dir] ? (mag > PAD_AXIS_OFF_THRESH)
                                           : (mag > PAD_AXIS_ON_THRESH);
            if (on != s_axis_on[axis][dir]) {    /* hysteresis edge → digital event */
                s_axis_on[axis][dir] = (uint8_t)on;
                hw_handle_pad_code(PI_PAD_AXIS_CODE(axis, dir), on);
            }
        }
        return 1;
    }
    case SDL_WINDOWEVENT:
        if (ev->window.event == SDL_WINDOWEVENT_SIZE_CHANGED &&
            hw_widescreen_mode() == 3)
            hw_widescreen_refresh();             /* auto: follow the window aspect */
        return 0;                                /* not exclusive — others may care */
    default:
        return 0;
    }
}

static uint8_t  s_key_byte = 0xFF;
/* s_blt_bzero, s_vposr_counter, s_audio[] live on g_state via game_state.h. */
int      s_copper_writing = 0; /* set during copper MOVE execution */

SDL_AudioDeviceID s_audio_dev = 0;
SDL_AudioSpec s_audio_spec;

/* ─────────────────────────────────────────────────────────────────────────── */
/* Init / fini                                                                  */
/* ─────────────────────────────────────────────────────────────────────────── */

/* Set by the harness (hw_request_headless) before hw_init so the PC port renders
 * to its framebuffer but does NOT open its own SDL window — the harness presents
 * the framebuffer itself (side-by-side). Avoids a second window. */
static int s_force_headless = 0;
void hw_request_headless(void) { s_force_headless = 1; }

int hw_init(const char *title, const char **disk_paths, int n_disks)
{
    s_headless = s_force_headless || (getenv("BENEFACTOR_HEADLESS") != NULL);

    /* Widescreen output width (BENEFACTOR_WIDESCREEN=<px>, or =1 → 480). Read here
     * (before the headless branch) so headless widescreen captures work too. */
    { const char *e = getenv("BENEFACTOR_WIDESCREEN");
      if (e) { int w = atoi(e); if (w <= 1) w = 480;
               if (w < HW_DISPLAY_W) w = HW_DISPLAY_W;
               if (w > HW_OUT_MAX)   w = HW_OUT_MAX;
               s_hw_out_w = w & ~1; } }
    /* "widescreen_mode" preset (options menu) overrides the legacy pixel width.
     * Resolved BEFORE the window exists, so auto starts at 16:9 and then follows
     * the first real window-resize event. */
    pc_config_load();
    hw_widescreen_refresh();
    hw_speed_refresh();    /* "game_speed" knob (1|2|4|turbo) */

    if (s_headless) {
        HW_LOG("HEADLESS mode – no SDL video/audio\n");
        GLOBAL_LOG_FLUSH();
        /* Minimal SDL init for timer/events if needed */
        if (SDL_Init(0) < 0) {
            HW_LOG("SDL_Init: %s\n", SDL_GetError());
            GLOBAL_LOG_FLUSH();
            return -1;
        }
    } else {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK |
                     SDL_INIT_GAMECONTROLLER) < 0) {
            HW_LOG("SDL_Init: %s\n", SDL_GetError());
            return -1;
        }
        /* Controllers already connected at launch (hot-plug arrivals come in as
         * SDL_CONTROLLERDEVICEADDED events through hw_handle_sdl_event). */
        for (int i = 0; i < SDL_NumJoysticks(); i++) hw_pad_open(i);

        /* Pick the present backend (BENEFACTOR_RENDER=sdl|vulkan, default sdl;
         * falls back to sdl if vulkan is unavailable) and let it own the window. */
        s_backend = present_backend_select(getenv("BENEFACTOR_RENDER"));
        if (s_backend->init(title, s_hw_out_w, HW_DISPLAY_H) != 0) {
            const PresentBackend *sdl = present_backend_sdl();
            if (s_backend == sdl) return -1;   /* sdl itself failed: nothing to fall back to */
            HW_LOG("[render] backend '%s' init failed (%s); falling back to sdl\n",
                   s_backend->name, SDL_GetError());
            s_backend = sdl;
            if (s_backend->init(title, s_hw_out_w, HW_DISPLAY_H) != 0) return -1;
        }
        HW_LOG("[render] present backend: %s\n", s_backend->name);

        hw_audio_open();
    }

    /* Store disk paths */
    s_n_disks = (n_disks < 4) ? n_disks : 4;
    for (int i = 0; i < s_n_disks; i++)
        s_disk_paths[i] = disk_paths[i] ? strdup(disk_paths[i]) : NULL;

    /* Init default palette to grey ramp */
    for (int i = 0; i < 32; i++)
        s_palette[i] = amiga_to_argb((uint16_t)(i * 0x111));

    s_frame_start_ns = (uint64_t)SDL_GetTicks64() * 1000000ULL;
    return 0;
}

void hw_fini(void)
{
    hw_audio_close();
    for (int i = 0; i < s_n_disks; i++) free(s_disk_paths[i]);
    if (!s_headless && s_backend) s_backend->shutdown();
    SDL_Quit();
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Frame presentation + vsync                                                   */
/* ─────────────────────────────────────────────────────────────────────────── */

int hw_present_frame(void)
{
    if (s_in_present_frame)
        return 0;
    s_in_present_frame = 1;

    /* Poll events — skip if the harness owns the SDL event queue (its
     * input_poll is the sole reader) or side-by-side mode handles them. */
    if (!g_harness_frame_hook && !s_ext_input) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { s_in_present_frame = 0; return 1; }
            if (hw_handle_sdl_event(&ev)) continue;   /* controllers, window resize */
            if (ev.type == SDL_MOUSEBUTTONDOWN || ev.type == SDL_MOUSEBUTTONUP) {
                if (ev.button.button == SDL_BUTTON_LEFT)
                    s_mouse_lmb = (ev.type == SDL_MOUSEBUTTONDOWN);
            }
            if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
                int down = (ev.type == SDL_KEYDOWN);
                /* ESC is handled inside hw_handle_key — it toggles the pause
                 * menu in gameplay, falls back to exit(0) elsewhere. Don't
                 * short-circuit return 1 here or pause never engages. */
                if (ev.key.keysym.sym == SDLK_F11) {
                    if (!s_headless && down && s_backend) s_backend->toggle_fullscreen();
                } else if (ev.key.keysym.sym == SDLK_F3) {
                    if (down) g_hw_perf_overlay = !g_hw_perf_overlay;
                } else {
                    hw_handle_key(ev.key.keysym.sym, down);
                }
            }
        }
    }

    /* FAST-FORWARD frame-skip: rendering + presenting every frame is what
     * actually capped the old turbo (texture upload at game rate). At >=2x
     * effective speed, run the game at full rate but only render/present a
     * ~60fps sample of it; events were already polled above so keys always
     * respond. The blit capture is still reset per frame (it is per-frame
     * data — letting it accumulate across skipped frames would overflow into
     * the next rendered one). This is a standalone interactive feature: the
     * harness paths (frame hooks/headless compare) never speed up, so their
     * captures are untouched. */
    if (hw_speed_eff_pct() >= 200 && !s_headless && !g_harness_frame_hook && !s_ext_input) {
        static uint64_t s_last_present_ms = 0;
        uint64_t now = SDL_GetTicks64();
        if (now - s_last_present_ms < 16) {
            hw_blit_capture_reset();
            s_frame_num++;
            s_in_present_frame = 0;
            hw_perf_fps_tick();   /* skipped frames count toward the fps readout */
            hw_pace_frame();      /* skipped frames still pace — FF is 5x, not unbounded */
            return 0;
        }
        s_last_present_ms = now;
    }

    /* Free-cam pan: ticked here (not in pc_step) so the camera keeps moving
     * while freecam_pause has the game frozen. */
    { extern void pc_freecam_tick(void); pc_freecam_tick(); }

    /* Render natively: native_render_frame walks the copper list straight from
     * chip RAM (no separate copper-execute pass — that was the old emulated path). */
    uint64_t perf_t = hw_perf_now_us();
    native_render_frame();  /* walks copper list from chip RAM, renders s_fb[] */
    hw_perf_acc(&g_hw_perf.render_us, perf_t);

    /* Harness snap is taken by the caller (pc.c) AFTER the timer interrupt,
     * to match PUAE's ordering where VBlank fires after copper and before snap. */

    perf_t = hw_perf_now_us();
    hw_compose_output();   /* 352 content -> wide output (pillarbox margins) */
    hw_perf_acc(&g_hw_perf.compose_us, perf_t);

    /* PC-native overlays — drawn into the FINAL composited output (s_out) AFTER the wide
     * playfield, so they stack on top of everything (the wide renderer would otherwise
     * overpaint them) and span the full widescreen width. Level-select first, then the
     * pause menu above it, then the save/load toast topmost. */
    { extern void pc_overlay_set_dims(int, int);
      extern void pc_level_select_overlay(uint32_t *fb);
      extern void pc_pause_menu_overlay(uint32_t *fb);
      extern void pc_toast_overlay(uint32_t *fb);
      extern void pc_hud_icons_overlay(uint32_t *fb);
      pc_overlay_set_dims(s_hw_out_w, HW_DISPLAY_H);
      pc_level_select_overlay(s_out);
      pc_hud_icons_overlay(s_out);    /* fast-forward / free-cam status icons */
      pc_pause_menu_overlay(s_out);
      pc_toast_overlay(s_out);
      pc_overlay_set_dims(HW_DISPLAY_W, HW_DISPLAY_H);   /* restore default */ }

    /* Free-cam fade return: whole-screen curtain over the composed output
     * (out + in, 1s total — see pc_freecam_toggle). */
    { extern int pc_freecam_fade_alpha(void);
      int fa = pc_freecam_fade_alpha();
      if (fa > 0) {
          int keep = 255 - fa;
          int n = s_hw_out_w * HW_DISPLAY_H;
          for (int i = 0; i < n; i++) {
              uint32_t px = s_out[i];
              s_out[i] = 0xFF000000u
                       | ((((px >> 16) & 0xFF) * (uint32_t)keep / 255u) << 16)
                       | ((((px >>  8) & 0xFF) * (uint32_t)keep / 255u) <<  8)
                       |  (((px       ) & 0xFF) * (uint32_t)keep / 255u);
          }
      } }

    if (!s_headless) {
        /* P4 — per-sprite windowed present: when this frame produced a fresh
         * BenRen draw list and no PC overlay is active (overlays read-modify the
         * composed surface), draw the scene's quads to the window instead of
         * blitting s_out. Verified byte-identical to s_out (harness `scenewin`). */
        extern int  native_render_scene_ready(void);
        extern const Scene *native_render_scene(void);
        extern void native_render_scene_yrange(int *, int *);
        extern int  pc_pause_active(void), pc_toast_visible(void);
        extern int  pc_hud_icons_active(void);
        extern int  g_level_select_visible;
        extern int  pc_freecam_fade_alpha(void);
        int overlay = pc_pause_active() || pc_toast_visible() || g_level_select_visible
                   || pc_hud_icons_active() || pc_freecam_fade_alpha() > 0;
        perf_t = hw_perf_now_us();
        if (s_backend->present_scene && native_render_scene_ready() && !overlay) {
            int ylo, yhi; native_render_scene_yrange(&ylo, &yhi);
            s_backend->present_scene(native_render_scene(), ylo, yhi,
                                     s_out, s_hw_out_w, HW_DISPLAY_H);
        } else {
            s_backend->present(s_out, s_hw_out_w, HW_DISPLAY_H);
        }
        hw_perf_acc(&g_hw_perf.present_us, perf_t);
        hw_perf_fps_tick();

        hw_pace_frame();   /* PAL 50 Hz scaled by the effective speed */
    }
    s_in_present_frame = 0;

    /* ── Frame BMP dump (works in headless mode too) ───────────────────── */
    {
        static int dump_frame = -2;
        if (dump_frame == -2) {
            const char *env = getenv("BENEFACTOR_DUMP_FRAME");
            dump_frame = env ? atoi(env) : -1;
        }
        if (dump_frame >= 0 && (uint32_t)s_frame_num == (uint32_t)dump_frame) {
            /* Dump the composed OUTPUT surface (s_out) — what is actually
             * presented, including the widescreen margins and overlays — not the
             * 352px content fb. hw_compose_output() already ran this frame. */
            const char *path = "frame_dump.bmp";
            FILE *fp = fopen(path, "wb");
            if (fp) {
                int W = s_hw_out_w, H = HW_DISPLAY_H;
                /* BMP row stride must be 4-byte aligned (already: W*4) */
                int row_bytes = W * 4;
                int data_size = row_bytes * H;
                int file_size = 54 + data_size;
                /* BMP file header (14 bytes) */
                uint8_t hdr[54] = {0};
                hdr[0]='B'; hdr[1]='M';
                hdr[2]=file_size&0xFF; hdr[3]=(file_size>>8)&0xFF;
                hdr[4]=(file_size>>16)&0xFF; hdr[5]=(file_size>>24)&0xFF;
                hdr[10]=54;  /* pixel data offset */
                /* BITMAPINFOHEADER (40 bytes) */
                hdr[14]=40;  /* header size */
                hdr[18]=W&0xFF; hdr[19]=(W>>8)&0xFF;
                hdr[22]=H&0xFF; hdr[23]=(H>>8)&0xFF;
                /* (negative height = top-down; use positive = bottom-up) */
                int negH = -H;
                hdr[22]=negH&0xFF; hdr[23]=(negH>>8)&0xFF; hdr[24]=(negH>>16)&0xFF; hdr[25]=(negH>>24)&0xFF;
                hdr[26]=1;  /* planes */
                hdr[28]=32; /* bits per pixel */
                fwrite(hdr, 1, 54, fp);
                /* Pixel data: s_out is ARGB (0xAARRGGBB) → BMP needs BGRA */
                for (int y = 0; y < H; y++) {
                    for (int x = 0; x < W; x++) {
                        uint32_t c = s_out[y * W + x];
                        uint8_t px[4] = {c&0xFF,(c>>8)&0xFF,(c>>16)&0xFF,0xFF};
                        fwrite(px, 1, 4, fp);
                    }
                }
                fclose(fp);
                HW_LOG("Frame %d saved to %s\n", dump_frame, path);
            }
        }
    }

    s_frame_num++;

    /* ── Test mode hooks (env vars) ──────────────────────────────────────── */
    {
        static int test_frames = -1;
        static int press_frame = -1;
        static int release_frame = -1;
        static int dump_parsed = 0;
        static int test_done = 0;
        static int frame_limit = 0;

        if (!dump_parsed) {
            dump_parsed = 1;
            const char *env;
            env = getenv("BENEFACTOR_TEST");
            if (env) test_frames = atoi(env);
            env = getenv("BENEFACTOR_PRESS");
            if (env) press_frame = atoi(env);
            env = getenv("BENEFACTOR_RELEASE");
            if (env) release_frame = atoi(env);
            env = getenv("BENEFACTOR_LIMIT");
            if (env) frame_limit = atoi(env);
            if (frame_limit > 0) hw_set_frame_limit(frame_limit);
        }

        if (press_frame >= 0 && (uint32_t)s_frame_num == (uint32_t)press_frame) {
            hw_set_fire(1);
            hw_set_mouse_lmb(1);
            GLOBAL_LOG( "[test] fire pressed at frame %d\n", s_frame_num);
        }
        if (release_frame >= 0 && (uint32_t)s_frame_num == (uint32_t)release_frame) {
            hw_set_fire(0);
            hw_set_mouse_lmb(0);
            GLOBAL_LOG( "[test] fire released at frame %d\n", s_frame_num);
        }

        if (test_frames > 0 && (uint32_t)s_frame_num >= (uint32_t)test_frames && !test_done) {
            test_done = 1;
            /* Dump memory regions from BENEFACTOR_DUMP */
            const char *dump_env = getenv("BENEFACTOR_DUMP");
            if (dump_env) {
                char buf[1024];
                strncpy(buf, dump_env, sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = '\0';
                char *tok = strtok(buf, ",");
                while (tok) {
                    unsigned long addr = 0; int len = 16;
                    if (sscanf(tok, "%lx:%d", &addr, &len) >= 1) {
                        addr &= 0xFFFFFF;
                        if (len > 256) len = 256;
                        GLOBAL_LOG( "[dump] $%06lX %d bytes:\n", addr, len);
                        for (int i = 0; i < len; i += 16) {
                            GLOBAL_LOG( "  %06lX:", addr + i);
                            for (int j = 0; j < 16 && i + j < len; j++) {
                                if (addr + i + j < RT_MEM_SIZE)
                                    GLOBAL_LOG( " %02X", g_mem[addr + i + j]);
                                else
                                    GLOBAL_LOG( " --");
                            }
                            GLOBAL_LOG( "\n");
                        }
                    }
                    tok = strtok(NULL, ",");
                }
            }
            hw_running = 0;
        }
    }

    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Framebuffer rendering (bitplane → RGB)                                       */
/* ─────────────────────────────────────────────────────────────────────────── */

void hw_set_pixel(int x, int y, uint32_t argb)
{
    if ((unsigned)x < HW_DISPLAY_W && (unsigned)y < HW_DISPLAY_H)
        s_fb[y * HW_DISPLAY_W + x] = argb;
}

void hw_set_color(int idx, uint16_t amiga_rgb12)
{
    if ((unsigned)idx < 32)
        s_palette[idx] = amiga_to_argb(amiga_rgb12);
}

void hw_set_frame_limit(int frames)
{
    s_frame_watchdog_limit = frames;
    s_frame_watchdog_count = 0;
}

/* ── Harness frame hook (NULL by default; set by benefactor-harness) ─────── */
void (*g_harness_frame_hook)(void) = NULL;
void (*g_harness_prerender_hook)(void) = NULL;

/* ── Snapshot of current HW state for the comparison harness ─────────────── */
#ifdef HARNESS_BUILD
void hw_get_snap(struct FrameState *s)
{
    uint32_t cop1lc = ((uint32_t)s_regs[COP1LCH >> 1] << 16) | s_regs[COP1LCL >> 1];
    s->cop1lc  = cop1lc & 0xFFFFFF;
    uint32_t cop2lc_r = ((uint32_t)s_regs[0x084 >> 1] << 16) | s_regs[0x086 >> 1];
    s->cop2lc  = cop2lc_r & 0xFFFFFF;
    /* Derive display registers from the copper list, matching PUAE's shadow
     * semantics.  At VBlank the copper restarts from cop1lc, runs the pre-WAIT
     * section (palette + display setup), then stalls.  The snapshot is taken
     * at that stall point, so pre-WAIT values are what PUAE reports.  For
     * registers not written before the first WAIT, the last post-WAIT write
     * from the previous frame's active scan is retained.
     *
     * There is no separate copper-execute pass (the old emulated path is gone), so
     * s_regs/s_bplcon0/s_bplptr are derived directly from the copper list here. */
    {
#define _NREGS 14
        static const uint16_t WATCH_REG[_NREGS] = {
            0x0100,0x0102,0x0104,0x0108,0x010A,  /* BPLCON0-2, BPL1/2MOD */
            0x008E,0x0090,0x0092,0x0094,          /* DIWSTRT/STOP, DDFSTRT/STOP */
            0x00E0,0x00E2,0x00E4,0x00E6,0x00E8    /* BPL1PTH/L BPL2PTH/L BPL3PTH */
        };
        uint16_t pre_val[_NREGS] = {0};
        uint16_t post_val[_NREGS] = {0};
        uint8_t  pre_set[_NREGS]  = {0};
        uint8_t  post_set[_NREGS] = {0};
        /* Extended: BPL3PTL through BPL6PTH/L */
        static const uint16_t EXT_REG[7] = {
            0x00EA,0x00EC,0x00EE,0x00F0,0x00F2,0x00F4,0x00F6
        };
        uint16_t ext_pre[7] = {0}, ext_post[7] = {0};
        uint8_t  ext_pre_set[7] = {0}, ext_post_set[7] = {0};

        uint32_t clc = cop1lc & 0xFFFFFF;
        if (clc >= 0x1000u && clc < 0x80000u && g_mem) {
            uint8_t *cp = g_mem + clc;
            int max_w = HARNESS_COPLIST_WORDS;
            int i = 0;
            /* Pass 1: pre-WAIT (first write wins) */
            for (; i + 1 < max_w; i += 2) {
                uint16_t w0 = (uint16_t)((cp[i*2]<<8)|cp[i*2+1]);
                uint16_t w1 = (uint16_t)((cp[i*2+2]<<8)|cp[i*2+3]);
                if (w0 == 0xFFFFu) { i = max_w; break; }
                if (w0 & 1u) break; /* first WAIT — end of pre-WAIT section */
                uint16_t reg = w0 & 0x01FEu;
                for (int r = 0; r < _NREGS; r++)
                    if (reg == WATCH_REG[r] && !pre_set[r]) { pre_val[r]=w1; pre_set[r]=1; }
                for (int r = 0; r < 7; r++)
                    if (reg == EXT_REG[r] && !ext_pre_set[r]) { ext_pre[r]=w1; ext_pre_set[r]=1; }
            }
            /* Pass 2: post-WAIT (last write wins) */
            for (; i + 1 < max_w; i += 2) {
                uint16_t w0 = (uint16_t)((cp[i*2]<<8)|cp[i*2+1]);
                uint16_t w1 = (uint16_t)((cp[i*2+2]<<8)|cp[i*2+3]);
                if (w0 == 0xFFFFu) break;
                if (w0 & 1u) continue;
                uint16_t reg = w0 & 0x01FEu;
                for (int r = 0; r < _NREGS; r++)
                    if (reg == WATCH_REG[r]) { post_val[r]=w1; post_set[r]=1; }
                for (int r = 0; r < 7; r++)
                    if (reg == EXT_REG[r]) { ext_post[r]=w1; ext_post_set[r]=1; }
            }
        }

#define _COP_REG(idx, fallback) (pre_set[idx] ? pre_val[idx] : (post_set[idx] ? post_val[idx] : (fallback)))
        s->bplcon0 = _COP_REG(0, s_bplcon0);
        s->bplcon1 = _COP_REG(1, s_regs[0x102>>1]);
        s->bplcon2 = _COP_REG(2, s_regs[0x104>>1]);
        s->bpl1mod = (int16_t)_COP_REG(3, s_regs[BPL1MOD>>1]);
        s->bpl2mod = (int16_t)_COP_REG(4, s_regs[BPL2MOD>>1]);
        s->diwstrt = _COP_REG(5, s_regs[DIWSTRT>>1]);
        s->diwstop = _COP_REG(6, s_regs[DIWSTOP>>1]);
        s->ddfstrt = _COP_REG(7, s_regs[DDFSTRT>>1]);
        s->ddfstop = _COP_REG(8, s_regs[DDFSTOP>>1]);
#undef _COP_REG
#undef _NREGS

        /* BPL pointers: assemble from high+low half, prefer pre-WAIT */
        static const int PTH_IDX[6] = {9,11,13,  /* pre/ext indices for BPL1-3 PTH */
                                        0, 2, 4}; /* ext indices for BPL4-6 PTH (0-indexed in EXT_REG) */
        /* Map per-plane: (pth_pre, pth_post, ptl_pre, ptl_post, fallback) */
        struct { uint8_t ph_src; uint16_t ph_pre, ph_post, pl_pre, pl_post; int pb; } bp[6];
        /* BPL1: WATCH_REG[9]=00E0, WATCH_REG[10]=00E2 */
        /* BPL2: WATCH_REG[11]=00E4, WATCH_REG[12]=00E6 */
        /* BPL3: WATCH_REG[13]=00E8, EXT_REG[0]=00EA */
        /* BPL4: EXT_REG[1]=00EC, EXT_REG[2]=00EE */
        /* BPL5: EXT_REG[3]=00F0, EXT_REG[4]=00F2 */
        /* BPL6: EXT_REG[5]=00F4, EXT_REG[6]=00F6 */
        (void)bp; (void)PTH_IDX;
        static const int WPH[6] = {9, 11, 13, -1, -1, -1};  /* WATCH_REG index for PTH, -1=ext */
        static const int WPL[6] = {10, 12, -1, -1, -1, -1}; /* WATCH_REG index for PTL, -1=ext */
        static const int EPH[6] = {-1, -1, -1,  1,  3,  5}; /* EXT_REG index for PTH, -1=watch */
        static const int EPL[6] = {-1, -1,  0,  2,  4,  6}; /* EXT_REG index for PTL, -1=watch */
        for (int p = 0; p < 6; p++) {
            uint16_t ph, pl;
            uint8_t ph_set, pl_set;
            if (WPH[p] >= 0) {
                ph = pre_set[WPH[p]] ? pre_val[WPH[p]] : post_val[WPH[p]];
                ph_set = pre_set[WPH[p]] | post_set[WPH[p]];
            } else {
                int ei = EPH[p];
                ph = ext_pre_set[ei] ? ext_pre[ei] : ext_post[ei];
                ph_set = ext_pre_set[ei] | ext_post_set[ei];
            }
            if (WPL[p] >= 0) {
                pl = pre_set[WPL[p]] ? pre_val[WPL[p]] : post_val[WPL[p]];
                pl_set = pre_set[WPL[p]] | post_set[WPL[p]];
            } else {
                int ei = EPL[p];
                pl = ext_pre_set[ei] ? ext_pre[ei] : ext_post[ei];
                pl_set = ext_pre_set[ei] | ext_post_set[ei];
            }
            if (ph_set || pl_set)
                s->bplpt[p] = (((uint32_t)ph << 16) | pl) & 0xFFFFFF;
            else
                s->bplpt[p] = s_bplptr[p];
        }
    }

    for (int i = 0; i < 8; i++)
        s->sprpt[i] = s_sprpt[i];

    /* Palette from copper list — two-pass scan to replicate PUAE hardware shadow.
     *
     * Why two passes?  PUAE's color_regs_ecs[] = the value the Amiga copper last
     * wrote to each COLOR register.  At snapshot time (VBlank / frame start), the
     * copper has just restarted from cop1lc, written the static palette section
     * (before the first WAIT), then stalled.  So:
     *
     *   • Registers written BEFORE the first WAIT → VBlank copper wrote them last;
     *     PUAE shadow = the pre-WAIT value.
     *   • Registers written ONLY AFTER a WAIT → VBlank copper never reached them;
     *     PUAE shadow = the last value from the previous frame's active scan.
     *     The last write in the full copper list is identical to what the active
     *     scan wrote, so we take the LAST write from the full list for these.
     *
     * Two-pass algorithm:
     *   Pass 1 (pre-WAIT): scan until the first WAIT/END; record all COLOR writes
     *                       into pre_pal[]; mark pre_written[].
     *   Pass 2 (post-WAIT): continue from that WAIT to end of list; record COLOR
     *                        writes into post_pal[] (last-write wins); mark
     *                        post_written[].
     *   Merge: use pre_pal[i] if pre_written[i], else post_pal[i] if post_written[i],
     *          else fall back to runtime s_palette[].
     */
    {
        uint16_t pre_pal[32]  = {0};
        uint16_t post_pal[32] = {0};
        uint8_t  pre_written[32]  = {0};
        uint8_t  post_written[32] = {0};

        uint32_t cop1lc_tmp = ((uint32_t)s_regs[COP1LCH >> 1] << 16) | s_regs[COP1LCL >> 1];
        cop1lc_tmp &= 0xFFFFFF;
        if (cop1lc_tmp && g_mem && cop1lc_tmp + HARNESS_COPLIST_WORDS * 2 < RT_MEM_SIZE) {
            uint8_t *cp = g_mem + cop1lc_tmp;
            int i = 0;

            /* Pass 1: pre-WAIT section */
            for (; i + 1 < HARNESS_COPLIST_WORDS; i += 2) {
                uint16_t w0 = (uint16_t)((cp[i * 2] << 8) | cp[i * 2 + 1]);
                uint16_t w1 = (uint16_t)((cp[i * 2 + 2] << 8) | cp[i * 2 + 3]);
                if (w0 == 0xFFFFu) { i = HARNESS_COPLIST_WORDS; break; } /* end sentinel */
                if (w0 & 1u) break; /* first WAIT/SKIP — end of pre-WAIT section */
                uint16_t reg = w0 & 0x01FEu;
                if (reg >= 0x180u && reg <= 0x1BEu) {
                    int idx = (reg - 0x180u) >> 1;
                    pre_pal[idx] = w1 & 0x0FFFu;
                    pre_written[idx] = 1;
                }
            }

            /* Pass 2: post-WAIT section (last-write wins for retained hw state) */
            for (; i + 1 < HARNESS_COPLIST_WORDS; i += 2) {
                uint16_t w0 = (uint16_t)((cp[i * 2] << 8) | cp[i * 2 + 1]);
                uint16_t w1 = (uint16_t)((cp[i * 2 + 2] << 8) | cp[i * 2 + 3]);
                if (w0 == 0xFFFFu) break;
                if (w0 & 1u) continue; /* additional WAITs — skip, keep scanning */
                uint16_t reg = w0 & 0x01FEu;
                if (reg >= 0x180u && reg <= 0x1BEu) {
                    int idx = (reg - 0x180u) >> 1;
                    post_pal[idx] = w1 & 0x0FFFu;
                    post_written[idx] = 1;
                }
            }
        }

        /* Merge: pre-WAIT > post-WAIT > runtime shadow fallback */
        for (int i = 0; i < 32; i++) {
            if (pre_written[i]) {
                s->palette[i] = pre_pal[i];
            } else if (post_written[i]) {
                s->palette[i] = post_pal[i];
            } else {
                /* No copper write found; fall back to runtime ARGB shadow */
                uint32_t argb = s_palette[i];
                uint8_t r = (argb >> 16) & 0xFF;
                uint8_t g = (argb >>  8) & 0xFF;
                uint8_t b = (argb      ) & 0xFF;
                s->palette[i] = (uint16_t)(((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));
            }
        }
    }

    s->coplist_valid = 0;
    if (s->cop1lc + HARNESS_COPLIST_WORDS * 2 < RT_MEM_SIZE) {
        uint8_t *p = g_mem + s->cop1lc;
        for (int i = 0; i < HARNESS_COPLIST_WORDS; i++)
            s->coplist[i] = (uint16_t)((p[i * 2] << 8) | p[i * 2 + 1]);
        s->coplist_valid = 1;
    }

    /* Paula audio channels — read from hardware register shadows */
    static const uint16_t aud_base[] = { 0x0A0, 0x0B0, 0x0C0, 0x0D0 };
    for (int n = 0; n < 4; n++) {
        uint16_t b = aud_base[n];
        s->audio[n].lc  = ((uint32_t)s_regs[(b + 0) >> 1] << 16) |
                                      s_regs[(b + 2) >> 1];
        s->audio[n].len = s_regs[(b + 4) >> 1];
        s->audio[n].per = s_regs[(b + 6) >> 1];
        s->audio[n].vol = s_regs[(b + 8) >> 1];
    }

    /* Bitplane data CRC — used as a DIFF trigger */
    s->bpl_data_crc = g_mem ? bpl_data_region_crc(g_mem, RT_MEM_SIZE) : 0;

    /* Animation phase tracking */
    s->flip_flop  = g_mem ? (uint16_t)((((uint8_t*)g_mem)[0x41A2] << 8) | ((uint8_t*)g_mem)[0x41A3]) : 0;
    s->sprite_ctr = g_mem ? (uint16_t)((((uint8_t*)g_mem)[0x42FC] << 8) | ((uint8_t*)g_mem)[0x42FD]) : 0;
    {
        uint8_t *m = (uint8_t*)g_mem;
        s->anim_ptr = m ? (((uint32_t)m[0x42FE] << 24) | ((uint32_t)m[0x42FF] << 16) |
                           ((uint32_t)m[0x4300] << 8)  |  (uint32_t)m[0x4301]) : 0;
    }
}
#endif /* HARNESS_BUILD */

/* Load audio register sync state written by puae_dump_audio_regs() and apply
 * it to the hardware register shadows.  Call this after hw_init() and before
 * the first frame so the PC port's audio shadows start at the same state as
 * PUAE had at the chip-RAM sync point. */
#ifdef HARNESS_BUILD
void hw_load_audio_sync(const char *path)
{
    AudioChanSnap snap[4];
    FILE *fp = fopen(path, "rb");
    if (!fp) return;
    if (fread(snap, sizeof(snap), 1, fp) != 1) { fclose(fp); return; }
    fclose(fp);

    static const uint16_t aud_base[] = { 0x0A0, 0x0B0, 0x0C0, 0x0D0 };
    for (int n = 0; n < 4; n++) {
        uint16_t b = aud_base[n];
        s_regs[(b + 0) >> 1] = (uint16_t)(snap[n].lc >> 16);
        s_regs[(b + 2) >> 1] = (uint16_t)(snap[n].lc & 0xFFFF);
        s_regs[(b + 4) >> 1] = snap[n].len;
        s_regs[(b + 6) >> 1] = snap[n].per;
        s_regs[(b + 8) >> 1] = snap[n].vol;
    }
}
#endif

/* Seed hardware-register shadows from a PUAE frame snapshot.
 * Called by the harness after pc_init_from_chip_dump() to bring s_palette[],
 * s_bplptr[], and audio vol shadows in sync with PUAE boot state.
 * Covers registers NOT present in chip RAM (custom chip shadows only). */
#ifdef HARNESS_BUILD
void hw_seed_sync_regs(const struct FrameState *snap)
{
    /* Palette: seed all 32; hw_get_snap's copper scan overrides 0-20 anyway */
    for (int i = 0; i < 32; i++)
        s_palette[i] = amiga_to_argb(snap->palette[i]);

    /* Bitplane pointers: seed all 6; copper scan overrides 0-3 */
    for (int i = 0; i < 6; i++)
        s_bplptr[i] = snap->bplpt[i];

    /* Seed the full Paula state per channel (sample pointer, length, period,
     * volume) from PUAE's sync-point state.  These registers are NOT in chip RAM,
     * so without this the channels keep stale boot values — e.g. ch3 kept playing
     * the one-shot from the sample START while PUAE had already advanced AUDxLC/LEN
     * to the loop/sustain point, producing audibly different audio.  Then restart
     * the channel playback from the seeded registers so the mixer's live pointer
     * matches. */
    static const uint16_t aud_base[] = { 0x0A0, 0x0B0, 0x0C0, 0x0D0 };
    for (int n = 0; n < 4; n++) {
        uint16_t b = aud_base[n];
        s_regs[(b + 0) >> 1] = (uint16_t)(snap->audio[n].lc >> 16);
        s_regs[(b + 2) >> 1] = (uint16_t)(snap->audio[n].lc & 0xFFFF);
        s_regs[(b + 4) >> 1] = snap->audio[n].len;
        s_regs[(b + 6) >> 1] = snap->audio[n].per;
        s_regs[(b + 8) >> 1] = snap->audio[n].vol;
    }
    hw_audio_resync();
}
#endif /* HARNESS_BUILD */

/* Copper execution and rendering are implemented in hw_copper.c */

/* ─────────────────────────────────────────────────────────────────────────── */
/* Disk loading                                                                 */
/* ─────────────────────────────────────────────────────────────────────────── */

int hw_load_disk(int disk, uint32_t offset, uint32_t len, uint32_t dst_amiga)
{
    if (disk < 1 || disk > s_n_disks || !s_disk_paths[disk-1]) {
        HW_LOG("hw_load_disk: no disk %d\n", disk);
        return -1;
    }
    FILE *fp = fopen(s_disk_paths[disk-1], "rb");
    if (!fp) {
        HW_LOG("Cannot open disk %d: %s\n", disk, s_disk_paths[disk-1]);
        return -1;
    }
    fseek(fp, (long)offset, SEEK_SET);
    uint8_t *dst = g_mem + (dst_amiga & 0xFFFFFF);
    size_t  n   = fread(dst, 1, len, fp);
    fclose(fp);
    HW_LOG("Disk %d: loaded %zu/%u bytes at $%06X (off $%X)\n",
            disk, n, len, dst_amiga, offset);
    return (n == len) ? 0 : -1;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Input                                                                        */
/* ─────────────────────────────────────────────────────────────────────────── */

uint16_t hw_joystick(void)
{
    /* Encode a digital joystick into JOYxDAT exactly as PUAE does
     * (inputdevice.c: joymousecounter + getjoystate), so the game's quadrature
     * decode at $578E62 yields clean directions. JOYxDAT = mouse_x (low byte) |
     * mouse_y<<8 (high byte); for held directions the low 2 bits of each are:
     *   b0 = bot ^ right,  b1 = right pressed,  b8 = top ^ left,  b9 = left pressed
     * (active-low: left/right/top/bot = 0 when that direction is held).
     * Cardinal results: RIGHT=$0003 LEFT=$0300 UP=$0100 DOWN=$0001.
     * (The previous single-bit guess mapped right->UP and up->DOWN, which made
     * the player hop diagonally / roll instead of walking.) */
    int left  = s_joy_left  ? 0 : 1;
    int right = s_joy_right ? 0 : 1;
    int top   = s_joy_up    ? 0 : 1;
    int bot   = s_joy_down  ? 0 : 1;
    uint16_t mx = (uint16_t)(((bot ^ right) ? 1 : 0) | ((right ^ 1) ? 2 : 0));
    uint16_t my = (uint16_t)(((top ^ left)  ? 1 : 0) | ((left  ^ 1) ? 2 : 0));
    return (uint16_t)((mx & 0xFF) | (my << 8));
}

uint8_t hw_cia_keyboard(void)
{
    return s_key_byte;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Register-level I/O (called from rt.c)                                        */
/* ─────────────────────────────────────────────────────────────────────────── */

/* ── Read ── */

uint8_t hw_read8(uint32_t addr)
{
    addr &= 0xFFFFFF;
    /* CIAs are byte-wide; do not reconstruct from a 16-bit word. */
    if ((addr >= 0xBFD000 && addr <= 0xBFDFFF) ||
        (addr >= 0xBFE000 && addr <= 0xBFEFFF)) {
        uint16_t w = hw_read16(addr & ~1u);
        /* Even addresses in CIA-B space hold the register value in the
         * lower byte of the 16-bit data bus; odd addresses in CIA-A
         * space do the same.  Reconstruct accordingly. */
        if (addr >= 0xBFD000 && addr <= 0xBFDFFF) {
            /* CIA-B even addresses → lower byte on Amiga data bus */
            return (uint8_t)(w & 0xFF);
        } else {
            /* CIA-A odd addresses → lower byte */
            return (uint8_t)(w & 0xFF);
        }
    }
    uint16_t w = hw_read16(addr & ~1u);
    return (addr & 1) ? (uint8_t)(w & 0xFF) : (uint8_t)(w >> 8);
}

volatile uint32_t g_hw_last_read = 0;   /* last hw register address read (watchdog diagnostic) */

/* ── Frame watchdog ──────────────────────────────────────────────────────────
 * Arm before stepping a single frame; if that frame doesn't finish within a few
 * seconds it's an infinite loop (typically an interrupt/beam busy-wait that never
 * gets satisfied). SIGALRM then reports the likely cause and kills the app. */
static volatile const char *s_wd_what = NULL;
static void hw_watchdog_handler(int sig)
{
    (void)sig;
    extern volatile uint32_t g_rt_last_call;
    uint32_t cop = ((uint32_t)s_regs[0x080 >> 1] << 16) | s_regs[0x082 >> 1];
    char buf[320];
    int n = snprintf(buf, sizeof buf,
        "\n[watchdog] '%s' frame exceeded the time limit — infinite loop "
        "(interrupt/beam busy-wait?).\n"
        "[watchdog]   last recompiled fn entered = $%06X, last hw read = $%06X, "
        "cop1lc = $%06X. Killing.\n",
        s_wd_what ? (const char *)s_wd_what : "?", g_rt_last_call, g_hw_last_read, cop);
    if (n > 0) { ssize_t w = write(2, buf, (size_t)n); (void)w; }
    /* Dump the recent rt_call chain (oldest..newest) — shows the handlers that
     * led into the spin, not just the last one (which has usually returned). */
    { extern int rt_recent_snapshot(uint32_t *out, int max);
      uint32_t recent[48]; int rn = rt_recent_snapshot(recent, 48);
      char rb[640]; int rp = snprintf(rb, sizeof rb, "[watchdog]   recent calls:");
      for (int i = 0; i < rn && rp < (int)sizeof rb - 10; i++)
          rp += snprintf(rb + rp, sizeof rb - rp, " $%06X", recent[i]);
      rp += snprintf(rb + rp, sizeof rb - rp, "\n");
      ssize_t w = write(2, rb, (size_t)rp); (void)w; }
    /* Dump the last instruction PCs (trace build only) — the spin loop itself. */
    { extern int rt_insn_ring_snapshot(uint32_t *out, int max);
      uint32_t ins[96]; int in = rt_insn_ring_snapshot(ins, 96);
      if (in > 0 && ins[in-1]) {
          char ib[900]; int ip = snprintf(ib, sizeof ib, "[watchdog]   last insns:");
          for (int i = 0; i < in && ip < (int)sizeof ib - 10; i++)
              ip += snprintf(ib + ip, sizeof ib - ip, " %06X", ins[i]);
          ip += snprintf(ib + ip, sizeof ib - ip, "\n");
          ssize_t w = write(2, ib, (size_t)ip); (void)w; } }
    /* Dump PC memory at the freeze so the stuck routine's data can be inspected. */
    { extern uint8_t *g_mem;
      if (g_mem) { int fd = open("logs/pc_freeze.bin", O_WRONLY|O_CREAT|O_TRUNC, 0644);
                   if (fd >= 0) { ssize_t w = write(fd, g_mem, 0x600000); (void)w; close(fd);
                                  const char *m = "[watchdog]   dumped PC memory -> logs/pc_freeze.bin\n";
                                  ssize_t w2 = write(2, m, strlen(m)); (void)w2; } } }
    _exit(2);
}
void hw_watchdog_arm(const char *what, int seconds)
{
    static int inited = 0;
    if (!inited) { signal(SIGALRM, hw_watchdog_handler); inited = 1; }
    s_wd_what = what;
    alarm((unsigned)(seconds > 0 ? seconds : 2));
}
void hw_watchdog_disarm(void) { alarm(0); }

uint16_t hw_read16(uint32_t addr)
{
    addr &= 0xFFFFFF;
    g_hw_last_read = addr;

    /* Native disk-boot hand-off: the cold-start ($3000) runs the title loop,
     * which re-reads $BFE000 (port-0 fire) every iteration. Once the title
     * copper is up (cop1lc = $7BC8/$86CC), the boot is done — break out of the
     * recompiled cold-start (via the hook's longjmp) so the frame-driven loop
     * (pc_step) can take over. */
    if (g_hw_boot_handoff && addr == 0xBFE000u) {
        uint32_t cop = ((uint32_t)s_regs[0x080>>1] << 16) | s_regs[0x082>>1];
        if (cop == 0x7BC8u || cop == 0x86CCu) g_hw_boot_handoff();
    }

    /* ── CIA-B ($BFD000) ── */
    if (addr >= 0xBFD000 && addr <= 0xBFDFFF) {
        int reg = (addr >> 8) & 0xF;
        switch (reg) {
        case CIA_TALO: return s_ciab_ta_cnt & 0xFF;
        case CIA_TAHI: return s_ciab_ta_cnt >> 8;
        case CIA_ICR: { uint8_t d = s_ciab_icr_data;
                        s_ciab_icr_data = 0;
                        if (d & s_ciab_icr_mask) d |= 0x80;
                        return d; }
        case CIA_PRA:  return 0xFF;   /* disk status – ready */
        default:       return 0xFF;
        }
    }

    /* ── CIA-A ($BFE001) ── */
    if (addr >= 0xBFE000 && addr <= 0xBFEFFF) {
        switch ((addr >> 8) & 0xF) {
        case CIA_PRA: {
            /* CIA-A PRA: bit6 = /FIR0 (port-0 / mouse-left), bit7 = /FIR1
             * (port-1 joystick fire), active-LOW. Gameplay reads bit7 for the
             * player's fire button (e.g. $578CFE tst.b $bfe001; bpl), so the
             * fire button must clear bit7 too — not only the mouse button. */
            uint8_t val = 0xFF;
            if (s_fire_pressed)  val &= ~0x01;
            if (s_fire_pressed)  val &= ~0x40;
            if (s_fire_pressed || s_mouse_lmb) val &= ~0x80;
            { static int t=-1; if(t<0) t=getenv("BFE_TRACE")?1:0;
              if(t){ extern uint32_t rt_get_last_insn(void); 
                     static uint32_t seen[64]; static int n=0; uint32_t pc=rt_get_last_insn();
                     int k=0; for(;k<n;k++) if(seen[k]==pc) break;
                     if(k==n && n<64 && g_gameplay_active){ seen[n++]=pc;
                       fprintf(stderr,"[bfe-read] $BFE001 -> %02X from pc=$%06X fire=%d\n",val,pc,s_fire_pressed); fflush(stderr);} } }
            return val;
        }
        case CIA_PRB:  return 0xFF;
        default:       return 0xFF;
        }
    }

    /* ── Custom chips ($DFF000) ── */
    if (addr >= 0xDFF000 && addr <= 0xDFFFFF) {
        uint32_t reg = addr & 0x1FE;
        switch (reg) {
        case DMACONR: {
            /* BBUSY (bit 15) is 0 when blitter idle; BZERO (bit 14) latches
             * when blit completes.  Reading DMACONR clears BZERO. */
            uint16_t val = s_dmacon | (s_blt_bzero ? 0x4000 : 0);
            s_blt_bzero = 0;
            return val;
        }
        case VPOSR: {
            /* Simulated beam: each VPOSR read advances the beam counter.
             * After ~5000 reads, a frame boundary occurs.  This keeps
             * the VPOSR wait loops spinning at a reasonable rate while
             * still generating frame boundaries for sync. The game's actual
             * vblank-sync spins (btst #0,$3/$5(a6) pairs) are folded by the
             * recompiler into explicit hw_vblank_wait() calls, so this path is
             * only a fallback for incidental VPOSR reads — it must NOT yield. */
            s_vposr_counter++;
            int do_frame = (s_vposr_counter >= 5000);
            if (do_frame) {
                s_vposr_counter = 0;
                s_scanline = 0;
                s_frame_num++;
                /* Presentation must have exactly ONE driver. When pc.c owns the
                 * frame loop (every current path), it presents explicitly once
                 * per frame; auto-presenting here too would inject a second
                 * present at a different beam phase → temporal double-buffer
                 * zigzag. Keep advancing the beam/frame counter (so the game's
                 * VPOSR wait-loops still terminate) but do NOT present. */
                if (!g_hw_pc_owns_present &&
                    hw_running && hw_present_frame() != 0) {
                    hw_running = 0;
                    s_scanline = 999;
                    return 0;
                }
            }
            uint16_t lof = 1;
            uint16_t val = (uint16_t)((lof << 15) | (s_frame_num & 1));
            return val;
        }
        case VHPOSR:
            {
                /* The gameplay code frame-syncs by busy-waiting for a specific
                 * beam scanline (cmpi.b #imm,$6(a6); bne) — inline loops that
                 * never call hw_vblank_wait. Different code paths wait for
                 * DIFFERENT lines: the main loop waits $3B ($577130/$5772A8/…)
                 * but the level-setup path waits $3A ($578472). A fixed return
                 * value satisfies one and hangs the other (this was the bug that
                 * stuck the level-intro card forever).
                 *
                 * Emulate a real advancing beam: each read steps the counter, so
                 * EVERY target line is hit eventually. When it wraps past the
                 * frame bottom that's one frame boundary → yield (present + let
                 * pc_step_coro deliver the gameplay IRQs). Incrementing BEFORE the
                 * return means a repeated wait for the same line must traverse a
                 * full frame (exactly one yield) rather than matching instantly. */
                
                if (g_gameplay_active && g_hw_vblank_yield) {
                    /* Model an advancing beam: each read steps one scanline. A
                     * beam-wait loop (cmpi.b #line,$6(a6); bne) exits when the
                     * beam reaches ITS target line — naturally, once per frame,
                     * whatever line it waits for ($3B main loop / $3A level-setup).
                     * Yield (present + deliver the level-3/6 IRQs) ONLY when the
                     * beam wraps to the top = exactly one displayed frame.
                     *
                     * The previous version yielded on EVERY read and returned the
                     * target line immediately: a single game-frame issues several
                     * beam reads, so the game advanced several presents per frame
                     * -> it ran ~4x too slow, stuttered (repeated frames), and the
                     * GET-READY banner phase never elapsed. One-yield-per-wrap puts
                     * one game-frame per displayed frame. */
                    static unsigned beam = 0;
                    beam++;
                    if (beam >= 312u) {          /* PAL frame = 312 scanlines */
                        beam = 0;
                        g_hw_vblank_yield();
                    }
                    return (uint16_t)((beam & 0xFFu) << 8);
                }
                /* Fast toggling bit 0 — no real-time delay so VPOSR wait
                 * loops complete instantly instead of spinning for 20ms. */
                static uint8_t vh_toggle = 0;
                vh_toggle ^= 1;
                return (uint16_t)(((s_scanline & 0xFF) << 8) | vh_toggle);
            }
        case JOY0DAT:  {
            uint16_t v = hw_joystick();
            HW_LOG("JOY0DAT read -> $%04X (up=%d dn=%d lt=%d rt=%d)\n",
                    v, s_joy_up, s_joy_down, s_joy_left, s_joy_right);
            return v;
        }
        case JOY1DAT:  return hw_joystick();   /* player joystick is on port 1 (the gameplay reads $c(a6)) */
        case POT0DAT:
        case POT1DAT:
            {
                uint16_t v = 0;
                if (s_fire_pressed) v = 0x0000;
                else v = 0x0300;
                HW_LOG("POT%cDAT read fire=%d -> $%04X\n",
                        (reg == POT0DAT) ? '0' : '1', s_fire_pressed, v);
                return v;
            }
        case POTINP:
            {
                uint8_t v = 0xFF;
                if (s_fire_pressed)     v &= ~0x40;
                if (s_mouse_lmb)        v &= ~0x10;
                HW_LOG("POTINP read fire=%d lmb=%d -> $%02X\n",
                        s_fire_pressed, s_mouse_lmb, v);
                return (uint16_t)(v << 8) | v;
            }
        default:
            return s_regs[reg >> 1];
        }
    }

    return 0;
}

uint32_t hw_read32(uint32_t addr)
{
    return ((uint32_t)hw_read16(addr) << 16) | hw_read16(addr + 2);
}

/* ── Write ── */

void hw_write8(uint32_t addr, uint8_t v)
{
    addr &= 0xFFFFFF;
    /* CIAs are byte-wide; do not reconstruct from a 16-bit word. */
    if ((addr >= 0xBFD000 && addr <= 0xBFDFFF) ||
        (addr >= 0xBFE000 && addr <= 0xBFEFFF)) {
        /* On Amiga, byte writes to CIA space land in the lower byte of
         * the 16-bit data bus.  Reconstruct the 16-bit word accordingly
         * so hw_write16 sees the byte value in its lower 8 bits. */
        hw_write16(addr & ~1u, (uint16_t)v);
        return;
    }
    /* Reconstruct 16-bit write (read-modify-write) for custom chips */
    uint16_t cur = hw_read16(addr & ~1u);
    if (addr & 1)
        hw_write16(addr & ~1u, (uint16_t)((cur & 0xFF00) | v));
    else
        hw_write16(addr & ~1u, (uint16_t)((cur & 0x00FF) | ((uint16_t)v << 8)));
}

static inline uint32_t _bplptr(int hi_reg, int lo_reg)
{
    return (((uint32_t)s_regs[hi_reg >> 1] << 16) | s_regs[lo_reg >> 1]) & 0xFFFFFF;
}

void hw_write16(uint32_t addr, uint16_t v)
{
    addr &= 0xFFFFFF;

    /* ── CIA-B ($BFD000) ── */
    if (addr >= 0xBFD000 && addr <= 0xBFDFFF) {
        int reg = (addr >> 8) & 0xF;
        switch (reg) {
        case CIA_TALO: s_ciab_ta_latch = (s_ciab_ta_latch & 0xFF00) | (v & 0xFF); break;
        case CIA_TAHI:
            s_ciab_ta_latch = (s_ciab_ta_latch & 0x00FF) | (uint16_t)((v & 0xFF) << 8);
            /* In one-shot mode with timer not running, signal immediately */
            if (!(s_ciab_cra & 1))
                s_ciab_icr_data |= 0x01;
            break;
        case CIA_CRA:
            s_ciab_cra = (uint8_t)v;
            if (v & 0x10) { s_ciab_ta_cnt = s_ciab_ta_latch; }  /* LOAD strobe */
            break;
        case CIA_ICR:
            if (v & 0x80)  s_ciab_icr_mask |=  (uint8_t)(v & 0x7F);
            else           s_ciab_icr_mask &= ~(uint8_t)(v & 0x7F);
            break;
        default: break;
        }
        return;
    }

    /* ── CIA-A ($BFE000) ── */
    if (addr >= 0xBFE000 && addr <= 0xBFEFFF) {
        /* Mostly ignore CIA-A writes (serial / keyboard handled separately) */
        return;
    }

    /* ── Custom chips ── */
    if (addr >= 0xDFF000 && addr <= 0xDFFFFF) {
        uint32_t reg = addr & 0x1FE;
        s_regs[reg >> 1] = v;   /* shadow copy */

        /* COP1LCL completes the 32-bit COP1LC write (the game uses move.l to
         * $7e(a6)): the display copper pointer is now committed — present here. */
        if (reg == COP1LCL && g_hw_cop1lc_present)
            g_hw_cop1lc_present();
        { 
          if ((reg == COP1LCL || reg == COP1LCH) && g_gameplay_active && getenv("GP_COP_TRACE")) {
              uint32_t c = ((uint32_t)s_regs[COP1LCH>>1]<<16)|s_regs[COP1LCL>>1];
              static FILE *cf = NULL; static int tried = 0;
              if (!tried) { tried = 1; cf = fopen("logs/gp_cop_trace.txt","w"); }
              if (cf) { fprintf(cf, "COP1LC=$%06X\n", c & 0xFFFFFF); fflush(cf); }
          } }

        switch (reg) {
        /* DMA control */
        case DMACON:
            if (v & 0x8000) s_dmacon |=  (v & 0x7FFF);
            else {
                s_dmacon &= ~(v & 0x7FFF);
                /* Audio DMA disabled for a channel => stop it. The game gates
                 * one-shot samples by clearing the channel's DMACON bit; the
                 * mixer otherwise loops the sample forever (the repeating
                 * intro / ringing notes). */
                for (int ch = 0; ch < 4; ch++)
                    if (v & (1u << ch)) s_audio[ch].active = 0;
            }
            if (getenv("BENEFACTOR_AUD_TRACE") && (v & 0x000F))
                fprintf(stderr, "[dma] f=%d DMACON %s aud=$%X (s_dmacon aud=$%X)\n",
                        hw_get_frame_num(), (v & 0x8000) ? "SET" : "CLR",
                        v & 0xF, s_dmacon & 0xF);
            /* SFX_TRACE: on audio-channel DMA ENABLE, log which sample (AUDxLC),
             * length, period — one line per "note/SFX on" so we can diff PC vs
             * PUAE jump sounds. */
            if ((v & 0x8000) && (v & 0x000F) && getenv("SFX_TRACE")) {
                static FILE *sf = NULL; if (!sf) sf = fopen("logs/sfx_pc.txt", "w");
                extern volatile uint32_t g_rt_last_call;
                if (sf) for (int ch = 0; ch < 4; ch++) if (v & (1u << ch)) {
                    int b = (AUD0LCH + ch * 0x10) >> 1;
                    uint32_t lc = ((uint32_t)s_regs[b] << 16) | s_regs[b + 1];
                    fprintf(sf, "f=%d ch%d LC=%06X LEN=%04X PER=%04X VOL=%02X fn=%06X\n",
                            hw_get_frame_num(), ch, lc & 0xFFFFFF, s_regs[b + 2],
                            s_regs[b + 3], s_regs[b + 4] & 0x7F,
                            (unsigned)g_rt_last_call);
                    fflush(sf);
                }
            }
            break;
        /* Interrupt */
        case INTENA:
            if (v & 0x8000) s_intena |=  (v & 0x7FFF);
            else            s_intena &= ~(v & 0x7FFF);
            break;
        case INTREQ:
            if (v & 0x8000) s_intreq |=  (v & 0x7FFF);
            else            s_intreq &= ~(v & 0x7FFF);
            break;
        /* BPLCON0 */
        case BPLCON0:
            s_bplcon0 = v;
            HW_LOG("BPLCON0 = $%04X (planes=%d)\n", v, (v>>12)&7);
            break;
        /* Bitplane pointers */
        case BPL1PTL: s_regs[BPL1PTL>>1]=v; s_bplptr[0] = _bplptr(BPL1PTH, BPL1PTL); HW_LOG("BPL1PT = $%06X\n", s_bplptr[0]); HWTRACE("[HWTRACE] BPL1PT=$%06X (lo write=$%04X)\n", s_bplptr[0], v); break;
        case BPL1PTH: s_regs[BPL1PTH>>1]=v; s_bplptr[0] = _bplptr(BPL1PTH, BPL1PTL); HW_LOG("BPL1PTH write -> BPL1PT = $%06X\n", s_bplptr[0]); HWTRACE("[HWTRACE] BPL1PT=$%06X (hi write=$%04X)\n", s_bplptr[0], v); break;
        case BPL2PTL: s_regs[BPL2PTL>>1]=v; s_bplptr[1] = _bplptr(BPL2PTH, BPL2PTL); HW_LOG("BPL2PT = $%06X\n", s_bplptr[1]); HWTRACE("[HWTRACE] BPL2PT=$%06X (lo write=$%04X)\n", s_bplptr[1], v); break;
        case BPL2PTH: s_regs[BPL2PTH>>1]=v; s_bplptr[1] = _bplptr(BPL2PTH, BPL2PTL); HW_LOG("BPL2PTH write -> BPL2PT = $%06X\n", s_bplptr[1]); HWTRACE("[HWTRACE] BPL2PT=$%06X (hi write=$%04X)\n", s_bplptr[1], v); break;
        case BPL3PTL: s_regs[BPL3PTL>>1]=v; s_bplptr[2] = _bplptr(BPL3PTH, BPL3PTL); HW_LOG("BPL3PT = $%06X\n", s_bplptr[2]); HWTRACE("[HWTRACE] BPL3PT=$%06X (lo write=$%04X)\n", s_bplptr[2], v); break;
        case BPL3PTH: s_regs[BPL3PTH>>1]=v; s_bplptr[2] = _bplptr(BPL3PTH, BPL3PTL); HW_LOG("BPL3PTH write -> BPL3PT = $%06X\n", s_bplptr[2]); HWTRACE("[HWTRACE] BPL3PT=$%06X (hi write=$%04X)\n", s_bplptr[2], v); break;
        case BPL4PTL: s_regs[BPL4PTL>>1]=v; s_bplptr[3] = _bplptr(BPL4PTH, BPL4PTL); HW_LOG("BPL4PT = $%06X\n", s_bplptr[3]); HWTRACE("[HWTRACE] BPL4PT=$%06X (lo write=$%04X)\n", s_bplptr[3], v); break;
        case BPL4PTH: s_regs[BPL4PTH>>1]=v; s_bplptr[3] = _bplptr(BPL4PTH, BPL4PTL); HW_LOG("BPL4PTH write -> BPL4PT = $%06X\n", s_bplptr[3]); HWTRACE("[HWTRACE] BPL4PT=$%06X (hi write=$%04X)\n", s_bplptr[3], v); break;
        case BPL5PTL: s_regs[BPL5PTL>>1]=v; s_bplptr[4] = _bplptr(BPL5PTH, BPL5PTL); HW_LOG("BPL5PT = $%06X\n", s_bplptr[4]); break;
        case BPL5PTH: s_regs[BPL5PTH>>1]=v; s_bplptr[4] = _bplptr(BPL5PTH, BPL5PTL); HW_LOG("BPL5PTH write -> BPL5PT = $%06X\n", s_bplptr[4]); break;
        case BPL6PTL: s_regs[BPL6PTL>>1]=v; s_bplptr[5] = _bplptr(BPL6PTH, BPL6PTL); HW_LOG("BPL6PT = $%06X\n", s_bplptr[5]); break;
        case BPL6PTH: s_regs[BPL6PTH>>1]=v; s_bplptr[5] = _bplptr(BPL6PTH, BPL6PTL); HW_LOG("BPL6PTH write -> BPL6PT = $%06X\n", s_bplptr[5]); break;
        /* Sprite pointers */
        case SPR0PTL: s_regs[SPR0PTL>>1]=v; s_sprpt[0] = _bplptr(SPR0PTH, SPR0PTL); break;
        case SPR0PTH: s_regs[SPR0PTH>>1]=v; s_sprpt[0] = _bplptr(SPR0PTH, SPR0PTL); break;
        case SPR1PTL: s_regs[SPR1PTL>>1]=v; s_sprpt[1] = _bplptr(SPR1PTH, SPR1PTL); break;
        case SPR1PTH: s_regs[SPR1PTH>>1]=v; s_sprpt[1] = _bplptr(SPR1PTH, SPR1PTL); break;
        case SPR2PTL: s_regs[SPR2PTL>>1]=v; s_sprpt[2] = _bplptr(SPR2PTH, SPR2PTL); break;
        case SPR2PTH: s_regs[SPR2PTH>>1]=v; s_sprpt[2] = _bplptr(SPR2PTH, SPR2PTL); break;
        case SPR3PTL: s_regs[SPR3PTL>>1]=v; s_sprpt[3] = _bplptr(SPR3PTH, SPR3PTL); break;
        case SPR3PTH: s_regs[SPR3PTH>>1]=v; s_sprpt[3] = _bplptr(SPR3PTH, SPR3PTL); break;
        case SPR4PTL: s_regs[SPR4PTL>>1]=v; s_sprpt[4] = _bplptr(SPR4PTH, SPR4PTL); break;
        case SPR4PTH: s_regs[SPR4PTH>>1]=v; s_sprpt[4] = _bplptr(SPR4PTH, SPR4PTL); break;
        case SPR5PTL: s_regs[SPR5PTL>>1]=v; s_sprpt[5] = _bplptr(SPR5PTH, SPR5PTL); break;
        case SPR5PTH: s_regs[SPR5PTH>>1]=v; s_sprpt[5] = _bplptr(SPR5PTH, SPR5PTL); break;
        case SPR6PTL: s_regs[SPR6PTL>>1]=v; s_sprpt[6] = _bplptr(SPR6PTH, SPR6PTL); break;
        case SPR6PTH: s_regs[SPR6PTH>>1]=v; s_sprpt[6] = _bplptr(SPR6PTH, SPR6PTL); break;
        case SPR7PTL: s_regs[SPR7PTL>>1]=v; s_sprpt[7] = _bplptr(SPR7PTH, SPR7PTL); break;
        case SPR7PTH: s_regs[SPR7PTH>>1]=v; s_sprpt[7] = _bplptr(SPR7PTH, SPR7PTL); break;
        /* Blitter */
        case BLTSIZE:
            s_blt_bzero = 0;
            hw_do_blit();
            break;
        /* DIWSTRT / DIWSTOP / COPJMP1 */
        case DIWSTRT: s_diwstrt = v; break;
        case DIWSTOP: s_diwstop = v; break;
        case COPJMP1:
            /* Restart copper at COP1LC */
            break;
        /* Audio trigger */
        case AUD0DAT: hw_audio_trigger(0); break;
        case AUD1DAT: hw_audio_trigger(1); break;
        case AUD2DAT: hw_audio_trigger(2); break;
        case AUD3DAT: hw_audio_trigger(3); break;
        /* Audio registers (shadow) */
        case AUD0LCH: case AUD0LCL: case AUD0PER: case AUD0VOL:
        case AUD1LCH: case AUD1LCL: case AUD1PER: case AUD1VOL:
        case AUD2LCH: case AUD2LCL: case AUD2PER: case AUD2VOL:
        case AUD3LCH: case AUD3LCL: case AUD3PER: case AUD3VOL:
            s_regs[reg >> 1] = v;
            if (getenv("BENEFACTOR_AUD_TRACE")) {
                static FILE *_af = NULL; static int _tried = 0;
                if (!_tried) { _tried = 1; _af = fopen("logs/aud_trace.txt", "w"); }
                static const char *rn[] = {"LCH","LCL","LEN","PER","VOL"};
                int ch = (reg - AUD0LCH) / 0x10, rt = ((reg - AUD0LCH) % 0x10) >> 1;
                if (_af) { fprintf(_af, "[aud] f=%d ch%d %s=$%04X\n", hw_get_frame_num(), ch, rt < 5 ? rn[rt] : "?", v); fflush(_af); }
            }
            break;
        /* AUDxLEN: shadow + start DMA stream for the channel if idle. */
        case AUD0LEN: case AUD1LEN: case AUD2LEN: case AUD3LEN:
            s_regs[reg >> 1] = v;
            if (getenv("BENEFACTOR_AUD_TRACE"))
                fprintf(stderr, "[aud] ch%d LEN=$%04X\n", (reg - AUD0LCH) / 0x10, v);
            hw_audio_dma_kick((reg - AUD0LCH) / 0x10);
            break;
        /* Palette */
        default:
            if (reg >= COLOR00 && reg < COLOR00 + 64) {
                int idx = (reg - COLOR00) >> 1;
                if (idx < 32) {
                    s_palette[idx] = amiga_to_argb(v);
                    if (idx <= 3) { HWTRACE("[HWTRACE] COLOR%02d=$%03X\n", idx, (unsigned)v & 0xFFF); }
                }
            }
            break;
        }
        return;
    }
}

void hw_write32(uint32_t addr, uint32_t v)
{
    hw_write16(addr,   (uint16_t)(v >> 16));
    hw_write16(addr+2, (uint16_t)(v));
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Synthetic beam counter – advance one frame worth of scanlines               */
/* ─────────────────────────────────────────────────────────────────────────── */

void hw_advance_scanline(void)
{
    s_scanline++;
    if (s_scanline >= 312) {
        s_scanline = 0;
        s_frame_num++;
    }
    /* Tick CIA-B timer once per scanline */
    if ((s_ciab_cra & 1) && s_ciab_ta_cnt > 0) {
        s_ciab_ta_cnt--;
        if (s_ciab_ta_cnt == 0) {
            s_ciab_icr_data |= 0x01;
            if (s_ciab_cra & 0x08)   /* one-shot */
                s_ciab_cra &= ~1;
            else
                s_ciab_ta_cnt = s_ciab_ta_latch;
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Recompiler helpers – expose internal state for generated code                */
/* ─────────────────────────────────────────────────────────────────────────── */

uint8_t hw_vposr_read(void)
{
    /* Advance beam and possibly trigger frame boundary */
    s_vposr_counter++;
    if (s_vposr_counter >= 5000) {
        s_vposr_counter = 0;
        s_scanline = 0;
        s_frame_num++;
        if (hw_running && hw_present_frame() != 0) {
            hw_running = 0;
            s_scanline = 999;
        }
    }
    uint16_t lof = 1;
    uint16_t val = (uint16_t)((lof << 15) | (s_frame_num & 1));
    return (uint8_t)val;
}

int hw_blitter_bzero(void)
{
    int r = s_blt_bzero;
    s_blt_bzero = 0;  /* reading clears BZERO, like DMACONR */
    return r;
}

void hw_blitter_sync(void)
{
    /* In our implementation blits are instant, so BZERO is already 1.
     * If we ever make blits async, this would spin or block. */
    s_blt_bzero = 1;
}

void (*g_hw_vblank_yield)(void) = NULL;
int g_hw_pc_owns_present = 0;

/* Called when the game commits the display copper pointer (writes COP1LC) — the
 * moment a new frame becomes visible on real hardware. The disk-boot coroutine
 * uses this to present at the true display-swap point, so the double-buffered
 * scroll is sampled at a consistent phase (no temporal judder). */
void (*g_hw_cop1lc_present)(void) = NULL;

/* Current copper-list-1 location (last value written to COP1LC $DFF080/82). */
uint32_t hw_get_cop1lc(void)
{
    return ((uint32_t)s_regs[0x080 >> 1] << 16) | s_regs[0x082 >> 1];
}

void hw_vblank_wait(void)
{
    /* Disk-boot coroutine mode: this is the per-frame yield point — hand control
     * back to the frame driver (render + input + IRQs), then resume the game.
     * Otherwise a no-op (the snapshot path drives frames from pc.c). */
    if (g_hw_vblank_yield) g_hw_vblank_yield();
}

void hw_wait_fire(int want_pressed)
{
    /* Folded `tst.b $bfe001; b(mi|pl) self` busy-loop. On real OCS the loop is
     * fine (CPU spins until the joystick fire bit flips); under our coroutine
     * model the spin never yields, so input never updates and the watchdog
     * fires. Yield a frame per check so the input layer can flip s_fire_pressed
     * (either via the keyboard mapper or the harness REPL). */
    if (want_pressed) {
        while (!s_fire_pressed && !s_mouse_lmb && hw_running) hw_vblank_wait();
    } else {
        while ((s_fire_pressed || s_mouse_lmb) && hw_running) hw_vblank_wait();
    }
}

void hw_vsync(void)
{
    /* Wait for next frame boundary */
    int start_frame = s_frame_num;
    while (s_frame_num == start_frame && hw_running) {
        s_vposr_counter++;
        if (s_vposr_counter >= 5000) {
            s_vposr_counter = 0;
            s_scanline = 0;
            s_frame_num++;
            if (hw_present_frame() != 0) {
                hw_running = 0;
                s_scanline = 999;
            }
        }
    }
}
