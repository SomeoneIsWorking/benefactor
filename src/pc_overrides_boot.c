/* pc_overrides_boot.c — Boot-time animation overrides (native PC C, no M68K emulation) */
#include "pc_internal.h"

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
void native_boot_anim_iterator(M68KCtx *ctx)
{
    uint32_t a4 = ctx->A[4];

    uint16_t outer = r16(a4); a4 += 2;   /* a4 -> delay word */
    if (outer > 256) outer = 256;        /* guard against bad table data */
    w16(ctx->A[5] + 0x2216u, outer);

    uint16_t delay = r16(a4);            /* per-pass vblank delay (constant) */

    for (uint16_t pass = 0; pass < outer; pass++) {
        /* $74B2-$74C2: wait (delay+1) vblank frames before stepping the palette. */
        for (uint16_t w = 0; w <= delay; w++)
            hw_vblank_wait();

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
 * Does NOT enter the code — callers choose how to transfer control. */
void native_overlay_load(void)
{
    extern int      disk_boot_load(int, uint32_t, uint32_t, uint32_t);
    extern uint32_t atn_decrunch(uint32_t);
    extern int      g_overlay_active;

    /* E1: raw data -> $50000 (Disk.1 off $F3780, len $1880) */
    disk_boot_load(1, 0x0F3780u, 0x00050000u, 0x1880u);
    /* E2: ATN!-crunched gameplay code -> $3330 (Disk.1 off $026270), decrunch */
    disk_boot_load(1, 0x026270u, 0x00003330u, 0x2E2E4u);
    atn_decrunch(0x00003330u);

    /* Dest-pointer stack the original loader builds at $100/$104 (big-endian). */
    extern uint8_t *g_mem;
    g_mem[0x100] = 0x00; g_mem[0x101] = 0x05; g_mem[0x102] = 0x00; g_mem[0x103] = 0x00;
    g_mem[0x104] = 0x00; g_mem[0x105] = 0x00; g_mem[0x106] = 0x33; g_mem[0x107] = 0x30;

    g_overlay_active = 1;
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
    extern uint8_t *g_mem;
    extern int      disk_boot_load(int, uint32_t, uint32_t, uint32_t);
    extern uint32_t atn_decrunch(uint32_t);
    extern int      g_overlay_active;
    static const struct { uint32_t off, dst, len; } ch[3] = {
        { 0x0548A0u, 0x06E000u, 0x001D1Au },
        { 0x0565BAu, 0x003330u, 0x012404u },
        { 0x0689BEu, 0x577000u, 0x012A1Cu },
    };
    for (int i = 0; i < 3; i++) { disk_boot_load(1, ch[i].off, ch[i].dst, ch[i].len);
                                  atn_decrunch(ch[i].dst); }
    g_mem[0x100]=0x00; g_mem[0x101]=0x06; g_mem[0x102]=0xE0; g_mem[0x103]=0x00;
    g_mem[0x104]=0x00; g_mem[0x105]=0x00; g_mem[0x106]=0x33; g_mem[0x107]=0x30;
    g_mem[0x108]=0x00; g_mem[0x109]=0x57; g_mem[0x10A]=0x70; g_mem[0x10B]=0x00;
    g_overlay_active = 1;
    #define RD32(a) (((uint32_t)g_mem[(a)]<<24)|((uint32_t)g_mem[(a)+1]<<16)\
                    |((uint32_t)g_mem[(a)+2]<<8)|g_mem[(a)+3])
    #define WR32(a,v) do{uint32_t _v=(v),_a=(a); g_mem[_a]=_v>>24; g_mem[_a+1]=_v>>16;\
                        g_mem[_a+2]=_v>>8; g_mem[_a+3]=_v;}while(0)
    /* TWO relocation passes (loader $204-$23C), reading one continuous table at
     * a0=$6E000, all writing into the code base a4=$577000:
     *   pass 1 base = dest[2] = $577000 (relocate $577000-region pointers)
     *   pass 2 base = dest[1] = $3330   (relocate pointers into the $3330 code,
     *                                    e.g. the level copper $3484 = $154+$3330)
     * Each pass = 2 blocks of [count, base-adjust, offsets...]; d1 = base + running
     * adjust; *(a4+off) += d1. (Earlier the missing pass 2 left COP1LC=$154 not $3484.) */
    { uint32_t a0 = 0x06E000u, a4 = 0x577000u;
      uint32_t base[2] = { a4, 0x00003330u };
      for (int pass = 0; pass < 2; pass++) {
          uint32_t d1 = base[pass];
          for (int blk = 0; blk < 2; blk++) {
              uint32_t d7 = RD32(a0); a0 += 4;
              d1 += RD32(a0); a0 += 4;
              for (uint32_t k = 0; k <= d7; k++) { uint32_t off = RD32(a0); a0 += 4;
                                                   WR32(a4 + off, RD32(a4 + off) + d1); } } } }
    #undef RD32
    #undef WR32
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
        /* LEVEL SELECT — start the F2/F3-picked level. The original PLAY
         * GAME exit at $003B06 does `moveq #0, d0` before jmp $150 — mirror
         * that so native_overlay_loader_reloc takes the d0==0 path
         * (otherwise it logs "unhandled" and bails). */
        ctx->D[0] = 0;
        rt_jump(ctx, 0x150u);
        return;
    }
    if (cursor == 2u) {
        /* LOAD EXTRA LEVELS — Disk.4 not yet supported. Return to menu top. */
        rt_jump(ctx, 0x003872u);
        return;
    }
    /* cursor == 0 — PLAY GAME — preserve the original engine's behaviour
     * by delegating to the recompiled function. */
    extern void gfn_gp_0039D0(M68KCtx *ctx);
    gfn_gp_0039D0(ctx);
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

void native_overlay_loader_reloc(M68KCtx *ctx)
{
    extern uint8_t *g_mem;
    extern int g_enter_gameplay;
    extern uint32_t g_gameplay_entry;
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
    extern int g_pc_start_level;
    if (g_pc_start_level > 0) {
        int n = g_pc_start_level;
        g_mem[0x20] = 0; g_mem[0x21] = (uint8_t)n;
        printf("[level-select] applying $20.w = %d at $150 hand-off\n", n);
    }
    g_gameplay_entry = 0x577000u;
    g_enter_gameplay = 1;
    printf("[overlay-loader] $150 d0=0: gameplay overlay loaded; entering $577000\n");
}
