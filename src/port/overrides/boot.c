/* pc_overrides_boot.c — Boot-time animation overrides (native PC C, no M68K emulation) */
#include "port/port_internal.h"
#include "engine/overlay_load.h"   /* shared pure overlay loaders */

/* Step a single 12-bit Amiga color one nibble-step toward its target.
 * Each of the R/G/B 4-bit channels moves ±1 per call. */
static uint16_t color_step(uint16_t src, uint16_t dst)
{
    uint16_t sb = src & 0x00Fu, db = dst & 0x00Fu;
    uint16_t sg = src & 0x0F0u, dg = dst & 0x0F0u;
    uint16_t sr = src & 0xF00u, dr = dst & 0xF00u;

    if (dr != sr) dr = (dr > sr) ? (uint16_t)(dr - 0x100u) : (uint16_t)(dr + 0x100u);
    if (dg != sg) dg = (dg > sg) ? (uint16_t)(dg - 0x010u) : (uint16_t)(dg + 0x010u);
    if (db != sb) db = (db > sb) ? (uint16_t)(db - 0x001u) : (uint16_t)(db + 0x001u);

    return (uint16_t)(dr | dg | db);
}

/* Native replacement for $0074DC (gfn_boot_animation_step).
 * Processes count+1 color entries:
 *   - reads source color from src (advances by 2 each entry)
 *   - reads+writes dest color at dst (advances by stride each entry)
 *   - steps each R/G/B nibble 1 step toward source */
static void anim_step_colors(uint32_t src, uint32_t dst, int16_t stride, int16_t count)
{
    for (int16_t i = count; i >= 0; i--) {
        uint16_t s = r16(src); src += 2;
        w16(dst, color_step(s, r16(dst)));
        dst = (uint32_t)((int32_t)dst + (int32_t)stride);
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * $0074AA — boot-animation / palette-fade iterator (native C replacement).
 *
 * Faithful reimplementation of the M68K routine at $74AA, which the recompiled
 * screen handlers ($3218/$31C2/$366A) call ONCE per fade and expect to BLOCK
 * for the whole multi-frame ramp:
 *
 *   outer = (a4)+                       ; pass count        -> $2216(a5)
 *   per pass (outer times):
 *     d7 = (a3)+                        ; delay word ($74B0)
 *     wait (delay+1) vblank frames      ; btst toggle x dbra ($74B2-$74C2)
 *     for each 12-byte entry until a 0 longword terminator:
 *       step every R/G/B nibble one step toward its source colour ($74C6-$74D2)
 *
 * So the palette ramps one nibble per pass and the whole fade spans
 * outer*(delay+1) frames (typically 16*2 = 32). The earlier version skipped the
 * waits and ran every pass at once, collapsing the fade to a single frame — the
 * logo fade-ins/outs were invisible and fire appeared to skip instantly.
 *
 * hw_vblank_wait() yields one frame through the game coroutine (same mechanism
 * the recompiled hold loops use), so each pass advances the displayed frame.
 *
 * Animation table layout at A4:
 *   [outer_count : 16][delay : 16]
 *   entries, each 12 bytes: [src_ptr:32][dst_ptr:32][stride:16s][count:16s]
 *   [terminator : 32 = 0]
 * Colors accumulate in the copper list across passes — do NOT restore them in
 * native_rebuild_copper_static().
 * ───────────────────────────────────────────────────────────────────────────── */
/* Runs on the game thread, so the per-pass vblank wait is a plain BLOCKING
 * hw_vblank_wait() — it blocks the game thread one frame and the host releases
 * it, exactly like the recompiled hold loops. No continuation state needed. */
void native_boot_anim_iterator(M68KCtx *ctx)
{
    uint32_t a4 = ctx->A[4];
    uint16_t outer = r16(a4); a4 += 2;   /* a4 -> delay word */
    if (outer > 256) outer = 256;
    w16(ctx->A[5] + 0x2216u, outer);
    uint16_t delay = r16(a4);            /* per-pass vblank delay (constant) */

    for (uint16_t pass = 0; pass < outer; pass++) {
        /* $74B2-$74C2: wait (delay+1) vblank frames before stepping the palette. */
        for (uint16_t wv = 0; wv <= delay; wv++) hw_vblank_wait();

        uint32_t a3 = a4 + 2;            /* skip delay word -> first entry */
        for (int guard = 0; guard < 500; guard++) {
            uint32_t src    = r32(a3); a3 += 4;
            uint32_t dst    = r32(a3); a3 += 4;
            int16_t  stride = (int16_t)r16(a3); a3 += 2;
            int16_t  count  = (int16_t)r16(a3); a3 += 2;
            anim_step_colors(src, dst, stride, count);
            if (r32(a3) == 0) break;     /* longword 0 = end of entry list */
        }
    }

    ctx->A[4] = a4;                      /* matches $74AA: a4 past outer_count */
}

/* Native replacement for $6D714 — the runtime disk-overlay loader.
 *
 * The original relocates its body to $150 and walks a descriptor table at
 * $6DEBA; with d0=1 it loads section 1 (the gameplay overlay): two chunks from
 * Disk.1 (one raw, one ATN!-crunched), then jumps into the loaded code via a
 * pointer at $104.  Decoded + byte-exact validated (see OVERLAY_TEST).
 *
 * We reproduce that natively: load both chunks, flip the runtime to the
 * gameplay bank (g_overlay_active so rt dispatch uses g_fn_table_gp), and enter
 * at $3330. */
/* Load + decrunch the gameplay overlay into chip RAM and flip to the gp bank.
 * Does NOT enter the code — callers choose how to transfer control.
 *
 * IMPORTANT: replicate the `$6D714` block-copy that the original loader does
 * BEFORE jumping into its own relocated body at `$150`. The copy moves
 * $1504 words = 10760 bytes from `$6D734` to `$150..$2A57`. This is not
 * cosmetic relocation: data words inside that source block end up at low-RAM
 * fixed addresses the gameplay engine later reads. Confirmed concrete
 * example: `$1890.w` gets its `$0200` value from this copy (source at
 * `$6EE74`). Without the copy `$1890 = 0`, and `$59AC38`'s level-identity
 * `cmp` at `$59AC56` takes the wrong branch into `jmp(a0)` over data
 * (the W6L2 cold-boot crash).
 *
 * The copy goes IN-PLACE in the post-decrunch chip RAM, so the source bytes
 * at `$6D734` must be the pristine boot-decrunch image when we copy —
 * overlay_load.c's loader_block_copy() re-decrunches it itself. */
void native_overlay_load(void)
{
    /* Pure load lives in overlay_load.c (shared with the standalone bank dumper).
     * It re-decrunches the boot image itself before the $6D734 -> $150 block
     * copy, so it is safe from ANY state (boot, exit-to-menu after gameplay
     * reused $6E000+, retry, …). */
    overlay_load_title();
    g_pc_screen = PC_SCR_OVERLAY;
}

void native_overlay_loader(M68KCtx *ctx)
{
    native_overlay_load();
    printf("[overlay-loader] gameplay overlay loaded; entering $3330\n");
    /* Enter the gameplay code (sets a5=$511E itself). */
    rt_jump(ctx, 0x00003330u);
}

/* Override for $000150 — the loader body the game relocated to low memory and
 * calls from the title interrupt to start the game. We can't enter gameplay
 * here (we're on the IRQ-delivery call stack, not the game coroutine), so load
 * the overlay and ask pc_step_coro to restart the coroutine into $3330. d0 is
 * the section index; section 1 is the gameplay overlay. */
/* Reproduce the d0=0 disk-overlay load NATIVELY into g_mem (8MB, so $577000 is
 * valid free space). Descriptor @ $8D6: 3 Disk.1 chunks, disk byte-offset =
 * diskparam>>8, all ATN!-crunched. ch0=reloc table @ $6E000; ch1=code/data @
 * $3330; ch2=code @ $577000 (relocated using ch0). Entry = $577000.
 * VERIFIED: $577000 disassembles to a clean gameplay init routine (move #$2700,sr;
 * set up DMACON/INTENA/vectors), so load+decrunch+relocation are correct. */
void native_overlay_load_d0(void)
{
    /* Pure load (3 ATN! chunks + 2-pass relocation) lives in overlay_load.c,
     * shared with the standalone bank dumper. It re-decrunches the boot image
     * before the $6D714 block copy, so re-entry from a state where gameplay
     * already reused $6E000+ (retry, level select after play) stays correct. */
    overlay_load_gameplay();
    g_pc_screen = PC_SCR_OVERLAY;
}

/* Override for $000150 — the loader body the game relocated to low memory and
 * calls from the title to start the game. The d0=0 overlay load is implemented
 * + verified (native_overlay_load_d0), entry = $577000. BUT $577000 is
 * runtime-loaded fast-RAM code with no recompiled bank, so we can't RUN it yet
 * (rt_call would skip). Until that bank exists this stub does NOT load during
 * normal play (the load overwrites $3330, breaking the still-running title) —
 * set DUMP_GMEM_AFTER_LOAD=1 to perform the load and dump the recompilable
 * gameplay image to logs/gmem_after_load.bin. */
/* Gameplay overlay's disk reader at $577B8C (jsr -$7286(a5), a5=$57EE12). The
 * "ACCESSING!" screen calls it to stream level data off disk via raw MFM, which
 * PC doesn't emulate. Reproduce natively: d0 = source (offset<<8 | disk-1),
 * d1 = length, d2 = dest; read linearly from the WHDLoad image, report success. */
void native_gp_disk_read(M68KCtx *ctx)
{
    extern int disk_boot_load(int, uint32_t, uint32_t, uint32_t);
    uint32_t src = ctx->D[0], len = ctx->D[1], dest = ctx->D[2];
    uint32_t off = src >> 8;
    int disk = (int)(src & 0xFFu) + 1;
    int rc = disk_boot_load(disk, off, dest, len);
    printf("[gp-disk-read] disk%d off=$%06X len=$%06X dest=$%06X rc=%d\n",
           disk, off, len, dest, rc);
    ctx->D[0] = 0;   /* success: caller does neg.w d0; bmi error -> d0=0 continues */
}

#include <string.h>

/* $577E96 — the in-game level-segment decruncher. The recompiled body handles
 * ONLY "ATN!" (non-ATN falls through to a plain rts, leaving the segment
 * crunched — downstream the level-state init $57CC1A zero-scan then runs off
 * into garbage). The fan-made Disk.4 extra levels are crunched with standard
 * Imploder magic "IMP!" — same container and bitstream as ATN! (the token
 * tables travel in the stream), so route those through our C atn_decrunch
 * (which now accepts both magics) and keep ATN! on the recompiled body.
 * Exit contract is trivial: the original is movem-balanced and no caller
 * consumes d0/flags after the bsr. */
void native_level_decrunch(M68KCtx *ctx)
{
    extern uint32_t atn_decrunch(uint32_t);
    uint32_t magic = MR32(ctx->A[0]);
    if (magic == 0x494D5021u) {          /* "IMP!" -> native Imploder */
        uint32_t n = atn_decrunch(ctx->A[0]);
        printf("[lvl-decrunch] IMP! at $%06X -> %u bytes\n", ctx->A[0], n);
        return;
    }
    extern void gfn_gpl_577E96(M68KCtx *ctx);
    gfn_gpl_577E96(ctx);                 /* ATN! (and the no-op non-match path) */
}

/* Native port of the title-bank glyph blitter at $0049B6 — the routine
 * the menu uses to draw "PLAY GAME" / "ENTER PASSWORD" / "LOAD EXTRA
 * LEVELS" onto the menu bitmap. Once it's ours we can do whatever
 * substitution we want before the blit runs.
 *
 * Algorithm (faithfully translated from the M68K disassembly):
 *   a2 = ASCII string ptr (null-terminated), post-incremented
 *   a1 = pixel destination (bitplane base byte)
 *   d6 = 2nd-column shift offset (caller-provided)
 *   a0 = font glyph base = $1BB30 (16 rows x 86 bytes per row, char_idx
 *        = ascii - $20 indexes into each row)
 *
 *   for each char in (a2)+:
 *     a4 = font + (char - $20)
 *     a3 = a1
 *     for row = 0..15:
 *       d0 = (a4)
 *       OR d0 into 5 bitplanes at $00/$28/$50/$78/$A0 of (a3)
 *       ror #1, d0
 *       OR d0 into (a3, d6.w)
 *       not d0, AND into $28(a3, d6.w)
 *       a4 += $56  ; next row of glyph data
 *       a3 += $C8  ; next row of bitmap
 *     a1 += 1     ; advance one char-column
 *
 * This is a faithful native port of the blitter — it renders exactly the
 * string the engine passes. (The old cursor-1 "ENTER PASSWORD"->"LEVEL SELECT"
 * label swap was removed; LEVEL SELECT is now layered on the faithful menu in
 * pc_menu.c, where the menu item itself is changed rather than the glyphs.) */
void native_menu_glyph_blit(M68KCtx *ctx)
{
    uint32_t orig_a2 = ctx->A[2];

    /* Read up to 31 chars + null. */
    char buf[32] = {0};
    int len = 0;
    while (len < 31) {
        uint8_t c = MR8(orig_a2 + (uint32_t)len);
        if (c == 0) break;
        buf[len++] = (char)c;
    }
    const char *render = buf;

    if (getenv("GLYPH_LOG"))
        fprintf(stderr, "[glyph] a2=$%06X a1=$%06X d6=%d str=\"%s\"\n",
                orig_a2, ctx->A[1], (int)(int16_t)(uint16_t)ctx->D[6], buf);

    const uint32_t font_base = 0x1BB30u;
    uint32_t a1 = ctx->A[1];
    int16_t   d6 = (int16_t)(uint16_t)ctx->D[6];

    for (int ci = 0; render[ci]; ci++) {
        uint8_t  c    = (uint8_t)render[ci];
        uint32_t a4   = font_base + (uint32_t)(c - 0x20);
        uint32_t a3   = a1;

        for (int row = 0; row < 16; row++) {
            uint8_t d0 = MR8(a4);
            MW8(a3,            MR8(a3)            | d0);
            MW8(a3 + 0x28u,    MR8(a3 + 0x28u)    | d0);
            MW8(a3 + 0x50u,    MR8(a3 + 0x50u)    | d0);
            MW8(a3 + 0x78u,    MR8(a3 + 0x78u)    | d0);
            MW8(a3 + 0xA0u,    MR8(a3 + 0xA0u)    | d0);

            /* ror.b #1, d0 — rotate right by 1. */
            d0 = (uint8_t)((d0 >> 1) | ((d0 & 1) << 7));

            uint32_t addr_d6      = a3        + (uint32_t)(int32_t)d6;
            uint32_t addr_d6_p28  = a3 + 0x28u + (uint32_t)(int32_t)d6;
            MW8(addr_d6,     MR8(addr_d6)     | d0);
            MW8(addr_d6_p28, MR8(addr_d6_p28) & (uint8_t)~d0);

            a4 += 0x56u;
            a3 += 0xC8u;
        }
        a1 += 1u;
    }

    /* Engine's exit state: a2 advanced past the null. Even though we used
     * a substituted string for rendering, the caller's a2 should reflect
     * "consumed" the original string. */
    ctx->A[2] = orig_a2 + (uint32_t)len + 1u;
    ctx->A[1] = a1;
}

/* Title-menu cursor handlers — direct C ports of $003C5A (DOWN) and
 * $003C88 (UP). See the comment in pc_register_overrides() for the
 * disassembly each one mimics. */
/* Native main-menu fire-dispatch (gfn_gp_0039D0). Reached when fire was
 * pressed on the main-menu (the original code's $0039BC: btst #5,d0; bne
 * $39D0 — only branches here when the "select" bit is set). cursor =
 * -$18BE(a5) — 0=PLAY GAME, 1=(was PASSWORD, now LEVEL SELECT), 2=LOAD
 * EXTRA LEVELS.
 *
 * Mapping:
 *   cursor 0 -> PLAY GAME      -> rt_jump($150) overlay loader
 *   cursor 1 -> LEVEL SELECT   -> rt_jump($150) using $20.w as set by
 *                                 F2/F3 hotkeys. The original engine path
 *                                 would have run a password-entry screen
 *                                 here and rewritten $20.w to 1 — by
 *                                 intercepting BEFORE that, our F2/F3
 *                                 selection survives.
 *   cursor 2 -> LOAD EXTRA LVL -> needs Disk.4 (see [[project-disk4-extra-levels]]).
 *                                 For now, return to the menu loop top so
 *                                 the user can pick a different row.
 *
 * This override fires INSTEAD of gfn_gp_0039D0; the menu's per-frame
 * setup (drawing, audio, copper) still runs through gfn_gp_003872 each
 * frame, only the final fire-dispatch is ours. */
void native_main_menu_fire_dispatch(M68KCtx *ctx)
{
    extern uint8_t *g_mem;
    uint16_t cursor = MR16(ctx->A[5] - 6334u);

    if (cursor == 1u) {
        /* LEVEL SELECT — open the interactive picker. The user picks a
         * level here (joy UP/DOWN cycles 1..60, FIRE confirms), THEN we
         * do the gameplay hand-off. The engine's per-frame work (audio,
         * music, copper) keeps running because hw_vblank_wait() yields
         * one frame per iteration, but the menu state machine is parked
         * here until we return — no risk of $003C5A et al. double-handling
         * the arrows. */
        extern void hw_vblank_wait(void);
        extern int  hw_joy_up(void);
        extern int  hw_joy_down(void);
        extern int  hw_joy_left(void);
        extern int  hw_joy_right(void);
        extern int  hw_get_fire(void);
        extern int  g_level_select_visible;
        /* pc_set_start_level / pc_get_start_level / pc_level_split come from
         * pc.h (via pc_internal.h) — the single declaration point. */

        g_level_select_visible = 1;

        /* Wait for the entry fire press to release. */
        while (hw_get_fire()) hw_vblank_wait();

        /* World/level geometry comes from the SSoT accessors in pc.h
         * (pc_levels_in_world / pc_world_first_level) — never re-hardcode. */
        int prev_u = 0, prev_d = 0, prev_l = 0, prev_r = 0;
        int cancelled = 0;
        for (;;) {
            hw_vblank_wait();
            /* ESC closes the panel (hw_handle_key clears the flag); without
             * this check the picker loop kept spinning invisibly and the
             * menu appeared frozen. */
            if (!g_level_select_visible) { cancelled = 1; break; }
            int u = hw_joy_up(), d = hw_joy_down();
            int lt = hw_joy_left(), rt = hw_joy_right(), f = hw_get_fire();

            int level = pc_get_start_level();
            int world = 0, liw = 0;
            pc_level_split(level, &world, &liw);
            if (world < 0 || world >= pc_num_worlds_ui()) world = 0;
            int liw_max = pc_levels_in_world(world) - 1;
            if (liw < 0)        liw = 0;
            if (liw > liw_max)  liw = liw_max;

            /* UP/DOWN navigate level within current world (clamped, no wrap).
             * LOCKED targets refuse (pc_profile_try_select): the cursor can
             * only reach completed levels + the next one (or anything with
             * the unlock_all_levels knob on). */
            if (u && !prev_u && liw > 0)
                pc_profile_try_select(pc_world_first_level(world) + (liw - 1));
            if (d && !prev_d && liw < liw_max)
                pc_profile_try_select(pc_world_first_level(world) + (liw + 1));

            /* LEFT/RIGHT cycle worlds with wrap; reset to liw 0 of new world.
             * A world whose first level is locked can't be navigated to —
             * keep scanning in the pressed direction so unlocked worlds
             * remain reachable across a locked gap (wrap-around). */
            /* World cycling includes the Disk.4 extra worlds (slots 7+),
             * placed after world 6 in the wrap order — so LEFT from
             * UNDERWORLD (world 0) lands on the extras, per their billing
             * as the "before Underworld" bonus shelf. */
            int nw = pc_num_worlds_ui();
            if (lt && !prev_l)
                for (int k = 1; k < nw; k++)
                    if (pc_profile_try_select(pc_world_first_level(
                            (world + nw - k) % nw))) break;
            if (rt && !prev_r)
                for (int k = 1; k < nw; k++)
                    if (pc_profile_try_select(pc_world_first_level(
                            (world + k) % nw))) break;

            prev_u = u; prev_d = d; prev_l = lt; prev_r = rt;
            if (f) break;
        }
        g_level_select_visible = 0;
        if (cancelled) {               /* back to the menu, no level start.
                                        * $39D0 is NOT a jsr'd function — it's
                                        * a branch target inside the menu loop;
                                        * its own no-action paths jump to $39BE
                                        * (the loop continuation). A plain C
                                        * return here unwinds the loop and ENDS
                                        * the game thread (frame freezes with
                                        * stale overlays). Jump like the engine
                                        * does — cursor + highlight stay put. */
            rt_jump(ctx, 0x0039BEu);
            return;
        }
        { extern int g_pc_menu_visible; g_pc_menu_visible = 0; }

        /* Same exit setup as the engine's PLAY GAME path
         * ($003AF4..$003B08). The INTREQ clear matters: stale title-bank
         * vector handlers ($003532 / $005694) fire post-overlay-load
         * otherwise and crash. */
        MW16(ctx->A[6] + 0x94u, 0x7FFFu);
        MW16(ctx->A[6] + 0x98u, 0x7FFFu);
        ctx->A[7] = 0x00080000u;
        ctx->D[0] = 0;
        rt_jump(ctx, 0x150u);
        return;
    }
    if (cursor == 2u) {
        /* OPTIONS — same page ESC/Start opens from the title; the menu loop
         * keeps running underneath (proven by the ESC path, f948da7).
         * RESUME the loop with a plain return (the dispatch is a jsr from the
         * loop body) — re-entering $3872 resets the ENGINE cursor to 0 while
         * the on-screen highlight stays put, so fire would then run CONTINUE
         * with OPTIONS still visually selected (user-reported). */
        { extern void pc_pause_open_options(void);
          pc_pause_open_options(); }
        rt_jump(ctx, 0x0039BEu);   /* resume the menu loop (see above) */
        return;
    }
    /* cursor == 0 — CONTINUE — start the FIRST UNCLEARED level (profile.json
     * progress; level 1 on a fresh profile). Uses the same $150 hand-off as
     * the LEVEL SELECT picker (the engine's own PLAY GAME path $3AF4..$3B08
     * always starts level 1, so it is no longer delegated to). The first
     * uncleared level is always unlocked (everything before it is cleared),
     * so try_select cannot refuse; the level-1 fallback is belt-and-braces. */
    {
        { extern int g_pc_menu_visible; g_pc_menu_visible = 0; }
        int next = pc_menu_continue_level();
        if (!pc_profile_try_select(next)) pc_set_start_level(1);
        MW16(ctx->A[6] + 0x94u, 0x7FFFu);
        MW16(ctx->A[6] + 0x98u, 0x7FFFu);
        ctx->A[7] = 0x00080000u;
        ctx->D[0] = 0;
        rt_jump(ctx, 0x150u);
    }
}

/* ── $003872 — title main-menu setup+loop. We OWN the option setup ───────────────
 * The menu draws three option records (PLAY GAME / ENTER PASSWORD / LOAD EXTRA
 * LEVELS) from the gp image at a5-relative addresses, via the glyph blitter
 * $0049B6. Rather than hooking the per-glyph drawer (a presentation hack), we
 * construct the option list ourselves: rewrite item 1's string ("ENTER PASSWORD"
 * at a5-$696) to "LEVEL SELECT" BEFORE the engine builds/draws the menu, so the
 * engine's own renderer draws our option. Selecting it (cursor 1) is handled by
 * native_main_menu_fire_dispatch ($0039D0) -> native level picker -> $150.
 *
 * The string lives in the loaded gp image; re-applying on every menu entry keeps
 * it correct across reloads (attract loop, LOAD-EXTRA return, exit-to-menu). */
/* First uncleared VANILLA level — what CONTINUE starts. */
int pc_menu_continue_level(void)
{
    int next = 1;
    while (next < PC_NUM_LEVELS && pc_profile_completed(next)) next++;
    return next;
}

static void menu_write_string(uint32_t dst, const char *str)
{
    extern uint8_t *g_mem;
    uint32_t i = 0;
    for (; str[i]; i++) g_mem[dst + i] = (uint8_t)str[i];
    g_mem[dst + i] = 0;
}

/* Draw a string onto a menu PAGE with the engine's own glyph renderer
 * (native_menu_glyph_blit) — used for the DISK.4 indicator. The string is
 * staged in a scratch corner of g_mem the menu never touches. */
static void menu_page_text(uint32_t page_addr, const char *str)
{
    extern void native_menu_glyph_blit(M68KCtx *ctx);
    const uint32_t stage = 0x7FFF00u;
    /* Trailing space: the glyph renderer's shifted second column AND-carves
     * the NEXT byte-column in the glyph's shape; mid-string that carve is
     * overdrawn by the following char, but the LAST char's carve stays — a
     * "ghost" of the final glyph cut into the textured background. A space
     * (blank glyph) as the final char makes the trailing carve a no-op. */
    char buf[34];
    snprintf(buf, sizeof buf, "%s ", str);
    menu_write_string(stage, buf);
    M68KCtx t = {0};
    t.A[2] = stage;
    t.A[1] = page_addr;
    t.D[6] = 1601;                    /* engine menu-font shadow: +8 rows +1 col
                                       * (GLYPH_LOG shows d6=1601 for all items) */
    native_menu_glyph_blit(&t);
}

/* On-page menu extras, drawn after each page-1 art unpack
 * (native_menu_art_unpack): the "DISK.4 LOADED" indicator above the beach
 * window when the extra-levels disk was detected. Page offset = row*$C8 +
 * byte-col. (The CONTINUE target is shown by the pc_menu_subtext_overlay
 * small-font subtext instead — see level_select_ui.c.) */
#define MENU_D4_TEXT_OFF  (150u * 0xC8u + 19u)
void native_menu_indicator(uint32_t page)
{
    if (pc_extra_worlds_available() > 0)
        menu_page_text(page + MENU_D4_TEXT_OFF, "DISK.4 LOADED");
}

int g_pc_menu_visible = 0;   /* main menu on screen (subtext overlay gate) */

void native_menu_setup(M68KCtx *ctx)
{
    g_pc_menu_visible = 1;
    /* Item 0: "PLAY GAME" -> "CONTINUE". STRICT 10-char+NUL budget: the next
     * byte (a5-$699) is the high byte of item 1's page-dest pointer
     * ($04CE8A) — an 11-char string NUL's it and the LEVEL SELECT row gets
     * drawn into low RAM (burned 2026-06-11). The level it will start is
     * drawn separately right after the text (native_menu_indicator). */
    menu_write_string(ctx->A[5] - 0x6A4u, "CONTINUE");

    menu_write_string(ctx->A[5] - 0x696u, "LEVEL SELECT");  /* <= "ENTER PASSWORD" */

    /* Item 2: "LOAD EXTRA LEVELS" -> "OPTIONS". Extra levels need no manual
     * load step anymore: Disk.4 is auto-detected and its worlds appear in
     * LEVEL SELECT; the freed row becomes the visible OPTIONS entry. */
    menu_write_string(ctx->A[5] - 0x682u, "OPTIONS");

    /* Engine extras flag (the old LOAD EXTRA LEVELS action) — set it
     * automatically whenever the disk provides extra worlds. */
    if (pc_extra_worlds_available() > 0) MW8(0x38u, 0xFFu);

    /* On-page indicators are drawn post-unpack by native_menu_art_unpack
     * ($3700 hook) — the engine renders the menu on page 1 only (GLYPH_LOG:
     * every item draw targets $4xxxx; page 2 is the pwfield restore source). */

    extern void gfn_gp_003872(M68KCtx *ctx);
    gfn_gp_003872(ctx);                            /* engine builds/draws/loops the menu */
}

/* $3700 — menu-art unpacker post-hook: page 1 ($49000) is unpacked from the
 * compressed art each menu entry, so on-page extras (the DISK.4 indicator)
 * must be re-drawn after every unpack. */
#define MENU_ART_SRC 0x1339Au
void native_menu_art_unpack(M68KCtx *ctx)
{
    extern void gfn_gp_003700(M68KCtx *ctx);
    int is_menu_art = (ctx->A[0] == MENU_ART_SRC);
    uint32_t dst = ctx->A[1];
    gfn_gp_003700(ctx);
    if (is_menu_art) native_menu_indicator(dst);
}

void native_menu_cursor_down(M68KCtx *ctx)
{
    /* $003C5A — direct port:
     *   move.w #$384, $2BE2(a5)
     *   cmpi.w #$2,   -$18BE(a5)
     *   beq.s  end
     *   addq.w #1,    -$18BE(a5)
     *
     * Confirmed working at main-menu (cop1lc=$008302). Cap = 2 because
     * the menu has 3 options: PLAY GAME / ENTER PASSWORD / LOAD EXTRA
     * LEVELS. The third is backed by Disk.4 and is conditional on the
     * disk being present (see [[project-disk4-extra-levels]]) — when
     * the row is hidden we still leave the engine's cursor cap at 2;
     * the UI just doesn't render row 3. */
    MW16(ctx->A[5] + 0x2BE2u, 0x0384);
    uint16_t cur = MR16(ctx->A[5] - 6334u);
    if (cur != 2) MW16(ctx->A[5] - 6334u, cur + 1);
}

void native_menu_cursor_up(M68KCtx *ctx)
{
    /* $003C88 — symmetric: decrement cursor if > 0. */
    MW16(ctx->A[5] + 0x2BE2u, 0x0384);
    uint16_t cur = MR16(ctx->A[5] - 6334u);
    if (cur != 0) MW16(ctx->A[5] - 6334u, cur - 1);
}

/* ── $003C6E / $003C9A — main-menu LEFT/RIGHT: the DIFFICULTY selector ────────
 * Direct ports of the live disassembly (capstone over the loaded title
 * overlay; these two never recompiled — they were stubbed to a no-op, which
 * is why left/right on PLAY GAME did nothing):
 *
 *   $003C6E (LEFT):  move.w #$384,$2BE2(a5)
 *                    tst.w  -$18BE(a5)   ; only on the PLAY GAME row
 *                    bne.s  end
 *                    btst   #0,$1F.w     ; already at the lowest setting?
 *                    bne.s  end
 *                    ror.w  $1E.w        ; rotate the difficulty bit right
 *
 *   $003C9A (RIGHT): same with btst #2 (highest) + rol.w $1E.w, then falls
 *                    through to $3CB2: `cmpi.w #1,-$18BE(a5)` → the password-
 *                    FIELD entry init (cursor into the text field). Cursor 1
 *                    is LEVEL SELECT in this port (no password field), so the
 *                    tail is intentionally NOT ported.
 *
 * Difficulty = the single set bit in the word at $1E (1=lowest, 2, 4=highest;
 * the menu loop's `and.w #7,$1E.w; lsl #2` indexes a per-difficulty record at
 * -$18C0(a5), so the row label/art follows automatically). */
void native_menu_diff_left(M68KCtx *ctx)
{
    MW16(ctx->A[5] + 0x2BE2u, 0x0384);
    if (MR16(ctx->A[5] - 6334u) != 0) return;      /* not on PLAY GAME */
    if (MR8(0x1Fu) & 0x01u) return;                /* lowest already   */
    uint16_t v = MR16(0x1Eu);
    MW16(0x1Eu, (uint16_t)((v >> 1) | (v << 15))); /* ror.w #1 */
}

/* ── Vestigial password FIELD ("3MQLGPQLGP") beside LEVEL SELECT — RE'd ───────
 * Final RE (2026-06-11), correcting BOTH earlier theories. Live pcwatch on the
 * field region during a real menu entry shows three writers: $3700 (art unpack,
 * background), $49B6 (menu option strings) and **$3DAA — the field TEXT
 * renderer**. The "glyphs are baked into the artwork" claim (2026-06-10) was
 * FALSE: the text is created at runtime, cell by cell, by $3DAA:
 *
 *  - $3DAA (one cell at cursor offset -$15B6(a5)): phase 1 restores the cell
 *    background by copying page 2 → page 1 ($5CE99→$4CE99, 16 rows x 6 +$28
 *    steps); phase 2 reads the char from the live password buffer -$1380(a5),
 *    indexes the font at $1BB30 by (char-$20) and AND/OR-draws the glyph.
 *    The field init walks it across all 10 chars; the menu idle loop re-enters
 *    it every frame via `beq.w $3daa` in gfn_gp_003BAA (an rt_jump — fully
 *    dispatchable). The inline copy of its body inside gfn_gp_003BAA is
 *    UNREACHABLE double-emission bloat (no goto targets it).
 *  - Why every earlier entry override "didn't fire": rt.c's override_lookup
 *    SKIPS title-bank overrides at addresses >= $3294 while g_overlay_active,
 *    unless allow-listed (the menu allow-list). $3DAA/$3700 simply weren't on
 *    the list — nothing was inline. Both are allow-listed now.
 *
 * Ownership fix: native_menu_pwfield_draw overrides $3DAA and draws NOTHING —
 * passwords are replaced by LEVEL SELECT in this port, so the field has no
 * reason to exist. With the creator dead, the field area shows the art's own
 * clean background — VERIFIED (2026-06-11, override-only screenshots, normal +
 * highlighted cursor states): the art ships NO baked field text. The former
 * weave-copy erase machinery + the $3700 unpack wrapper were deleted. */
/* $3DAA — the password-field cell renderer (see the RE block above). The field
 * does not exist in this port; never draw it. */
void native_menu_pwfield_draw(M68KCtx *ctx)
{
    /* Faithful state port of $3DAA minus the pixel writes. The routine is NOT
     * draw-only: besides rendering the cell it copies the current char from
     * the source walker *( -$13B4(a5) ) into the text shadow at
     * -$1380(a5) + -$15B6(a5) — and the PLAY GAME fire path DECODES that
     * shadow (password == start level/difficulty state), so a bare no-op
     * breaks game start (verified: rungame stuck in menu until this write
     * was restored). Registers need no mimicry: the original clobbers
     * d1/d2/d7/a0-a3 as scratch, so no caller may rely on them. */
    int16_t  off = (int16_t)MR16(ctx->A[5] - 5558u);   /* -$15B6: char offset  */
    uint32_t src = MR32(MR32(ctx->A[5] - 5044u));      /* *(-$13B4): char ptr  */
    MW8(ctx->A[5] - 4992u + (uint32_t)(int32_t)off,    /* -$1380 text shadow   */
        MR8(src));
}


void native_menu_diff_right(M68KCtx *ctx)
{
    MW16(ctx->A[5] + 0x2BE2u, 0x0384);
    if (MR16(ctx->A[5] - 6334u) != 0) return;      /* not on PLAY GAME */
    if (MR8(0x1Fu) & 0x04u) return;                /* highest already  */
    uint16_t v = MR16(0x1Eu);
    MW16(0x1Eu, (uint16_t)((v << 1) | (v >> 15))); /* rol.w #1 */
}

void native_overlay_loader_reloc(M68KCtx *ctx)
{
    extern uint8_t *g_mem;
    extern int      disk_boot_load(int, uint32_t, uint32_t, uint32_t);
    extern uint32_t atn_decrunch(uint32_t);

    if (ctx->D[0] == 3u) {
        /* d0=3: END-GAME / credits overlay. Path taken by $5773A2 (the win-
         * sequence tail: marry-man-repairs-machine → teleport → cutscene).
         * Decoded from the chunk descriptor table at relocated $8D6 (source
         * $6DEBA): group d0=3 has ONE entry — Disk.3 off $0C7100, len $1888C,
         * decrunch to $3330 — followed by a callback `$00000100` meaning
         * "after load, jmp via mem[$100]" (which is the dest just pushed:
         * $3330). So effectively: load disk3 → decrunch at $3330 → jmp $3330. */
        overlay_load_credits();   /* shared pure loader (see overlay_load.c) */
        if (getenv("DUMP_GMEM_AFTER_CREDITS")) {
            FILE *df = fopen("logs/gmem_after_credits.bin", "wb");
            if (df) { fwrite(g_mem, 1, 0x600000, df); fclose(df); }
            fprintf(stderr, "[overlay-loader] $150 d0=3: credits overlay"
                    " loaded; dumped logs/gmem_after_credits.bin; exiting.\n");
            exit(0);
        }
        /* Flip dispatch to the credits/end-game bank — different bytes at
         * $3330+ than gameplay, so the gpl table mustn't match here. */
        g_pc_screen = PC_SCR_CREDITS;
        printf("[overlay-loader] $150 d0=3: end-game/credits overlay loaded;"
               " entering $3330 with credits bank active\n");
        rt_jump(ctx, 0x00003330u);
        return;
    }
    if (ctx->D[0] != 0) {
        printf("[overlay-loader] $150 d0=%u unhandled\n", (unsigned)ctx->D[0]);
        return;
    }
    /* This is the real menu→"Start Game" hand-off: load the gameplay overlay
     * (3 disk chunks + ATN! decrunch + two-pass relocation) and enter $577000.
     * Sets g_enter_gameplay so pc_step_coro restarts the coroutine on the
     * gameplay bank (same path PC_FORCE_GAMEPLAY uses).
     * DUMP_GMEM_AFTER_LOAD=1 just dumps the loaded image without entering. */
    native_overlay_load_d0();

    /* The real $150 routine (replaced by this override) also initialises the
     * low-memory display pointers it uses — notably $3e and $184 = $A68 (a fixed
     * immediate in that code). Without this, $3e stays 0, so the card glyph
     * renderer ($578162, dest = *($3e)+$1c) writes over low memory $100 (the
     * disk-chunk pointer table); gameplay then walks a null chain at $57D11A and
     * hangs. Replicate that pointer init here. */
    { uint32_t a;
      for (a = 0x3eu; ; a = 0x184u) {
          g_mem[a] = 0x00; g_mem[a+1] = 0x00; g_mem[a+2] = 0x0A; g_mem[a+3] = 0x68;
          if (a == 0x184u) break;
      } }

    if (getenv("DUMP_GMEM_AFTER_LOAD")) {
        FILE *df = fopen("logs/gmem_after_load.bin", "wb");
        if (df) { fwrite(g_mem, 1, 0x600000, df); fclose(df); }
        printf("[overlay-loader] $150 d0=0: loaded; dumped logs/gmem_after_load.bin\n");
        return;
    }
    /* Preload all 60 level names natively, BEFORE the game ever runs.
     *
     * The boot-time gameplay overlay loaded at $577000 includes a per-world
     * disk-load descriptor table at $577452. Each world is a zero-terminated
     * list of chunks; each chunk = 3 longwords (dest_metadata, src_encoded,
     * length). For each world, the LAST chunk is the one containing the
     * level descriptor + 10-entry name table at byte offset $61.
     *
     * src_encoded layout: (disk_offset << 8) | (disk_index_zero_based).
     *
     * We read each world's last chunk into a scratch g_mem area, run the
     * existing atn_decrunch on it, then scrape the names — no engine state
     * touched, no levels played. See [[project-title-card-structure]] for
     * the renderer-buffer copy code at $57CBBA (g_mem-resident at runtime). */
    {
        extern void pc_preload_all_level_names(void);
        pc_preload_all_level_names();
    }

    /* Level-select hand-off: the gameplay dispatcher at $5779AA reads $20.w
     * (subtract 1, *4, indexes the 60-entry level table at $57782E). The
     * title's password-screen code at $003A40 clamps $20.w to 1..$3c every
     * frame it runs, so any pre-fire poke gets reverted. We're past the
     * title now (it just jmp'd to $150), so this is the right moment to
     * apply the user's selection from pc_set_start_level(). g_pc_start_level
     * is 0 when no override was set — leave $20.w alone in that case so the
     * normal title-driven value (level 1 at fresh boot) stands. */
    if (g_pc_start_level > 0) {
        int n = g_pc_start_level;
        g_mem[0x20] = 0; g_mem[0x21] = (uint8_t)n;
        printf("[level-select] applying $20.w = %d at $150 hand-off\n", n);
    }
    g_gameplay_entry = 0x577000u;
    g_enter_gameplay = 1;
    printf("[overlay-loader] $150 d0=0: gameplay overlay loaded; entering $577000\n");
}
