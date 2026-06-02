/* game_state.h — Single source of truth for all state that constitutes a
 * savestate. Everything that must round-trip through pc_savestate /
 * pc_loadstate lives here (the M68K register file, the game coroutine, the
 * Amiga custom-chip register shadows, the audio channels, and the bank-
 * routing / state-machine flags). Files keep using the legacy names
 * (s_regs, s_dmacon, g_overlay_active, …) through the #defines below — those
 * resolve to fields of the single g_state instance.
 *
 * Save/load is `fwrite(&g_state, sizeof g_state, 1, f)` and the matching read.
 * No field is duplicated outside this struct. */
#pragma once

#include <stdint.h>
#include "recomp/rt.h"   /* M68KCtx */

/* Audio channel state — mirrored from the live Paula register shadows. Lives
 * inside GameState so it round-trips with everything else. */
typedef struct AudioChannel {
    uint32_t ptr;      /* current play position (chip RAM address) */
    uint32_t start;    /* start of audio data (from LCH/LCL) */
    uint16_t len;      /* length in words */
    uint16_t period;   /* period value (~pitch) */
    uint8_t  vol;      /* volume 0-64 */
    uint8_t  active;   /* 1 = playing */
    int      pos;      /* current byte index */
    int64_t  tick;     /* sub-sample accumulator (Paula clock ticks, exact) */
} AudioChannel;

typedef struct GameState {
    /* ── M68K register file (the game thread's CPU state) ────────────────── */
    M68KCtx    game_ctx;

    int        game_done;        /* set when the game flow returns from rt_call */
    int        coro_quit;        /* (legacy) host-requested quit */

    /* ── Amiga custom-chip register shadows ──────────────────────────────── */
    uint16_t regs[512];           /* indexed by (DFF offset >> 1) */
    uint16_t dmacon, intena, intreq;
    uint16_t bplcon0;
    uint32_t bplptr[6];
    uint32_t sprpt[8];
    uint32_t palette[32];         /* ARGB8888, derived from COLOR regs */
    uint16_t diwstrt;
    uint16_t diwstop;
    int      blt_bzero;
    int      vposr_counter;

    /* ── CIA-B timer (music tick rate) ───────────────────────────────────── */
    uint16_t ciab_ta_latch;
    uint16_t ciab_ta_cnt;
    uint8_t  ciab_cra;
    uint8_t  ciab_icr_data;
    uint8_t  ciab_icr_mask;

    /* ── Paula audio channel state ───────────────────────────────────────── */
    AudioChannel audio[4];

    /* ── Runtime bank routing (rt.c uses these to pick credits vs gpl vs
     *    gp vs intro) ─────────────────────────────────────────────────── */
    int overlay_active;
    int gameplay_active;
    int credits_active;

    /* ── Game-state machine (cross-coroutine restart flags) ──────────────── */
    int      enter_gameplay;
    uint32_t gameplay_entry;
    int      pc_start_level;
} GameState;

extern GameState g_state;

/* ── Legacy-name macro aliases ─────────────────────────────────────────────
 *
 * Existing call sites use the original variable names (s_regs, s_dmacon,
 * g_overlay_active, ...); these macros redirect them to the corresponding
 * fields of the single g_state instance. Because the macros expand to struct
 * member access, any stray `extern int g_overlay_active;` declaration is now
 * a syntax error — those have been removed from call sites and replaced by
 * `#include "game_state.h"`. */

#define s_regs                    (g_state.regs)
#define s_dmacon                  (g_state.dmacon)
#define s_intena                  (g_state.intena)
#define s_intreq                  (g_state.intreq)
#define s_bplcon0                 (g_state.bplcon0)
#define s_bplptr                  (g_state.bplptr)
#define s_sprpt                   (g_state.sprpt)
#define s_palette                 (g_state.palette)
#define s_diwstrt                 (g_state.diwstrt)
#define s_diwstop                 (g_state.diwstop)
#define s_blt_bzero               (g_state.blt_bzero)
#define s_vposr_counter           (g_state.vposr_counter)
#define s_ciab_ta_latch           (g_state.ciab_ta_latch)
#define s_ciab_ta_cnt             (g_state.ciab_ta_cnt)
#define s_ciab_cra                (g_state.ciab_cra)
#define s_ciab_icr_data           (g_state.ciab_icr_data)
#define s_ciab_icr_mask           (g_state.ciab_icr_mask)
#define s_audio                   (g_state.audio)

#define g_overlay_active          (g_state.overlay_active)
#define g_gameplay_active         (g_state.gameplay_active)
#define g_credits_active          (g_state.credits_active)
#define g_enter_gameplay          (g_state.enter_gameplay)
#define g_gameplay_entry          (g_state.gameplay_entry)
#define g_pc_start_level          (g_state.pc_start_level)

/* Game-thread state (only referenced inside pc.c) — legacy short names. */
#define s_game_ctx                (g_state.game_ctx)
#define s_game_done               (g_state.game_done)
#define s_coro_quit               (g_state.coro_quit)
