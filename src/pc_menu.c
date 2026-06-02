/* pc_menu.c — faithful native port of the title main menu (gfn_gp_003872).
 *
 * GOAL (per the faithful-port-then-enhance rule): reproduce the recompiled
 * menu's behavior EXACTLY in clean, hand-written C — same display setup, same
 * game-font option draws, same cursor highlight, same per-frame animation,
 * same input handling and idle-to-attract timeout — verified byte-identical
 * against the recompiled gfn_gp_003872. Only once that matches do we layer
 * changes (LEVEL SELECT, removing the password item, extra options) on top.
 *
 * This is NOT a render-overlay or a string-poke mimic. It walks the engine's
 * real state (a5-relative vars, a6=$DFF000 custom regs) and calls the engine's
 * own shared sub-routines (decompress $3700, glyph blit $0049B6, draw $7C22/
 * $7CA8/$4A00, input read $3BAA) via rt_call — exactly as the original does.
 *
 * Menu state (a5 base, a5=$511E at runtime):
 *   -$18be(a5)  cursor (0=PLAY GAME, 1=ENTER PASSWORD, 2=LOAD EXTRA LEVELS)
 *   -$15b6(a5)  password-entry sub-state counter
 *   -$13b4(a5)  current sprite/copper list write ptr (-$13b0 = list base)
 *    $2be2(a5)  menu-music countdown; while !=0 the menu stays, else -> attract
 *    $2be8(a5)  3-word option colour array (default $0041, selected $0fff)
 *   -$18c0(a5)  frame-indexed draw-table base / scratch input store
 *   The three menu items are structs {uint32 dest_bplptr; char text[]} at
 *   -$6a8 / -$69a / -$686 (a5); text inline after the 4-byte dest pointer.
 *
 * A/B verification: g_native_menu_enable (default 1). Set 0 (env PC_NATIVE_MENU=0)
 * to super-call the recompiled gfn_gp_003872 instead, so we can diff native vs
 * recompiled framebuffer / chip-RAM and confirm they match before trusting it.
 */
#include "pc_internal.h"
#include "generated/game.h"

extern void hw_blitter_sync(void);
extern void hw_vblank_wait(void);

/* Keep the recomp body diffable / A/B-toggleable (recomp-overrides rule). */
int g_native_menu_enable = 1;

/* Faithful native port of gfn_gp_003872 ($003872): menu display setup + the
 * per-frame input loop. On fire it exits via $0039D0 (the dispatch — still the
 * existing handler for now); on the music countdown reaching 0 it returns to
 * the attract/poster at $0033E2, exactly like the original. */
void pc_native_main_menu(M68KCtx *ctx)
{
    if (!g_native_menu_enable) { gfn_gp_003872(ctx); return; }

    const uint32_t a5 = ctx->A[5];
    const uint32_t a6 = ctx->A[6];

    /* ── Display setup ($003872–$00395C) ─────────────────────────────────── */
    MW16(a5 + 0x2be2u, 0x384);              /* menu-music countdown */
    ctx->A[0] = 0x1339au;
    ctx->A[1] = 0x49000u;
    rt_call(ctx, 0x003700u);                /* decompress menu gfx -> $49000 */

    hw_blitter_sync();
    MW32(a6 + 0x3eu, 0x9f00000u);           /* display/blit pointers */
    MW32(a6 + 0x42u, 0xffffffffu);
    MW32(a6 + 0x62u, 0);
    MW32(a6 + 0x4eu, 0x49000u);
    MW32(a6 + 0x52u, 0x59000u);
    MW16(a6 + 0x56u, 0x8032);
    MW16(a5 - 0x18beu, 1);                  /* cursor (reset to 0 below) */
    MW16(a5 - 0x15b6u, 0);
    MW32(a5 - 0x13b4u, 0x3d6eu);            /* sprite/copper list write ptr */
    MW32(0x963eu, 0x8ea89000u);

    /* $0038D4: build the menu sprite list — 10× $3DAA, advancing the list ptr. */
    hw_blitter_sync();
    for (int i = 0; i < 10; i++) {          /* dbra d6 from 9 => 10 iterations */
        MW16(a5 - 0x15b6u, (uint16_t)(MR16(a5 - 0x15b6u) + 1));
        rt_call(ctx, a5 - 0x1374u);         /* $003DAA */
        MW32(a5 - 0x13b4u, MR32(a5 - 0x13b4u) + 4u);
    }
    MW16(a5 - 0x15b6u, 0);
    MW16(a5 - 0x18beu, 0);                  /* cursor = PLAY GAME */

    /* $0038F4: fill the 5 menu bitplane pointers at $83EC ($49000 + i*$28). */
    {
        uint32_t d0 = 0x49000u, a0 = 0x83ecu;
        for (int i = 0; i < 5; i++) {       /* dbra d7 from 4 => 5 */
            MW16(a0 + 4u, (uint16_t)(d0 & 0xffffu));
            MW16(a0,      (uint16_t)((d0 >> 16) & 0xffffu));
            d0 += 0x28u;
            a0 += 8u;
        }
    }

    /* $003918: draw the three option rows via the glyph blitter ($0049B6).
     * Each item is {uint32 dest_bplptr; char text[]}; the blitter takes the
     * dest in a1 and the string in a2, shadow offset in d6. */
    ctx->D[6] = (ctx->D[6] & 0xffff0000u) | 0x641u;   /* shadow offset */
    {
        static const uint32_t item_off[3] = { 0x6a8u, 0x69au, 0x686u };
        for (int i = 0; i < 3; i++) {
            uint32_t item = a5 - item_off[i];
            ctx->A[1] = MR32(item);         /* dest bitplane ptr */
            ctx->A[2] = item + 4u;          /* inline string */
            rt_call(ctx, a5 - 0x768u);      /* $0049B6 glyph blit */
        }
    }

    /* $00393A: select frame-indexed draw buffer, set menu copper + INTENA. */
    {
        uint32_t a2 = a5 - 0x18c0u;
        uint32_t d0 = (uint32_t)(7u & MR16(0x1eu)) << 2;
        ctx->A[2] = MR32(a2 + d0);
    }
    MW32(a6 + 0x7eu, 0x8302u);              /* menu copper list (cop1lc) */
    MW16(a6 + 0x94u, 0x83f0u);              /* INTENA */
    ctx->A[4] = a5 + 0x2cb4u;
    rt_call(ctx, a5 + 0x2b04u);            /* $007C22 initial draw */

    /* ── Per-frame input loop ($003960–$0039CC) ──────────────────────────── */
    for (;;) {
        hw_vblank_wait();                   /* $003960 */

        /* $003968: option colour array — defaults $0041, selected row $0fff. */
        {
            uint32_t a3 = a5 + 0x2be8u;
            MW32(a3, 0x410041u);
            MW16(a3 + 4u, 0x41);
            uint32_t d0 = (uint32_t)MR16(a5 - 0x18beu);   /* cursor */
            d0 = (uint16_t)(d0 + d0);                      /* *2 (word index) */
            MW16(a3 + d0, 0xfff);
        }

        /* $00398C: cursor-highlight draw via $007CA8 (params from $2d3c table). */
        {
            uint32_t a3 = a5 + 0x2d3cu;
            ctx->A[0] = MR32(a3);     a3 += 4u;
            ctx->A[1] = MR32(a3);     a3 += 4u;
            ctx->D[6] = RT_SX16(MR16(a3));
            ctx->D[7] = RT_SX16(MR16(a3 + 2u));
            rt_call(ctx, a5 + 0x2b8au);     /* $007CA8 */
        }

        /* $00399C: per-frame animated draw ($004A00), frame-indexed buffer. */
        {
            uint32_t a2 = a5 - 0x18c0u;
            uint32_t d0 = (uint32_t)(7u & MR16(0x1eu)) << 2;
            ctx->A[2] = MR32(a2 + d0);
            rt_call(ctx, a5 - 0x71eu);      /* $004A00 */
        }

        /* $0039B0: read input -> d0, stash it, test fire (bit 5). */
        rt_call(ctx, a5 - 0x1574u);         /* $003BAA */
        uint32_t in = ctx->D[0];
        MW16(a5 - 0x18c0u, in);
        if (in & 0x20u) {                   /* btst #5 -> fire */
            rt_jump(ctx, 0x0039D0u);        /* dispatch (PLAY/PASSWORD/EXTRA) */
            return;
        }

        /* $0039BE: while the menu music is still running, keep looping;
         * once it finishes, return to the attract/poster at $0033E2. */
        if (MR16(a5 + 0x2be2u) != 0) continue;
        ctx->A[4] = a5 + 0x2c18u;
        rt_call(ctx, a5 + 0x2b04u);         /* $007C22 */
        rt_jump(ctx, a5 - 0x1d3cu);         /* $0033E2 attract */
        return;
    }
}

/* Faithful native port of gfn_gp_0039D0 ($0039D0): the menu fire dispatch.
 * cursor 0 = PLAY GAME (decode entered password -> level, start game at $150);
 * cursor 1 = ENTER PASSWORD (return to the loop so keys are read);
 * cursor 2 = LOAD EXTRA LEVELS (rebuild the menu list, reload). Returns to the
 * loop via $0039BE and reloads the menu via $003872, exactly like the original.
 * This REPLACES the old native_main_menu_fire_dispatch (the cursor-1->level-
 * select hijack); LEVEL SELECT is re-added as a proper change on this faithful
 * base afterward. */
void pc_native_menu_dispatch(M68KCtx *ctx)
{
    if (!g_native_menu_enable) { gfn_gp_0039D0(ctx); return; }

    const uint32_t a5 = ctx->A[5];
    const uint32_t a6 = ctx->A[6];

    uint16_t cursor = (uint16_t)MR16(a5 - 0x18beu);
    if (cursor == 2) goto load_extra;                       /* $0039D6 */
    if (cursor == 1) MW16(a5 - 0x15b6u, 0);                 /* $0039E2 */
    if (MR16(a5 - 0x15b6u) != 0) { rt_jump(ctx, 0x0039BEu); return; }  /* $0039EA */
    if (MR16(a5 - 0x18beu) != 0) { rt_jump(ctx, 0x0039BEu); return; }  /* $0039F0 */

    /* $0039F2 — PLAY GAME: redraw, run the password->level decoder, clamp. */
    ctx->A[4] = a5 + 0x2c18u;
    rt_call(ctx, a5 + 0x2b04u);             /* $007C22 */
    rt_call(ctx, a5 - 0xb22u);              /* $0045FC password/level decode */
    MW16(0x1cu, 0xbf);
    MW16(0x20u, 1);
    { uint8_t b = MR8(a5 - 0x970u); MW8(0x1du, b); if (b == 0) MW16(0x1cu, 0xbf); }   /* $003A0A */
    { uint8_t b = MR8(a5 - 0x96fu); MW8(0x21u, b); if (b == 0) MW16(0x20u, 1); }      /* $003A18 */
    if (MR16(0x1cu) > 0xbfu) {              /* $003A26 (not bls) */
        MW8(a5 - 0x96fu, 0); MW16(0x1cu, 0xbf); MW16(0x20u, 1);
        goto after_clamp;
    }
    if (MR16(0x20u) > 0x3cu) {              /* $003A40 (not bls) */
        if (MR8(0x38u) != 0) goto chk_5a;   /* $003A4C */
        MW8(a5 - 0x96fu, 0); MW16(0x20u, 1);
        goto after_clamp;
    }
    goto after_clamp;
chk_5a:                                      /* $003A5A */
    if (MR16(0x20u) > 0x5au) { MW8(a5 - 0x96fu, 0); MW16(0x20u, 1); }
after_clamp:                                 /* $003A6C */
    MW16(0x2cu, MR16(0x1cu));
    if (MR8(a5 - 0x96fu) != 0) goto start_game;     /* $003A76 */

    /* $003A78 — password incomplete: flash the entry, wait, rebuild list, reload. */
    MW32(a6 + 0x7eu, 0x95c2u);
    { uint32_t d0 = 0x2e23eu; MW16(0x9638u, (uint16_t)(d0 & 0xffffu)); MW16(0x9634u, (uint16_t)((d0 >> 16) & 0xffffu)); }
    for (int i = 0; i < 0x97; i++) hw_vblank_wait();        /* $003A98 d7=$96 */
    { uint32_t a0 = a5 - 0x13b0u;
      MW32(a5 - 0x13b4u, a0);
      static const uint32_t lst[10] = { 0x3d64u,0x3d56u,0x3d59u,0x3d55u,0x3d51u,
                                        0x3d58u,0x3d59u,0x3d55u,0x3d51u,0x3d58u };
      for (int i = 0; i < 10; i++) { MW32(a0, lst[i]); a0 += 4u; } }
    rt_jump(ctx, 0x003872u);                /* reload menu */
    return;

start_game:                                  /* $003AF4 */
    MW16(a6 + 0x94u, 0x7fff);
    MW16(a6 + 0x98u, 0x7fff);
    ctx->A[7] = 0x80000u;
    ctx->D[0] = 0;
    rt_jump(ctx, 0x150u);                    /* start the selected level */
    return;

load_extra:                                  /* $003B0C */
    ctx->A[4] = a5 + 0x2c18u;
    rt_call(ctx, a5 + 0x2b04u);             /* $007C22 */
    MW32(a6 + 0x7eu, 0x8182u);
    MW8(0x38u, 0xff);
    { uint32_t a0 = a5 - 0x13b0u;
      MW32(a5 - 0x13b4u, a0);
      static const uint32_t lst[10] = { 0x3d56u,0x3d57u,0x3d59u,0x3d57u,0x3d64u,
                                        0x3d65u,0x3d5eu,0x3d5cu,0x3d65u,0x3d66u };
      for (int i = 0; i < 10; i++) { MW32(a0, lst[i]); a0 += 4u; } }
    rt_jump(ctx, 0x003872u);                /* reload menu */
    return;
}
