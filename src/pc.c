/* pc.c – Native PC game engine — core (main loop, state machine, init/fini)
 *
 * Override implementations live in pc_overrides.c.
 */
#include "pc_internal.h"
/* game.h is included here ONLY for GAME_FN_COUNT (dispatch table size).
 * Do NOT call any gfn_* functions directly from this file. */
#include "generated/game.h"
#include "recomp/disk_boot.h"
#include "recomp/overlay_load.h"
#include <setjmp.h>
#include <pthread.h>

#ifdef HARNESS_BUILD
#include "harness/trace.h"
#include "harness/puae_state.h"
#endif

/* ── Globals ──────────────────────────────────────────────────────────────── */
uint8_t *g_chip = NULL;
const int g_fn_count = GAME_FN_COUNT;
static int s_harness_mode = 0;

/* Diagnostic flag: when nonzero, pc_loadstate accepts a savestate whose
 * identity word doesn't match this binary's `&g_state`. The coroutine resume
 * will probably crash (saved IP/SP point at the old binary's .text), but the
 * register file + g_mem load cleanly so we can read the saved game state. */
int g_pc_force_load_identity_mismatch = 0;

/* Single source of truth for every piece of state that constitutes a savestate
 * (M68K registers, coroutine, custom-chip shadows, audio, bank-routing flags,
 * state-machine flags). All the legacy names (s_regs, s_dmacon, g_overlay_active,
 * …) are macros that alias fields on this instance — see game_state.h. */
GameState g_state;

/* Apply the non-zero defaults that the original-storage initializers used to
 * carry. Called at startup (and could be called by a "reset" sequence). */
static void pc_state_reset_defaults(void)
{
    memset(&g_state, 0, sizeof g_state);
    s_diwstrt       = 0x2C81;
    s_diwstop       = 0x2CC1;
    g_gameplay_entry = 0x00003330u;
}

void pc_set_harness_mode(int on) { s_harness_mode = on; hw_set_no_pace(on); }

/* Per-frame audio: one displayed frame's worth of samples (50Hz @ 22050Hz). */
#define PC_AUD_SPF     441
/* The gameplay music player ($53A2/$59BFA6) is driven by the CIA-B timer, which
 * fires ~3x per displayed frame — delivering its ISR once/frame plays the song
 * ~3x too slow (measured vs PUAE, see project-native-music-player). The intro
 * and title/menu players advance once per frame. So gameplay gets GP_MUSIC_TICKS
 * sub-frame deliveries; everything else gets one. (TODO: derive the exact count
 * from the CIA-B timer period instead of this constant.) */
#define GP_MUSIC_TICKS 3

/* INTENA bits used to decide whether an installed interrupt vector may fire,
 * mirroring the CPU's interrupt-priority logic. INTEN (bit14) is the master
 * enable; a level only fires when master AND that level's source bit are set. */
#define INTENA_MASTER  0x4000u
#define INTENA_LVL6    0x2000u            /* EXTER — CIA-B timer (music/timer) */
#define INTENA_LVL3    0x0070u            /* VERTB | COPER | BLIT (vblank/copper) */

/* Call a recompiled function, saving/restoring all registers around it (used for
 * per-frame IRQ delivery). The IRQ runs on the MAIN thread while the game thread
 * is parked at its vblank wait, so it shares s_game_ctx/g_mem race-free; the
 * save/restore mirrors the CPU stacking registers across an interrupt. */
static void call_fn(M68KCtx *ctx, uint32_t addr)
{
    PC_LOG("-> $%06X\n", addr);
    uint32_t sa[8], sd[8];
    uint8_t n=ctx->N,z=ctx->Z,v=ctx->V,c=ctx->C,x=ctx->X;
    for (int i=0;i<8;i++) sa[i]=ctx->A[i];
    for (int i=0;i<8;i++) sd[i]=ctx->D[i];
    rt_call(ctx, addr);
    for (int i=0;i<8;i++) ctx->A[i]=sa[i];
    for (int i=0;i<8;i++) ctx->D[i]=sd[i];
    ctx->N=n;ctx->Z=z;ctx->V=v;ctx->C=c;ctx->X=x;
    PC_LOG("<- $%06X\n", addr);
}

/* ── Game loop (single path: disk-boot coroutine) ───────────────────────────── */

int pc_run(void)
{
    PC_LOG("entering game loop\n");
    /* The standalone is nothing more than a loop over the SAME per-frame advance
     * the harness drives (pc_step). No driver-specific game logic: pc_step does
     * the complete frame (game + IRQ/music + audio) so the PC behaves identically
     * however it is driven. */
    while (hw_running) {
        if (pc_step()) break;
    }
    PC_LOG("game loop exited\n");
    return 0;
}


int pc_step_threaded(void);
void pc_music_tick(void);

/* The single per-frame advance. Runs the game's own flow for one displayed frame
 * (presenting + delivering the vblank IRQ at its yields), then delivers the
 * level-6 music ISR at its per-screen rate and renders this frame's audio. Used
 * identically by pc_run (standalone) and the harness STEP_PC — no behavioural
 * difference between the two. */
/* Debug: force LEVEL COMPLETE (the teleport win). The win is gated by bit5 of
 * the $10AC(a5) flags byte ($0057FEBE bit5 = 0x20): the per-level main loop at
 * $5770F8 reaches `$5771DE: btst #5,$10ac; bne $5771FE`, which falls into the
 * level-complete sequence at $577218 (the LEVEL COMPLETE banner, then stop
 * audio at $5772A0, then $5772D6 "wait for fire to continue"). Setting bit5
 * makes the next main-loop pass take the win exactly as the real teleport-out
 * would. */
void pc_debug_complete_level(void)
{
    if (!g_gameplay_active || !g_mem) return;
    g_mem[0x0057FEBEu] |= 0x20;
}

/* Debug: force GAME OVER. Death sets bit15 of the end-of-level flags word
 * $10AC(a5) = $0057FEBE; the state code routes that to the game-over handler
 * ($578C3E, $1E!=8 → banner → CONTINUE/GAME OVER menu). */
void pc_debug_game_over(void)
{
    if (!g_gameplay_active || !g_mem) return;
    g_mem[0x0057FEBEu] |= 0x80;
}

/* Native level-select. The gameplay dispatcher at $5779AA reads $20.w
 * (low chip mem), does (level-1)*4, and indexes the 60-entry level table
 * at $57782E to pick (world, level_in_world). So $20.w == N selects level
 * N directly.
 *
 * BUT the title's password-screen code at $003A40 clamps $20.w EVERY frame
 * it runs (and at $003A04 unconditionally writes 1 when the password sub-
 * menu is entered), so any pre-fire poke to $20.w gets reverted. The right
 * moment to apply the user's selection is INSIDE native_overlay_loader_reloc
 * (the $150 override), which fires once when the title's "start game" path
 * jumps to $150 — after the title is done validating. Store the selection
 * here; the loader override reads it.
 *
 * 0 = no override (use the title's natural value, level 1 at fresh boot). */
/* g_pc_start_level moved to g_state (see game_state.h). */

void pc_set_start_level(int n)
{
    if (n < 1)  n = 1;
    if (n > 60) n = 60;   /* without the secret flag at $38; with it, 90 */
    g_pc_start_level = n;
    fprintf(stderr, "[level-select] start level := %d (applied at $150 hand-off)\n", n);
}

/* Read pending level-select choice (0 = none / pass-through). */
int pc_get_start_level(void)
{
    return g_pc_start_level > 0 ? g_pc_start_level : 1;
}

/* ── Level / world layout — the ONE place the geometry is defined ────────────
 * Mirrors the engine's level table at $57782E (worlds 0-1 have 9 levels,
 * 2-5 have 10, 6 has 2). We compute directly rather than reading $57782E
 * because that region is only populated in chip RAM after the gameplay
 * overlay loads; before that it's zeros, which would map every level to
 * (world 0, liw 0). The table is fixed game data, so the mirror can't
 * diverge. Everything else derives from pc_levels_in_world(). */
int pc_levels_in_world(int world)
{
    static const int wcount[PC_NUM_WORLDS] = { 9, 9, 10, 10, 10, 10, 2 };
    if (world < 0 || world >= PC_NUM_WORLDS) return 0;
    return wcount[world];
}

int pc_world_first_level(int world)
{
    if (world < 0 || world >= PC_NUM_WORLDS) return 0;
    int g = 1;
    for (int w = 0; w < world; w++) g += pc_levels_in_world(w);
    return g;
}

/* Map global level (1..60) -> (world, level_in_world). ALWAYS use this
 * rather than divmod-by-10 — the per-world counts are irregular. */
void pc_level_split(int level, int *world_out, int *level_in_world_out)
{
    int world = 0, liw = 0;
    if (level >= 1 && level <= PC_NUM_LEVELS) {
        int n = level - 1;
        for (int w = 0; w < PC_NUM_WORLDS; w++) {
            int c = pc_levels_in_world(w);
            if (n < c) { world = w; liw = n; break; }
            n -= c;
        }
    }
    if (world_out) *world_out = world;
    if (level_in_world_out) *level_in_world_out = liw;
}

/* World names — preloaded from disk alongside the level names. They live
 * in each world's last chunk near the start, Caesar-shifted by +0x1A on
 * letters (spaces pass through unchanged). pc_preload_all_level_names()
 * decodes + caches them. */
static char g_pc_preloaded_world_names[PC_NUM_WORLDS][32];
static int  g_pc_preloaded_names_ready;   /* defined below */
const char *pc_world_name(int world)
{
    if (world < 0 || world >= PC_NUM_WORLDS) return "?";
    if (!g_pc_preloaded_names_ready) pc_preload_all_level_names();
    if (g_pc_preloaded_names_ready && g_pc_preloaded_world_names[world][0])
        return g_pc_preloaded_world_names[world];
    return "?";
}

/* All 60 level names are read DIRECTLY from disk at gameplay-overlay-load
 * time — no playing through, no hardcoding. The mapping:
 *
 *   gameplay overlay (loaded once by native_overlay_loader_reloc) puts
 *   the world-descriptor table at $577452. Each world is a zero-
 *   terminated list of chunks (each chunk = 3 longwords: dest_metadata,
 *   src_encoded, length). The LAST chunk per world contains the 10-entry
 *   level-name array starting at byte offset $60 (44 bytes per entry,
 *   each entry: padding bytes + `}.` marker + `"NAME"` + padding).
 *
 *   src_encoded = (disk_offset << 8) | (zero_based_disk_index).
 *
 * pc_preload_all_level_names() reads each world's last chunk into a
 * scratch g_mem area, runs atn_decrunch in place, and scrapes the
 * names. Runs once. State is not perturbed. */
static char g_pc_preloaded_names[PC_NUM_LEVELS][32];
/* g_pc_preloaded_names_ready forward-declared above (used by pc_world_name). */

void pc_preload_all_level_names(void)
{
    extern uint8_t *g_mem;
    extern int      disk_boot_load(int, uint32_t, uint32_t, uint32_t);
    extern uint32_t atn_decrunch(uint32_t);

    if (g_pc_preloaded_names_ready) return;
    if (!g_mem) return;

    /* Per-world level counts / global offsets come from the SSoT accessors
     * (pc_levels_in_world / pc_world_first_level) — never re-hardcode them. */
    /* Scratch buffer for decompression — picked to stay clear of every
     * dest used by the gameplay overlay ($577000-$589A1C) and the per-level
     * disk reads ($073880, $5AC4EA+, $5BE77E+, $5C4874+, $5D902A+, $5F1A88+).
     * g_mem is 8MB, so $700000 sits well past all live regions. */
    const uint32_t scratch = 0x700000u;

    #define RD32(a) ( ((uint32_t)g_mem[(a)]   << 24) \
                    | ((uint32_t)g_mem[(a)+1] << 16) \
                    | ((uint32_t)g_mem[(a)+2] <<  8) \
                    |  (uint32_t)g_mem[(a)+3]       )

    /* The world-descriptor table lives at $577452, INSIDE the gameplay
     * overlay's $577000+ chunk. That chunk is loaded into $577000 by
     * native_overlay_load when the user actually picks PLAY GAME — but
     * we want names available the moment the main-menu shows up.
     *
     * Solution: load the overlay's chunk-2 (the one containing $577452)
     * into a SCRATCH location, point our table-walker at it, then
     * proceed normally. The title bank at $3000+ stays untouched. The
     * scratch is $700000 — same area we use for per-world chunks below
     * (we just reuse it; per-world load happens after we've extracted
     * the table from this scratch). */
    /* Pass 1: WALK the world-descriptor table at $577452 and snapshot
     * each world's chunks. The last chunk has names; the earlier chunks
     * have code / data with embedded handler pointers — we dump every
     * chunk when PC_DUMP_WORLDS=1 so the offline scanner (Pattern I) can
     * see the whole per-world picture.
     *
     * If $577452 is zero in chip RAM (overlay not loaded yet), load the
     * overlay's chunk-2 (Disk.1 $0689BE, len $012A1C) into scratch to
     * read the table from there. */
    uint32_t per_world_src[7] = {0};
    uint32_t per_world_len[7] = {0};
    /* Per-world ALL-chunks snapshot for PC_DUMP_WORLDS. Each world has
     * up to 6 chunks per the table layout. */
    #define MAX_CHUNKS 6
    uint32_t all_chunks_src[7][MAX_CHUNKS] = {{0}};
    uint32_t all_chunks_len[7][MAX_CHUNKS] = {{0}};
    int      all_chunks_n[7] = {0};
    {
        uint32_t cursor = 0x577452u;
        uint32_t cursor_end = 0x577800u;
        if (RD32(0x577452u) == 0u) {
            if (disk_boot_load(1, 0x0689BEu, scratch, 0x012A1Cu) > 0
                && atn_decrunch(scratch) != 0) {
                cursor     = scratch + (0x577452u - 0x577000u);
                cursor_end = scratch + (0x577800u - 0x577000u);
            }
        }
        for (int world = 0; world < PC_NUM_WORLDS; world++) {
            uint32_t last_src = 0, last_len = 0;
            int      cn = 0;
            while (cursor < cursor_end && RD32(cursor) != 0) {
                last_src = RD32(cursor + 4);
                last_len = RD32(cursor + 8);
                if (cn < MAX_CHUNKS) {
                    all_chunks_src[world][cn] = last_src;
                    all_chunks_len[world][cn] = last_len;
                    cn++;
                }
                cursor += 12;
            }
            cursor += 4;  /* skip the zero terminator */
            all_chunks_n[world] = cn;
            per_world_src[world] = last_src;
            per_world_len[world] = last_len;
        }
    }

    /* Optional: dump EVERY chunk of every world so the offline pointer
     * scanner can see the full per-world data. */
    if (getenv("PC_DUMP_WORLDS")) {
        for (int world = 0; world < PC_NUM_WORLDS; world++) {
            for (int ci = 0; ci < all_chunks_n[world]; ci++) {
                uint32_t src = all_chunks_src[world][ci];
                uint32_t len = all_chunks_len[world][ci];
                if (src == 0 || len == 0 || len > 0x20000u) continue;
                uint32_t off = src >> 8;
                int      disk = (int)(src & 0xFFu) + 1;
                if (disk < 1 || disk > 3) continue;
                if (disk_boot_load(disk, off, scratch, len) <= 0) continue;
                uint32_t out = atn_decrunch(scratch);
                if (out == 0) continue;
                char path[64];
                snprintf(path, sizeof path, "logs/world_%d_chunk_%d.bin", world, ci);
                FILE *f = fopen(path, "wb");
                if (f) { fwrite(g_mem + scratch, 1, out, f); fclose(f);
                         fprintf(stderr, "[world-dump] %s: %u bytes\n", path, out); }
            }
        }
    }
    #undef MAX_CHUNKS

    /* Pass 2: per-world chunk load + name extraction. */
    for (int world = 0; world < PC_NUM_WORLDS; world++) {
        uint32_t last_src = per_world_src[world];
        uint32_t last_len = per_world_len[world];
        if (last_src == 0 || last_len == 0 || last_len > 0x20000u) continue;

        /* src_encoded = (disk_offset << 8) | (zero-based disk index). */
        uint32_t disk_off = last_src >> 8;
        int      disk_num = (int)(last_src & 0xFFu) + 1;
        if (disk_num < 1 || disk_num > 3) continue;

        int rc = disk_boot_load(disk_num, disk_off, scratch, last_len);
        if (rc <= 0) continue;
        uint32_t outlen = atn_decrunch(scratch);
        if (outlen == 0) continue;
        /* (All-chunks dump for Pattern I happened earlier, before pass 2.) */
        /* Decode the world name. Stored at offset $004, Caesar-shifted by
         * +0x1A on letters (spaces pass through). The run ends at the
         * first non-letter, non-space byte. Worlds 2 and 3 have a 2-byte
         * header at $002 that LOOKS like ASCII (chars in printable range
         * but not in the cipher's letter window) — starting strictly at
         * $004 avoids picking those up as "JSTONES" / "THE  TREETOP". */
        {
            int run_len = 0;
            for (int i = 0; i < 28; i++) {
                uint8_t c = g_mem[scratch + 4 + i];
                if (c == ' ') { run_len++; continue; }
                int d = (int)c - 0x1A;
                if (d >= 'A' && d <= 'Z') { run_len++; continue; }
                break;
            }
            int j = 0;
            for (int i = 0; i < run_len && j < 31; i++) {
                uint8_t c = g_mem[scratch + 4 + i];
                g_pc_preloaded_world_names[world][j++] =
                    (c == ' ') ? ' ' : (char)((int)c - 0x1A);
            }
            g_pc_preloaded_world_names[world][j] = 0;
        }

        /* Names are stored 44 bytes apart starting at scratch + $60 (the
         * first opening-quote of slot 0 lives right at offset $60). BUT the
         * storage order is NOT the play order: each per-world chunk also
         * embeds the engine's $32 level table (a run of one 12-byte entry
         * per level: [data_off, name_off, 0]). The level CARD picks its name
         * via name_off — relocated against the name-array base $5786A8 — so
         * play-order level `liw` maps to stored slot (name_off / 44), which
         * is a non-trivial permutation. Reading sequentially instead made
         * level-select show e.g. world-1 L4/L8 and L7/L9 swapped (the engine
         * dispatcher at $5779C2 loads this name pointer into -$67ca(a5);
         * confirmed against the live $32 table for all 7 worlds).
         *
         * Locate the table by its signature: n consecutive 12-byte entries
         * whose 3rd long is 0 and whose (2nd long / 44) values form a
         * permutation of 0..n-1. Fall back to identity if not found. */
        int n = pc_levels_in_world(world);
        long tbl = -1;
        for (uint32_t o = 0; tbl < 0 && (uint64_t)o + 12u * (uint32_t)n <= outlen; o += 2) {
            int seen[16] = {0}, ok = 1;
            for (int k = 0; k < n; k++) {
                uint32_t l0 = RD32(scratch + o + (uint32_t)k * 12u);
                uint32_t l1 = RD32(scratch + o + (uint32_t)k * 12u + 4u);
                uint32_t l2 = RD32(scratch + o + (uint32_t)k * 12u + 8u);
                if (l2 != 0 || l0 == 0 || l0 >= outlen || (l1 % 44u) != 0) { ok = 0; break; }
                uint32_t s = l1 / 44u;
                if (s >= (uint32_t)n || seen[s]) { ok = 0; break; }
                seen[s] = 1;
            }
            if (ok) tbl = (long)o;
        }

        for (int liw = 0; liw < n; liw++) {
            int slot = liw;   /* identity fallback */
            if (tbl >= 0)
                slot = (int)(RD32(scratch + (uint32_t)tbl + (uint32_t)liw * 12u + 4u) / 44u);
            uint32_t entry = scratch + 0x60u + (uint32_t)slot * 44u;
            int qa = -1;
            for (int i = 0; i < 36; i++) {
                if (g_mem[entry + i] == '"') { qa = i; break; }
            }
            if (qa < 0) continue;
            int gi = (pc_world_first_level(world) - 1) + liw;
            int j = 0;
            for (int i = qa + 1; i < 44 && g_mem[entry + i] != '"' && j < 31; i++) {
                g_pc_preloaded_names[gi][j++] = (char)g_mem[entry + i];
            }
            g_pc_preloaded_names[gi][j] = 0;
        }
    }

    /* Zero the scratch area so we don't leave garbage where later disk
     * reads might pass through. */
    memset(g_mem + scratch, 0, 0x20000u);
    #undef RD32
    g_pc_preloaded_names_ready = 1;
    fprintf(stderr, "[level-names] preloaded all %d from disk overlays:\n", PC_NUM_LEVELS);
    for (int w = 0; w < PC_NUM_WORLDS; w++) {
        fprintf(stderr, "  world %d: \"%s\"\n", w, g_pc_preloaded_world_names[w]);
        for (int i = 0; i < pc_levels_in_world(w); i++) {
            int gl = pc_world_first_level(w) + i;
            fprintf(stderr, "    L%-2d (w%dl%d): \"%s\"\n",
                    gl, w, i + 1, pc_static_level_name(gl));
        }
    }
    fflush(stderr);
}

const char *pc_static_level_name(int level)
{
    if (level < 1 || level > PC_NUM_LEVELS) return "?";
    if (!g_pc_preloaded_names_ready) pc_preload_all_level_names();
    if (g_pc_preloaded_names_ready && g_pc_preloaded_names[level - 1][0])
        return g_pc_preloaded_names[level - 1];
    return "?";
}

/* Name of the level currently selected in $20.w.
 *
 * NOTE: the $5786AC name table is stored in slot order, which is NOT the
 * play order — level `liw` maps to a permuted name slot via the engine's
 * $32 level table (see pc_preload_all_level_names). Indexing $5786AC by
 * liw directly therefore mislabels e.g. world-0 L4/L8 and L7/L9. We defer
 * to the preloaded table, which already applies that permutation. */
const char *pc_current_level_name(void)
{
    extern uint8_t *g_mem;
    if (!g_mem) return "?";
    int level = ((int)g_mem[0x20] << 8) | g_mem[0x21];
    if (level < 1 || level > PC_NUM_LEVELS) return "?";
    return pc_static_level_name(level);
}

/* Banner state (cop1lc=$003914): GENERIC banner copper. Three engine paths
 * use it — title card ($5782B4), LEVEL COMPLETE banner ($578C3E),
 * GET READY banner ($578D0E) — so cop1lc alone can't tell them apart.
 *
 * The title-card path uniquely owns the wait-counter at $57FEF6: $5782B4
 * writes $258 (600 frames = 12s upper bound) on entry, then decrements it
 * every frame in its wait loop (subq.w #1, $57FEF6 at $5784AC) until it
 * hits 0 OR fire is pressed. The other two banner paths don't touch this
 * counter. So $57FEF6 > 0 means "title card is showing right now."
 *
 * pc_is_banner_displayed(): true for any of the three banners (cop1lc).
 * pc_is_title_card_displayed(): true only for the world+level title card. */
int pc_is_banner_displayed(void)
{
    extern uint32_t hw_get_cop1lc(void);
    return hw_get_cop1lc() == 0x003914u;
}

int pc_is_title_card_displayed(void)
{
    extern uint8_t *g_mem;
    if (!g_mem) return 0;
    uint16_t timer = ((uint16_t)g_mem[0x57FEF6u] << 8) | g_mem[0x57FEF7u];
    return timer > 0 && pc_is_banner_displayed();
}

/* Legacy alias — old code says "level card" but means "any banner". */
int pc_is_level_card_displayed(void) { return pc_is_banner_displayed(); }

/* Deferred save/load: SDLK_s / SDLK_d fire from inside hw_present_frame, on the
 * MAIN thread. The keys set these flags and the work happens at the next pc_step
 * boundary, where the game thread is parked at its vblank wait (its M68K context
 * quiescent). */
int g_pc_pending_save = 0;
int g_pc_pending_load = 0;

/* ── Game thread + ping-pong handoff ────────────────────────────────────────────
 * The game runs on its own OS thread; the SDL main thread paces frames. They
 * hand off cooperatively via a condvar so EXACTLY ONE runs at a time (no data
 * races on s_game_ctx/g_mem, deterministic frame-by-frame). This is the ucontext
 * coroutine re-expressed with real threads + a mutex, as the user requested.
 *
 *   game thread:  rt_call($3000) ... hw_vblank_wait() -> game_thread_yield()
 *                   [hands the turn to main, blocks until released]
 *   main thread:  pc_step_threaded(): release game one frame, wait until it
 *                   parks, then present (SDL must be on main) + deliver IRQ.
 *
 * s_turn: 1 = the game thread runs, 0 = the main thread runs (game parked). */
static uint32_t s_game_entry   = 0x003000u;   /* address the game thread runs from */
/* s_game_done lives on g_state (macro): set when the game flow returns from rt_call */

static pthread_t       s_game_thread;
static int             s_game_thread_live = 0;
static pthread_mutex_t s_hand_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_hand_cv  = PTHREAD_COND_INITIALIZER;
static int             s_turn          = 0;
static int             s_game_exit_req = 0;
static __thread int    s_is_game_thread = 0;

int pc_step_threaded(void);

/* The game thread's per-frame wait. Wired to hw_vblank_wait via g_hw_vblank_yield
 * (the same seam the old coroutine used). Hands the turn to main and blocks until
 * the host releases it. An IRQ handler that hits a wait runs on the MAIN thread
 * (call_fn) — it must NOT block here (would deadlock), so non-game threads return
 * immediately. On a restart request the parked thread exits cleanly. */
static void game_thread_yield(void)
{
    if (!s_is_game_thread) return;
    pthread_mutex_lock(&s_hand_mtx);
    s_turn = 0;                            /* hand the turn back to main */
    pthread_cond_broadcast(&s_hand_cv);
    while (s_turn == 0 && !s_game_exit_req)
        pthread_cond_wait(&s_hand_cv, &s_hand_mtx);
    int exit_req = s_game_exit_req;
    pthread_mutex_unlock(&s_hand_mtx);
    if (exit_req) pthread_exit(NULL);
}

static void *game_thread_main(void *arg)
{
    (void)arg;
    s_is_game_thread = 1;
    /* Wait for the host's first release before touching anything. */
    pthread_mutex_lock(&s_hand_mtx);
    while (s_turn == 0 && !s_game_exit_req)
        pthread_cond_wait(&s_hand_cv, &s_hand_mtx);
    int exit_req = s_game_exit_req;
    pthread_mutex_unlock(&s_hand_mtx);
    if (exit_req) return NULL;

    rt_call(&s_game_ctx, s_game_entry);    /* the whole game flow (never normally returns) */

    { extern uint32_t rt_get_last_insn(void);
      fprintf(stderr, "[game] flow RETURNED from $%06X (last insn $%06X)\n",
              s_game_entry, rt_get_last_insn()); }
    pthread_mutex_lock(&s_hand_mtx);
    s_game_done = 1;
    s_turn = 0;
    pthread_cond_broadcast(&s_hand_cv);
    pthread_mutex_unlock(&s_hand_mtx);
    return NULL;
}

/* Spawn a fresh game thread (blocked at its initial wait until the first frame
 * release). Caller has already set s_game_ctx + s_game_entry + bank flags. */
static void game_thread_spawn(void)
{
    s_game_done     = 0;
    s_game_exit_req = 0;
    s_turn          = 0;
    pthread_create(&s_game_thread, NULL, game_thread_main, NULL);
    s_game_thread_live = 1;
}

/* Cooperatively stop the running game thread (parked at a wait or its initial
 * wait) and join it. Only valid to call from the main thread with the game
 * parked (s_turn == 0). */
static void game_thread_stop(void)
{
    if (!s_game_thread_live) return;
    pthread_mutex_lock(&s_hand_mtx);
    s_game_exit_req = 1;
    s_turn = 1;                            /* wake it so it observes the exit flag */
    pthread_cond_broadcast(&s_hand_cv);
    pthread_mutex_unlock(&s_hand_mtx);
    pthread_join(s_game_thread, NULL);
    s_game_thread_live = 0;
}

/* Release the game thread to run for exactly one frame (until its next vblank
 * wait), then block until it parks (or the flow finishes). On return the game is
 * parked and the main thread owns all shared state. */
static void game_thread_run_one_frame(void)
{
    if (!s_game_thread_live) return;
    pthread_mutex_lock(&s_hand_mtx);
    s_turn = 1;
    pthread_cond_broadcast(&s_hand_cv);
    while (s_turn == 1 && !s_game_done)
        pthread_cond_wait(&s_hand_cv, &s_hand_mtx);
    pthread_mutex_unlock(&s_hand_mtx);
}

int pc_step(void)
{
    /* Service any deferred pause-menu action (Resume/Retry/ExitToMenu/Quit)
     * before anything else — we're on the MAIN thread here and the game thread
     * is parked, so it's safe to stop/respawn the game thread if needed. */
    { extern void pc_pause_tick(void); pc_pause_tick(); }

    if (g_pc_pending_load) {
        g_pc_pending_load = 0;
        pc_loadstate("logs/savestate.bin");
    }
    if (!hw_running) return 1;

    /* While paused, freeze the game thread entirely — don't release it.
     * Still call hw_present_frame so the pause overlay stays visible and
     * SDL events keep flowing (so the user can navigate the menu). */
    extern int pc_pause_active(void);
    if (pc_pause_active()) {
        if (g_harness_prerender_hook) g_harness_prerender_hook();
        if (hw_present_frame() != 0) return 1;
        if (g_harness_frame_hook) g_harness_frame_hook();
        return 0;
    }

    hw_watchdog_arm("PC", 2);          /* catch an infinite loop in one frame */
    int r = pc_step_threaded();        /* release the game thread for one frame */
    hw_watchdog_disarm();
    if (g_pc_pending_save) {
        g_pc_pending_save = 0;
        pc_savestate("logs/savestate.bin");
    }

    short ab[PC_AUD_SPF * 2];
    if (g_overlay_active || g_gameplay_active || g_credits_active) {
        /* The overlay music player (menu, level card, gameplay, credits — all the
         * same CIA-timer-driven $53A2 player) advances ~3x per displayed frame;
         * one tick/frame plays it too slow. The intro crawl uses a different
         * player ($55A0) that advances once/frame (handled in the else branch).
         * Render audio between ticks so each sub-frame note is actually heard.
         * NOTE: gameplay is g_gameplay_active (overlay=0) — it MUST be included
         * here or it gets zero music ticks (no sound past the menu). */
        int ticks = GP_MUSIC_TICKS;
        int done = 0;
        for (int k = 0; k < ticks; k++) {
            pc_music_tick();
            int chunk = (PC_AUD_SPF * (k + 1) / ticks) - done;
            hw_audio_render(ab + done * 2, chunk);
            done += chunk;
        }
    } else {
        hw_audio_render(ab, PC_AUD_SPF);
    }
    hw_audio_queue(ab, PC_AUD_SPF);
    return r;
}

/* ── Native disk boot (game-thread flow) ─────────────────────────────────────
 * Boot from the original disk images, no snapshot: decrunch Disk.1's crunched
 * main game (ATN!) into $3000, then run the recompiled cold-start ($3000) on the
 * game thread — each frame wait (hw_vblank_wait) parks it and pc_step_threaded
 * releases it one frame at a time. The game drives its real flow (intro → logos
 * → title → menu → gameplay). */
/* s_game_ctx is the M68K register file (on g_state via game_state.h). */

/* Deliver the game's level-6 CIA-B timer interrupt service routines. On real
 * hardware the timer IRQ alternates two handlers on the $78 vector:
 *   $3160 → $0055A0  — music player + animation/auto-advance frame counters.
 *   $0058C2          — copies the audio shadow ($69F6+) into the Paula regs.
 * A static recompiler can't take async interrupts, so the runtime calls the
 * service routines itself once per frame (the rate the game programmed the
 * timer for). $55A0 is the rts-terminated leaf of the $3160 wrapper; both are
 * the game's own code (no fudged values). */
/* True when the game has the given interrupt level enabled (master INTEN bit +
 * that level's source bit), i.e. the real CPU would actually take the interrupt.
 * A vector still holding the PREVIOUS screen's handler (e.g. the title's $3532 /
 * $5694 during a level load) does not fire while its level is masked — the load
 * polls the beam precisely because it has interrupts off. This replaces the old
 * "is the vector in the gameplay bank?" band-aid with the real HW condition. */
static int irq_level_enabled(uint16_t levelbits)
{
    uint16_t ie = hw_get_intena();
    return (ie & INTENA_MASTER) && (ie & levelbits);
}

/* Deliver the level-6 (CIA-B timer) music ISR once. Called pc_step's per-screen
 * number of times per frame. Gated on the interrupt being enabled so a stale
 * vector from a previous screen doesn't run during a masked load. */
void pc_music_tick(void)
{
    if (!g_overlay_active && !g_gameplay_active && !g_credits_active) return;
    if (!irq_level_enabled(INTENA_LVL6)) return;
    uint32_t v6 = ((uint32_t)g_chip[0x78] << 24) | ((uint32_t)g_chip[0x79] << 16)
                | ((uint32_t)g_chip[0x7a] << 8)  |  (uint32_t)g_chip[0x7b];
    if (v6) call_fn(&s_game_ctx, v6);
}

static void coro_deliver_timer_irq(void)
{
    if (g_gameplay_active || g_overlay_active || g_credits_active) {
        /* Gameplay / overlay / credits each install their own level-3 ($6c,
         * vblank — sets the frame's copper/COP1LC) and level-6 ($78, music/
         * timer) handlers. Fire each only when the game has that interrupt
         * level enabled, so stale vectors from the previous screen don't run
         * during the masked load. Credits sets $6c=$350A, $78=$351C; gameplay
         * sets $6c=$57825A, $78=$59BF3E etc. Either way, just deliver whatever
         * the game installed at the vector. */
        uint32_t v3 = ((uint32_t)g_chip[0x6c] << 24) | ((uint32_t)g_chip[0x6d] << 16)
                    | ((uint32_t)g_chip[0x6e] << 8)  |  (uint32_t)g_chip[0x6f];
        /* Level-3 (vblank) fires once per displayed frame here. Level-6 (the CIA-B
         * timer / music ISR) is delivered by pc_music_tick in pc_step at the
         * per-screen sub-frame rate — delivering it here too over-counted it. */
        if (v3 && irq_level_enabled(INTENA_LVL3)) call_fn(&s_game_ctx, v3);
        return;
    }
    /* Intro and the cover-art title share this address range but install
     * DIFFERENT interrupt handlers. The intro sets $78=$3160 (-> $55A0 music)
     * and copies the audio shadow via $58C2. The title installs its OWN level-3
     * ($6c=$3532) and level-6 ($78=$56C4) handlers — a different music driver
     * that lives in the title-state ("gp") bank, NOT the intro chip dump (whose
     * bytes at $56xx-$5Dxx are stale). Honor whatever the game installed: for
     * the intro vector keep the known-good intro leaves; otherwise select the
     * title-state bank (memory at the title matches the gp dump) and deliver the
     * installed vectors, exactly as PUAE's CPU does on the real autovectors. */
    uint32_t v3 = ((uint32_t)g_chip[0x6c] << 24) | ((uint32_t)g_chip[0x6d] << 16)
                | ((uint32_t)g_chip[0x6e] << 8)  |  (uint32_t)g_chip[0x6f];
    uint32_t v6 = ((uint32_t)g_chip[0x78] << 24) | ((uint32_t)g_chip[0x79] << 16)
                | ((uint32_t)g_chip[0x7a] << 8)  |  (uint32_t)g_chip[0x7b];
    extern int rt_intro_has_fn(uint32_t), rt_gp_has_fn(uint32_t);
    if (v6 && !rt_intro_has_fn(v6) && rt_gp_has_fn(v6)) {
        /* Title vector: handler lives only in the title-state bank. */
        g_overlay_active = 1;
        if (v3) call_fn(&s_game_ctx, v3);
        call_fn(&s_game_ctx, v6);
        g_overlay_active = 0;
    } else {
        /* Intro (all screens): the $3160 wrapper isn't recompiled, so call its
         * leaf music driver + the audio-shadow copy directly. */
        call_fn(&s_game_ctx, 0x0055A0u);
        call_fn(&s_game_ctx, 0x0058C2u);
    }
}

/* Common bring-up shared between the full-boot path and the direct-to-gameplay
 * shortcut: hardware + runtime + disks + boot loader + override registration.
 * Returns 0 on success, -1 on failure. */
static int pc_common_bringup(const char **disks, int n_disks)
{
    pc_state_reset_defaults();   /* zero g_state + non-zero defaults */
    if (hw_init("Benefactor (disk boot)", NULL, 0) < 0) return -1;
    if (rt_init(NULL, 0, 0x080000) < 0) return -1;     /* allocate g_mem, no chip dump */
    g_chip = g_mem;
    if (disk_boot_open(disks, n_disks) < 0) {
        fprintf(stderr, "[pc] could not open disk images\n");
        return -1;
    }
    /* Boot loader step: Load(Disk.1 @$1880, $2442E → $3000) + Decrunch($3000).
     * Shared with the bank dumper via overlay_load_main(). */
    overlay_load_main();
    pc_register_overrides();
    g_hw_vblank_yield   = game_thread_yield;  /* hw_vblank_wait parks the game thread */
    g_hw_pc_owns_present = 1;
    { extern int g_native_render_delay; const char *e = getenv("PC_RENDER_DELAY");
      g_native_render_delay = e ? atoi(e) : 1; }  /* blitter-latency model (frames) */
    return 0;
}

/* ── Recompiler bank-input dumps, from the disks alone (no PUAE/Kickstart) ──────
 * The three recompiler banks each correspond to an overlay the game loads into
 * $3000+, and every one of those loads is reimplemented in hand-written C (pure
 * disk read + ATN! decrunch + relocation): the boot decrunch (main/intro), the
 * title overlay (native_overlay_load), and the gameplay overlay
 * (native_overlay_load_d0). So we can regenerate every bank input from the
 * Disk.* images with NO emulator. Each bank is produced from a FRESH boot
 * decrunch so the overlay loads see clean $6D734 source data (mirrors the real
 * boot → overlay sequence). */
static int dump_region(const char *path, uint32_t off, uint32_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[dumpbanks] cannot open %s\n", path); return -1; }
    size_t n = fwrite(g_mem + off, 1, len, f);
    fclose(f);
    fprintf(stderr, "[dumpbanks] wrote %s (%zu bytes from $%06X)\n", path, n, off);
    return (n == len) ? 0 : -1;
}

int pc_dump_banks_from_disk(const char **disks, int n_disks, const char *out_dir)
{
    hw_request_headless();                          /* no SDL window/audio */
    if (pc_common_bringup(disks, n_disks) < 0) return -1;  /* overlay_load_main + setup */

    char p[1024];
    /* main / intro bank: the boot-decrunched $3000 image. Recompiler reads the
     * --chip-dump $3000 slice. */
    snprintf(p, sizeof p, "%s/chip_ram_dump.bin", out_dir);
    if (dump_region(p, 0, 0x80000u) < 0) return -1;

    /* gp / title bank: fresh decrunch, then the title overlay load. */
    overlay_load_main();  overlay_load_title();
    snprintf(p, sizeof p, "%s/chip_flow_gp.bin", out_dir);
    if (dump_region(p, 0, 0x80000u) < 0) return -1;

    /* gpl / gameplay bank: fresh decrunch, then the gameplay overlay load. */
    overlay_load_main();  overlay_load_gameplay();
    snprintf(p, sizeof p, "%s/gmem_after_load.bin", out_dir);
    if (dump_region(p, 0, 0x600000u) < 0) return -1;

    /* credits / end-game bank: fresh decrunch, then the credits overlay load. */
    overlay_load_main();  overlay_load_credits();
    snprintf(p, sizeof p, "%s/gmem_after_credits.bin", out_dir);
    if (dump_region(p, 0, 0x600000u) < 0) return -1;

    fprintf(stderr, "[dumpbanks] done -> %s\n", out_dir);
    return 0;
}

/* Reset to a fresh cold start ($3000): stop any running game thread, clear the
 * M68K context, and spawn a new game thread parked at its initial wait. */
static void pc_cps_reset(void)
{
    game_thread_stop();
    memset(&s_game_ctx, 0, sizeof s_game_ctx);
    s_game_entry = 0x003000u;
    game_thread_spawn();
}

/* (Re)start the flow at an explicit entry with a given register/bank setup —
 * used by exit-to-menu (title $3330) and direct-to-gameplay ($577000). Stops the
 * current game thread and spawns a fresh one parked at its initial wait. Safe to
 * call from the main thread with the game parked. */
static void pc_cps_start_at(uint32_t entry, uint32_t a5, int gameplay,
                            uint32_t d5, uint32_t d6)
{
    game_thread_stop();
    memset(&s_game_ctx, 0, sizeof s_game_ctx);
    s_game_entry = entry;
    g_overlay_active  = !gameplay;
    g_gameplay_active = gameplay;
    g_credits_active  = 0;
    s_game_ctx.A[5] = a5;
    s_game_ctx.A[6] = 0x00DFF000u;
    s_game_ctx.A[7] = 0x00080000u;
    s_game_ctx.D[5] = d5;
    s_game_ctx.D[6] = d6;
    game_thread_spawn();
}

int pc_init_from_disk(const char **disks, int n_disks)
{
    if (pc_common_bringup(disks, n_disks) < 0) return -1;
    /* The cold-start ($3000) drives the whole flow (intro → logos → title → menu
     * → gameplay) on the game thread; each frame wait (hw_vblank_wait) parks it
     * and pc_step_threaded releases it one frame at a time. */
    pc_cps_reset();
    fprintf(stderr, "[pc] disk boot: game-thread flow\n");
    return 0;
}

/* "Exit to main menu" from the pause menu — drop into the gp/title bank's
 * attract entry ($003330), which renders the poster ("cover art" screen).
 * No fire-skip hack: we land there directly because $003330 IS the poster
 * entry (the same address the engine reaches after the intro, and the same
 * one the menu's fade-back-to-attract path jumps to).
 *
 * Steps:
 *   1. Clear all g_state (bank flags, register file, shadow regs, audio).
 *   2. Reload the title overlay (native_overlay_load) — d0=1 equivalent.
 *      Required because if we're coming from gameplay/credits, the bytes
 *      at $3330+ have been overwritten with gameplay/credits code. Also
 *      replays the $6D714 block-copy so low-RAM engine state is fresh.
 *   3. Restart the continuation-stack flow at the title attract entry $003330
 *      (gp bank a5=$511E). The respawned game thread enters the poster. */
void pc_request_cold_restart(void)
{
    extern void native_overlay_load(void);
    pc_state_reset_defaults();           /* zeros g_state, resets non-zero defaults */
    native_overlay_load();               /* reload title/intro overlay + block-copy */
    pc_cps_start_at(0x00003330u, 0x0000511Eu, /*gameplay=*/0, /*d5=*/0, /*d6=*/0);
    fprintf(stderr, "[pc] exit-to-menu: flow restart at $003330 (poster)\n");
}

/* Direct-to-gameplay entry. Skips intro/title/menu entirely: the gameplay
 * overlay is loaded, $20.w is written, and the coroutine is set to enter
 * $577000 directly with the loader-handoff register state ($150 would set).
 *
 * This documents the contract the recompiled gameplay engine expects at its
 * entry point — anything in this function is what we have to keep providing
 * as we native-port more of the engine. Currently the engine itself still
 * runs (we don't own $577000+ yet); this just removes the title machinery so
 * we can drive any level immediately and compare to PUAE cleanly. */
int pc_init_to_gameplay(const char **disks, int n_disks, int level)
{
    extern uint8_t *g_mem;
    extern void native_overlay_load_d0(void);

    if (pc_common_bringup(disks, n_disks) < 0) return -1;

    /* Gameplay-engine entry contract — observed from the natural title→$150
     * path and the recompiled $577000 prologue (see gameplay_coro_entry):
     *   - g_mem must hold the loaded+decrunched gameplay overlay
     *     (base code at $3330, level engine at $577000, reloc table at $6E000).
     *   - $20.w  = level number (1..60) — read by $5779AA's level dispatcher.
     *   - $3e/$184 = $A68 (display pointers initialised by the real $150 body
     *     — without this, card glyph renderer overwrites $100 and hangs).
     *   - Disk-chunk dest-pointer table at $100/$104/$108 — built by
     *     native_overlay_load_d0().
     *   - All 60 level-name tables preloaded (pc_preload_all_level_names) so
     *     the title-card renderer can look them up without disk I/O reentry.
     *   - Coroutine register state at $577000: a5=$57EE12 (set by $577000
     *     itself), a6=$DFF000, a7=$80000 (reset on entry), d5=$1000, d6=$FFFF
     *     — see gameplay_coro_entry().
     */
    native_overlay_load_d0();

    /* $3e and $184 — both point at a sentinel ($A68) the card-renderer chases.
     * The real $150 body sets these as immediates; missing this causes the
     * card glyph blit to overwrite chunk pointers at $100 and the gameplay
     * dispatch later walks a null chain (see project_card_freeze_chain). */
    for (uint32_t a = 0x3eu; ; a = 0x184u) {
        g_mem[a] = 0x00; g_mem[a+1] = 0x00; g_mem[a+2] = 0x0A; g_mem[a+3] = 0x68;
        if (a == 0x184u) break;
    }

    extern void pc_preload_all_level_names(void);
    pc_preload_all_level_names();

    /* Pin the requested level. $20.w is the engine's level number; $5779AA
     * indexes (level-1)*4 into the 60-entry table at $57782E to pick a
     * (world, level_in_world) pair. */
    if (level < 1)  level = 1;
    if (level > 60) level = 60;
    g_mem[0x20] = 0; g_mem[0x21] = (uint8_t)level;

    /* Enter the gameplay engine at $577000 directly. $577000 does its own
     * a5/a7/a6 init; the d5=$1000/d6=$FFFF loader-handoff values come from us. */
    g_gameplay_entry = 0x00577000u;
    pc_cps_start_at(0x00577000u, 0x0057EE12u, /*gameplay=*/1, /*d5=*/0x1000u, /*d6=*/0xFFFFu);

    fprintf(stderr, "[pc] direct-to-gameplay: entering $577000 at level %d\n", level);
    return 0;
}

/* Per-frame driver. Release the game thread to run until its next vblank wait
 * (it draws this frame's content, then parks), then present the drawn frame and
 * deliver the vblank IRQ — same order as the old coroutine (present, IRQ, then
 * next frame the game resumes). Present + IRQ run on the MAIN thread while the
 * game thread is parked, so there are no races. Returns 1 to quit. */
int pc_step_threaded(void)
{
    if (s_game_done && !g_enter_gameplay) return 1;

    game_thread_run_one_frame();   /* run the game to its next vblank wait (parks) */

    if (g_harness_prerender_hook) g_harness_prerender_hook();
    if (hw_present_frame() != 0) return 1;
    if (g_harness_frame_hook) g_harness_frame_hook();

    /* The $150 loader override (from the title main loop OR its IRQ) may have set
     * g_enter_gameplay. Restart the game thread at the gameplay entry. Checked
     * before s_game_done because the loader call unwinds the title flow. */
    if (!g_enter_gameplay) {
        if (s_game_done) return 1;
        coro_deliver_timer_irq();   /* level-3 vblank ISR (music via pc_music_tick) */
    }
    if (g_enter_gameplay) {
        g_enter_gameplay = 0;
        pc_cps_start_at(g_gameplay_entry, 0x0057EE12u, /*gameplay=*/1,
                        /*d5=*/0x00001000u, /*d6=*/0x0000FFFFu);
        printf("[game] restarting game thread into gameplay $%06X\n", g_gameplay_entry);
        return 0;
    }
    return s_game_done ? 1 : 0;
}

/* Savestate: snapshot the M68K register context (g_state) + memory to a file.
 * Called between pc_step()s, where the game thread is parked at its vblank wait.
 * STOPGAP: this captures s_game_ctx + g_mem but NOT the game thread's C call
 * stack, so loading mid-flow cannot resume the thread at the exact suspended
 * point (proper fix would require serialising the call chain, same limitation
 * the coroutine/CPS variants had). It is reliable for the fresh-entry paths
 * (direct-to-gameplay / exit-to-menu) which respawn the thread at a known entry.
 * Does NOT save hw.c statics; the game rewrites the bulk of them next frame. */
/* Linux-only: re-exec self with ASLR disabled, before any other startup. Kept so
 * BSS-static addresses captured in a savestate stay stable across restarts of
 * the SAME binary. */
#if defined(__linux__)
#include <sys/personality.h>
#include <unistd.h>
void pc_pin_address_space(int argc, char **argv) {
    int p = personality(0xffffffff);
    if (p == -1 || (p & ADDR_NO_RANDOMIZE)) return;       /* already pinned */
    if (personality(p | ADDR_NO_RANDOMIZE) == -1) return; /* best-effort */
    (void)argc;
    execvp("/proc/self/exe", argv);
    /* Fall through if execvp failed — caller continues; savestate cross-
     * process may not work, but normal operation does. */
}
#else
void pc_pin_address_space(int argc, char **argv) { (void)argc; (void)argv; }
#endif

#define PC_SAVESTATE_MAGIC 0x42454E53u   /* 'BENS' */
#define PC_SAVESTATE_VER   5u   /* v5: dropped dead ucontext fields from g_state */

/* Savestate format (v5): one g_state blob + g_mem.
 *   uint32_t magic, ver
 *   uint32_t sizeof(g_state)
 *   uint32_t RT_MEM_SIZE
 *   uint64_t identity (linker addr of g_state — differs per build/binary)
 *   GameState g_state
 *   uint8_t   g_mem[RT_MEM_SIZE]
 * The identity word rejects loads from a different executable up front (the
 * game thread's C stack is not captured — see the STOPGAP note on pc_savestate). */

int pc_savestate(const char *path)
{
    extern uint8_t *g_mem;
    if (!g_mem || !path) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[pc] savestate: open %s failed\n", path); return -1; }
    uint32_t hdr[4] = { PC_SAVESTATE_MAGIC, PC_SAVESTATE_VER,
                        (uint32_t)sizeof g_state, (uint32_t)RT_MEM_SIZE };
    uint64_t ident = (uint64_t)(uintptr_t)&g_state;
    int ok = 1;
    ok &= fwrite(hdr,    sizeof hdr,     1, f) == 1;
    ok &= fwrite(&ident, sizeof ident,   1, f) == 1;
    ok &= fwrite(&g_state, sizeof g_state, 1, f) == 1;
    ok &= fwrite(g_mem,  RT_MEM_SIZE,    1, f) == 1;
    fclose(f);
    if (!ok) { fprintf(stderr, "[pc] savestate: short write\n"); return -1; }
    fprintf(stderr, "[pc] savestate -> %s (gameplay_active=%d overlay=%d)\n",
            path, g_gameplay_active, g_overlay_active);
    return 0;
}

int pc_loadstate(const char *path)
{
    extern uint8_t *g_mem;
    if (!g_mem || !path) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[pc] loadstate: open %s failed\n", path); return -1; }
    uint32_t hdr[4];
    if (fread(hdr, sizeof hdr, 1, f) != 1 ||
        hdr[0] != PC_SAVESTATE_MAGIC || hdr[1] != PC_SAVESTATE_VER ||
        hdr[2] != (uint32_t)sizeof g_state ||
        hdr[3] != (uint32_t)RT_MEM_SIZE) {
        fprintf(stderr, "[pc] loadstate: bad/incompatible header in %s\n", path);
        fclose(f); return -1;
    }
    uint64_t saved_ident = 0;
    if (fread(&saved_ident, sizeof saved_ident, 1, f) != 1) {
        fprintf(stderr, "[pc] loadstate: short read (ident)\n");
        fclose(f); return -1;
    }
    if (saved_ident != (uint64_t)(uintptr_t)&g_state) {
        extern int g_pc_force_load_identity_mismatch;
        if (!g_pc_force_load_identity_mismatch) {
            fprintf(stderr,
                "[pc] loadstate: savestate %s was written by a DIFFERENT binary\n"
                "      (saved ident=$%016lx, this binary=$%016lx). Save and load\n"
                "      must use the SAME executable (don't mix benefactor-pc and\n"
                "      benefactor-harness, and re-save after a rebuild).\n"
                "      Pass --force-load to bypass for diagnostics (game state will\n"
                "      load but coroutine resume is likely to crash since the saved\n"
                "      RIP/RSP and stack return addresses point at OLD .text).\n",
                path, (unsigned long)saved_ident,
                (unsigned long)(uintptr_t)&g_state);
            fclose(f); return -1;
        }
        fprintf(stderr, "[pc] loadstate: identity mismatch IGNORED (--force-load);"
                " expect coroutine instability.\n");
    }
    int ok = 1;
    ok &= fread(&g_state, sizeof g_state, 1, f) == 1;
    ok &= fread(g_mem,    RT_MEM_SIZE,    1, f) == 1;
    fclose(f);
    if (!ok) { fprintf(stderr, "[pc] loadstate: short read\n"); return -1; }
    /* g_state (incl. s_game_ctx) + g_mem are now loaded. The running game thread
     * (parked at its vblank wait) keeps its own C-stack position, which no longer
     * matches the loaded memory — see the STOPGAP note on pc_savestate. Callers
     * that need a clean resume respawn the thread via pc_cps_start_at. */
    fprintf(stderr, "[pc] loadstate <- %s (gameplay_active=%d overlay=%d enter=%d entry=$%06X)\n",
            path, g_gameplay_active, g_overlay_active,
            g_enter_gameplay, g_gameplay_entry);
    return 0;
}

void pc_fini(void)
{
    game_thread_stop();
    g_chip = NULL;
    rt_fini(); hw_fini();
}
