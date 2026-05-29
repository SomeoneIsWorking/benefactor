/*
 * engine.c  –  Native PC game engine (boot + main loop)
 *
 * Replaces the entire Amiga dispatch-table / copper-list / blitter-wait
 * architecture with native C game states.
 */

#include "engine.h"
#include "recomp/rt.h"
#include "recomp/hw.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>

/* ── Helpers ──────────────────────────────────────────────────────────────── */

static inline uint8_t  read_u8 (uint32_t a) { return g_mem[a]; }
static inline uint16_t read_u16(uint32_t a) { return (g_mem[a]<<8)|g_mem[a+1]; }
static inline uint32_t read_u32(uint32_t a) { return ((uint32_t)g_mem[a]<<24)|((uint32_t)g_mem[a+1]<<16)|((uint32_t)g_mem[a+2]<<8)|g_mem[a+3]; }
static inline void write_u16(uint32_t a, uint16_t v) { g_mem[a]=v>>8; g_mem[a+1]=v; }
static inline void write_u32(uint32_t a, uint32_t v) { g_mem[a]=v>>24; g_mem[a+1]=v>>16; g_mem[a+2]=v>>8; g_mem[a+3]=v; }

/* ── Init ─────────────────────────────────────────────────────────────────── */

void engine_run(M68KCtx *ctx)
{
    fprintf(stderr, "[engine] boot start\n");

    ctx->A[5] = 0x00531cu;
    ctx->A[6] = 0x00dff002u;
    ctx->A[7] = 0x0080000u;

    memset(g_mem, 0, 8);
    write_u32(0x00006cu, 0x0000314eu);
    write_u32(0x000078u, 0x00003160u);
    write_u32(ctx->A[5] + 0x14aau, 0x00061fbcu);
    write_u32(ctx->A[5] + 0x16ccu, 0x00003160u);

    /* WHDLoad init */
    rt_call(ctx, 0x00531Cu);
    ctx->A[6] = 0x00dff002u;

    /* Enable DMA + copper list (title screen list at $7770) */
    hw_write16(0xdff094u, 0x83c0u);
    hw_write32(0xdff080u, 0x00007770u);
    hw_write16(0xdff098u, 0xe020u);

    /* Write dispatch word area (used by some functions) */
    write_u16(ctx->A[5] + 0x2214u, 0);

    fprintf(stderr, "[engine] entering game loop\n");

    /* ── Native game state machine ──────────────────────────────────────── */

    enum { STATE_TITLE, STATE_INIT, STATE_GAME, STATE_PAUSE, STATE_RESET };
    int state = STATE_TITLE;

    while (hw_running) {
        /* Poll input once per frame */
        uint8_t ciaa = hw_read8(0xbfe001u);
        int lmb_pressed = ((ciaa & 0x80) == 0);

        switch (state) {

        case STATE_TITLE: {
            /* Title screen – copper list already set by dispatch.
             * Flip the toggle flag, call sprite/blitter setup, render. */
            uint16_t flag = read_u16(ctx->A[5] + 0x117au);
            write_u16(ctx->A[5] + 0x117au, ~flag);

            /* Use recompiled helpers for sprite/copper setup */
            rt_call(ctx, 0x0041A4u);
            rt_call(ctx, 0x00405Cu);
            /* Call timer interrupt handler (preserve CCR across it) */
            {
                uint8_t n=ctx->N,z=ctx->Z,v=ctx->V,c=ctx->C;
                rt_call(ctx, 0x0055A0u);
                ctx->N=n;ctx->Z=z;ctx->V=v;ctx->C=c;
            }

            if (hw_present_frame() != 0) { hw_running = 0; break; }

            if (lmb_pressed) {
                state = STATE_INIT;
                fprintf(stderr, "[engine] title -> init\n");
            }
            break;
        }

        case STATE_INIT: {
            /* Transition: run the 125-frame and 230-frame init sequences.
             * Sets up copper lists, calls $74AA, then advances to game. */
            for (int phase = 0; phase < 2; phase++) {
                uint32_t cop = (phase == 0) ? 0x0078f0u : 0x0077c0u;
                hw_write32(0xdff080u, cop);

                /* Call $74AA (animation/data setup) via recompiled helper */
                rt_call(ctx, ctx->A[5] + 0x218eu);
            }

            /* Set game copper list */
            hw_write32(0xdff080u, 0x0091d0u);
            state = STATE_GAME;
            fprintf(stderr, "[engine] init -> game\n");
            break;
        }

        case STATE_GAME: {
            /* Game frame:
             * 1. Call $31A0 (blitter wait + setup)
             * 2. Do game rendering blits
             * 3. Update sprite table
             * 4. Render frame
             * 5. Check input / timer for state transitions */

            /* Blitter clear (from $31A0) – triggers a blit via BLTSIZE */
            rt_call(ctx, 0x0031A0u);

            /* The recompiled $3488 does the game blits and VPOSR sync. */
            rt_call(ctx, 0x003488u);

            if (hw_present_frame() != 0) { hw_running = 0; break; }

            /* After game renders, check for input to advance or exit */
            static int game_frames = 0;
            game_frames++;

            if (lmb_pressed || game_frames > 300) {
                state = STATE_PAUSE;
                game_frames = 0;
                fprintf(stderr, "[engine] game -> pause\n");
            }
            break;
        }

        case STATE_PAUSE: {
            /* Brief pause (50 frames equivalent) */
            hw_write32(0xdff080u, 0x007770u);
            if (hw_present_frame() != 0) { hw_running = 0; break; }
            state = STATE_RESET;
            fprintf(stderr, "[engine] pause -> reset\n");
            break;
        }

        case STATE_RESET: {
            /* Reset back to title screen */
            hw_write32(0xdff080u, 0x00007770u);
            write_u16(ctx->A[5] + 0x117au, 0);
            state = STATE_TITLE;
            fprintf(stderr, "[engine] reset -> title\n");
            break;
        }

        default: state = STATE_TITLE; break;
        }

        /* hw_present_frame() already handles 50 Hz pacing; this is a
         * fallback delay in case the frame was skipped without presenting. */
        SDL_Delay(20);
    }

    fprintf(stderr, "[engine] game loop exited\n");
}

/* ── Override registration (minimal – only for init & helpers) ─────────────── */

void engine_install_overrides(void)
{
    /* Map game entry point $3000 to the native engine loop. */
    rt_register_override(0x003000u, engine_run);
    fprintf(stderr, "[engine] overrides installed\n");
}
