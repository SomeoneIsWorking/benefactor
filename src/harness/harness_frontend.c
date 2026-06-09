/* harness/harness_frontend.c
 *
 * Libretro "frontend" for benefactor-harness.
 * Provides PUAE boot + interactive side-by-side display of PUAE vs PC port.
 *
 * Phases:
 *   1. Fast-forward boot: run PUAE silently (no rendering) until game state.
 *   2. Interactive: run PUAE + PC frame-locked, side-by-side, with input.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <unistd.h>

/* Prevent libretro VFS from redefining FILE/fprintf/etc. — we use real stdio here */
#define SKIP_STDIO_REDEFINES

#include "libretro.h"
#include "libretro-core.h"
#include "engine/hw.h"
#include "harness/input.h"
#include "harness/harness_internal.h"

#ifdef SDL_DISPLAY
#include <SDL2/SDL.h>
static SDL_Window   *s_sdl_win     = NULL;
static SDL_Renderer *s_sdl_ren     = NULL;
static SDL_Texture  *s_sdl_tex     = NULL;
static int           s_sdl_win_w   = 0;
static int           s_sdl_win_h   = 0;
static enum retro_pixel_format s_pixel_fmt = RETRO_PIXEL_FORMAT_RGB565;

/* PUAE frame buffer (copied from video_cb, RGB565 or XRGB8888) */
static int      s_puae_w = 0, s_puae_h = 0;
static int      s_puae_pitch = 0;   /* bytes per row */
static int      s_puae_bpp = 2;     /* bytes per pixel (RGB565=2, XRGB8888=4) */

/* Fast-forward flag: skip rendering during boot */
int g_harness_fast_forward = 0;

/* Combined display functions (declared for harness_main) */
#ifdef SDL_DISPLAY
void harness_combined_init(void);
void harness_combined_present(void);
void harness_combined_fini(void);
#endif
#endif

/* ── paths set by harness_main before calling retro_init ── */
char harness_system_dir[RETRO_PATH_MAX] = "";
char harness_save_dir  [RETRO_PATH_MAX] = "/tmp";

/* ── Variable table ── */
typedef struct { const char *key; const char *value; } VarEntry;
static const VarEntry s_vars[] = {
    { "puae_model",                  "A1200"         },
    { "puae_model_fd",               "A500"          },
    { "puae_model_hd",               "A1200"         },
    { "puae_model_cd",               "CD32"          },
    { "puae_kickstart",              "auto"           },
    { "puae_chipmem_size",           "2MB"            },
    { "puae_bogomem_size",           "none"           },
    { "puae_fastmem_size",           "4MB"            },
    { "puae_cpu_model",              "68020"          },
    { "puae_cpu_multiplier",         "0"              },
    { "puae_cpu_throttle",           "0.0"            },
    { "puae_cpu_compatibility",      "normal"         },
    { "puae_fpu_model",              "none"           },
    { "puae_immediate_blits",        "false"          },
    { "puae_collision_level",        "sprites"        },
    { "puae_gfx_framerate",          "0"              },
    { "puae_gfx_colors",             "16bit"          },
    { "puae_floppy_speed",           "100"            },
    { "puae_floppy_multidrive",      "disabled"       },
    { "puae_floppy_sound",           "disabled"       },
    { "puae_floppy_sound_empty_mute","disabled"       },
    { "puae_floppy_write_protection","disabled"       },
    { "puae_floppy_write_redirect",  "disabled"       },
    { "puae_statusbar",              "disabled"       },
    { "puae_use_whdload_buttonwait",  "disabled"      },
    { NULL, NULL }
};

/* ── PUAE video callback ── */
static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
    static int s_logged_geom = 0;
    static int s_crop_inited = 0;
    static int s_crop_x = 0, s_crop_y = 0, s_crop_w = 0, s_crop_h = 0;
    if (!data) return;

    /* Skip slow framebuffer copy during fast-forward boot phase */
    if (g_harness_fast_forward) {
#ifndef SDL_DISPLAY
        return;
#endif
    }

    s_puae_w = (int)w;
    s_puae_h = (int)h;
    s_puae_pitch = (int)pitch;
    s_puae_bpp = (s_pixel_fmt == RETRO_PIXEL_FORMAT_XRGB8888) ? 4 : 2;
    if (!s_logged_geom) {
        char buf[128];
        int n;
        s_logged_geom = 1;
        n = snprintf(buf, sizeof(buf), "[PUAE_FB_GEOM] w=%u h=%u pitch=%zu bpp=%d\n",
                     w, h, pitch, s_puae_bpp);
        if (n > 0)
            write(2, buf, (size_t)n);
    }
    if (!s_crop_inited) {
        const char *e;
        s_crop_inited = 1;
        s_crop_w = (int)w;
        /* Default crop_y=2, crop_h=512: maps output row y → PUAE line 2+2y.
         * Each PAL display line (doubled in PUAE's 2× output) maps to exactly
         * one PC output row — PAL line 44 → y=17, PAL line 89 → y=62.
         * Override via BENEFACTOR_PUAE_CROP_{Y,H} env vars. */
        s_crop_y = 2;
        s_crop_h = 564;   /* 512 -> 564: ~10% more PUAE source lines (extend down,
                           * matching the taller HW_DISPLAY_H=282 so the bottom of
                           * the playfield/HUD isn't cut). 2 src lines per output. */
        /* crop_x=100, crop_w=640: src_x = 100 + 2x. PUAE's captured frame is
         * ~35 PC-pixels right of centre vs the native renderer, so shift the
         * crop window right by 70 source px (= 35 output px) to align them.
         * Override via BENEFACTOR_PUAE_CROP_X. */
        /* PUAE's native frame is 720 wide; the playfield is right-aligned in it
         * (content ~src[106..716]) with a wide left border. To fill the taller/
         * wider output (352) at an undistorted 2 src-cols-per-output-px scale we
         * need 704 source cols, which only fits at crop_x<=16 (16+704=720). So
         * keep the original left-aligned anchor (100) so the playfield hugs the
         * left like the PC render, and take the full 704 at 2:1 — source columns
         * past the 720-wide PUAE frame are filled with black border in the copy
         * loop below (NOT clamped, which would stretch the image). */
        s_crop_x = 100;
        s_crop_w = 704;
        e = getenv("BENEFACTOR_PUAE_CROP_X"); if (e && *e) s_crop_x = atoi(e);
        e = getenv("BENEFACTOR_PUAE_CROP_Y"); if (e && *e) s_crop_y = atoi(e);
        e = getenv("BENEFACTOR_PUAE_CROP_W"); if (e && *e) s_crop_w = atoi(e);
        e = getenv("BENEFACTOR_PUAE_CROP_H"); if (e && *e) s_crop_h = atoi(e);
        if (s_crop_x < 0) s_crop_x = 0;
        if (s_crop_y < 0) s_crop_y = 0;
        if (s_crop_w < 1) s_crop_w = (int)w;
        if (s_crop_h < 1) s_crop_h = (int)h;
        /* NOTE: deliberately do NOT clamp crop_w/crop_h to the frame here — the
         * copy loop renders source pixels beyond the frame edge as black border,
         * preserving the 2:1 scale instead of stretching a clamped crop. */
    }

    /* Always capture PUAE frame to s_puae_fb (used for headless pixel comparison).
     * Scale from PUAE's native resolution to 320x256 via nearest-neighbor. */
    if (!g_harness_fast_forward) {
        for (int y = 0; y < FB_H; y++) {
            int src_y = s_crop_y + (y * s_crop_h) / FB_H;
            const uint8_t *src_row = (const uint8_t *)data + src_y * pitch;
            for (int x = 0; x < FB_W; x++) {
                int src_x = s_crop_x + (x * s_crop_w) / FB_W;
                if (src_x >= (int)w || src_y >= (int)h) {   /* past frame edge → border */
                    s_puae_fb[y * FB_W + x] = 0xFF000000u;
                    continue;
                }
                if (s_puae_bpp == 4) {
                    s_puae_fb[y * FB_W + x] = ((const uint32_t *)src_row)[src_x];
                } else {
                    uint16_t rgb = ((const uint16_t *)src_row)[src_x];
                    uint8_t r = (rgb >> 11) & 0x1F; r = (r << 3) | (r >> 2);
                    uint8_t g = (rgb >> 5)  & 0x3F; g = (g << 2) | (g >> 4);
                    uint8_t b = (rgb)       & 0x1F; b = (b << 3) | (b >> 2);
                    s_puae_fb[y * FB_W + x] = (0xFFu << 24) | (r << 16) | (g << 8) | b;
                }
            }
        }
    }

#ifdef SDL_DISPLAY
    /* SDL display path (interactive mode only) — render PUAE output to window */
    (void)0; /* SDL display code managed separately via harness_combined_present */
#endif
}

/* Audio capture (env AUDIODUMP): PUAE PCM -> logs/audio_puae.raw (stereo int16
 * at PUAE's libretro rate). 'frames' is sample-frames (L+R pairs). */
static size_t audio_batch_cb(const int16_t *data, size_t frames)
{
    if (data && frames && getenv("AUDIODUMP") && !g_harness_fast_forward) {
        static FILE *s_apu = NULL; static int s_init = 0;
        if (!s_init) { s_apu = fopen("logs/audio_puae.raw", "wb"); s_init = 1; }
        if (s_apu) fwrite(data, sizeof(int16_t), frames * 2, s_apu);
    }
    return frames;
}
static void audio_cb(int16_t l, int16_t r)
{
    if (getenv("AUDIODUMP") && !g_harness_fast_forward) {
        static FILE *s_apu1 = NULL; static int s_init = 0;
        if (!s_init) { s_apu1 = fopen("logs/audio_puae_single.raw", "wb"); s_init = 1; }
        if (s_apu1) { int16_t lr[2] = { l, r }; fwrite(lr, sizeof(int16_t), 2, s_apu1); }
    }
}

/* ── PUAE input callbacks (read from shared input state) ── */
static void input_poll_cb(void) { input_poll(); }

static int16_t input_state_cb(unsigned port, unsigned dev, unsigned idx, unsigned id)
{
    (void)port; (void)dev; (void)idx;
    switch (id) {
    case RETRO_DEVICE_ID_JOYPAD_UP:     return input_up();
    case RETRO_DEVICE_ID_JOYPAD_DOWN:   return input_down();
    case RETRO_DEVICE_ID_JOYPAD_LEFT:   return input_left();
    case RETRO_DEVICE_ID_JOYPAD_RIGHT:  return input_right();
    case RETRO_DEVICE_ID_JOYPAD_B:      return input_fire();
    case RETRO_DEVICE_ID_JOYPAD_A:      return input_space();
    case RETRO_DEVICE_ID_MOUSE_LEFT:    return input_space();  /* port-0 (mouse) button */
    default: return 0;
    }
}

/* ── log callback (libretro calls this for log messages) ── */
static void harness_log_cb(enum retro_log_level level, const char *fmt, ...)
{
    (void)level;
    va_list ap;
    va_start(ap, fmt);
    char buf[4096];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    printf("%s", buf);
    va_end(ap);
}

/* ── environment callback ── */
static bool harness_environ_cb(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
        struct retro_log_callback *cb = (struct retro_log_callback *)data;
        cb->log = harness_log_cb;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = harness_system_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = harness_save_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *var = (struct retro_variable *)data;
        for (const VarEntry *e = s_vars; e->key; e++) {
            if (strcmp(e->key, var->key) == 0) {
                var->value = e->value;
                return true;
            }
        }
        var->value = NULL;
        return false;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = false;
        return true;
    case RETRO_ENVIRONMENT_GET_FASTFORWARDING:
        *(bool *)data = g_harness_fast_forward ? true : false;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        s_pixel_fmt = *(enum retro_pixel_format *)data;
        return true;
    case RETRO_ENVIRONMENT_GET_VFS_INTERFACE:
        return false;
    case RETRO_ENVIRONMENT_GET_PERF_INTERFACE:
        return false;
    case RETRO_ENVIRONMENT_GET_LED_INTERFACE:
        return false;
    case RETRO_ENVIRONMENT_GET_LANGUAGE:
        *(unsigned *)data = RETRO_LANGUAGE_ENGLISH;
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
    case RETRO_ENVIRONMENT_SET_MESSAGE:
    case RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE:
    case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
        return true;
    default:
        return false;
    }
}

/* ── Combined side-by-side display ── */
#ifdef SDL_DISPLAY
void harness_combined_init(void)
{
    if (s_sdl_win) return;  /* already initialized */
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("[sdl] SDL_Init error: %s\n", SDL_GetError());
        return;
    }
    int win_w = FB_W * 2;
    int win_h = FB_H;
    s_sdl_win = SDL_CreateWindow("PUAE vs PC – Benefactor",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w * 2, win_h * 2, SDL_WINDOW_RESIZABLE);
    if (!s_sdl_win) { printf("[sdl] CreateWindow: %s\n", SDL_GetError()); return; }
    /* NO_VSYNC=1 → don't sync present to the display refresh, so a headed run
     * goes as fast as the host CPU allows ("turbo") instead of ~60Hz. */
    Uint32 _rflags = SDL_RENDERER_ACCELERATED |
                     (getenv("NO_VSYNC") ? 0u : (Uint32)SDL_RENDERER_PRESENTVSYNC);
    s_sdl_ren = SDL_CreateRenderer(s_sdl_win, -1, _rflags);
    if (!s_sdl_ren) {
        s_sdl_ren = SDL_CreateRenderer(s_sdl_win, -1, SDL_RENDERER_SOFTWARE);
        if (!s_sdl_ren) { printf("[sdl] CreateRenderer: %s\n", SDL_GetError()); return; }
    }
    SDL_RenderSetLogicalSize(s_sdl_ren, win_w, win_h);
    s_sdl_tex = SDL_CreateTexture(s_sdl_ren,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, win_w, win_h);
    if (!s_sdl_tex) { printf("[sdl] CreateTexture: %s\n", SDL_GetError()); return; }
    s_sdl_win_w = win_w;
    s_sdl_win_h = win_h;
}

void harness_combined_present(void)
{
    if (!s_sdl_tex || !s_sdl_ren || !s_sdl_win) return;
    /* Pump the window's event queue so it actually maps/appears and stays
     * responsive (without this the WM never shows the window and marks it "not
     * responding"). Quit the whole harness if the window is closed. */
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) exit(0);
    }
    static uint32_t composite[FB_W * 2 * FB_H];
    const uint32_t *pc_fb = hw_get_framebuffer();
    if (!pc_fb) return;

    /* Show frame counter overlay */
    static int frame_count = 0;
    frame_count++;

    for (int y = 0; y < FB_H; y++) {
        for (int x = 0; x < FB_W; x++) {
            composite[y * FB_W * 2 + x] = s_puae_fb[y * FB_W + x];
            composite[y * FB_W * 2 + FB_W + x] = pc_fb[y * FB_W + x];
        }
    }
    /* Draw divider line */
    for (int y = 0; y < FB_H; y++)
        composite[y * FB_W * 2 + FB_W - 1] = 0xFFFFFFFFu;

    SDL_UpdateTexture(s_sdl_tex, NULL, composite, FB_W * 2 * 4);
    SDL_RenderClear(s_sdl_ren);
    SDL_RenderCopy(s_sdl_ren, s_sdl_tex, NULL, NULL);
    SDL_RenderPresent(s_sdl_ren);
}

/* PUAE-only present: show just PUAE's framebuffer (right half dimmed). */
void harness_puae_present(void)
{
    if (!s_sdl_tex || !s_sdl_ren || !s_sdl_win) return;
    static uint32_t composite[FB_W * 2 * FB_H];
    for (int y = 0; y < FB_H; y++)
        for (int x = 0; x < FB_W; x++) {
            composite[y * FB_W * 2 + x]        = s_puae_fb[y * FB_W + x];
            composite[y * FB_W * 2 + FB_W + x] = 0xFF101010u;
        }
    SDL_UpdateTexture(s_sdl_tex, NULL, composite, FB_W * 2 * 4);
    SDL_RenderClear(s_sdl_ren);
    SDL_RenderCopy(s_sdl_ren, s_sdl_tex, NULL, NULL);
    SDL_RenderPresent(s_sdl_ren);
}

void harness_combined_fini(void)
{
    if (s_sdl_tex)  SDL_DestroyTexture(s_sdl_tex);
    if (s_sdl_ren)  SDL_DestroyRenderer(s_sdl_ren);
    if (s_sdl_win)  SDL_DestroyWindow(s_sdl_win);
    s_sdl_tex = NULL; s_sdl_ren = NULL; s_sdl_win = NULL;
    SDL_Quit();
}

void harness_interactive_delay(int ms)
{
    if (ms > 0) SDL_Delay((uint32_t)ms);
}
#endif /* SDL_DISPLAY */

#ifndef SDL_DISPLAY
void harness_combined_init(void) {}
void harness_combined_present(void) {}
void harness_puae_present(void) {}
void harness_combined_fini(void) {}
void harness_interactive_delay(int ms) { (void)ms; }
#endif

/* ── Called by harness_main before retro_init ── */
void harness_frontend_init(void)
{
    retro_set_environment(harness_environ_cb);
    retro_set_video_refresh(video_cb);
    retro_set_audio_sample(audio_cb);
    retro_set_audio_sample_batch(audio_batch_cb);
    retro_set_input_poll(input_poll_cb);
    retro_set_input_state(input_state_cb);
}
