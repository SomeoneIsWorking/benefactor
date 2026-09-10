#include "port/level_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/game_state.h"
#include "common/log.h"
#include "engine/disk_boot.h"
#include "engine/hw.h"
#include "port/config.h"
#include "port/port.h"
#include "port/port_internal.h"
#include "runtime/guest_runtime.h"

/* ── Level / world layout — the ONE place the geometry is defined ────────────
 * Mirrors the engine's level table at $57782E (worlds 0-1 have 9 levels,
 * 2-5 have 10, 6 has 2). We compute directly rather than reading $57782E
 * because that region is only populated in chip RAM after the gameplay
 * overlay loads; before that it's zeros, which would map every level to
 * (world 0, liw 0). The table is fixed game data, so the mirror can't
 * diverge. Everything else derives from pc_levels_in_world(). */
static int s_extra_worlds = -1; /* set by pc_preload_all_level_names */
int pc_extra_worlds_available(void) {
    if (s_extra_worlds < 0)
        pc_preload_all_level_names();
    return s_extra_worlds < 0 ? 0 : s_extra_worlds;
}
int pc_num_worlds_ui(void) { return PC_NUM_WORLDS + pc_extra_worlds_available(); }
int pc_num_levels_ui(void) { return PC_NUM_LEVELS + 5 * pc_extra_worlds_available(); }

int pc_levels_in_world(int world) {
    static const int wcount[PC_NUM_WORLDS] = {9, 9, 10, 10, 10, 10, 2};
    if (world >= PC_NUM_WORLDS && world < pc_num_worlds_ui())
        return 5; /* Disk.4 extras */
    if (world < 0 || world >= PC_NUM_WORLDS)
        return 0;
    return wcount[world];
}

int pc_world_first_level(int world) {
    if (world < 0 || world >= pc_num_worlds_ui())
        return 0;
    int g = 1;
    for (int w = 0; w < world; w++)
        g += pc_levels_in_world(w);
    return g;
}

/* Map global level (1..60) -> (world, level_in_world). ALWAYS use this
 * rather than divmod-by-10 — the per-world counts are irregular. */
void pc_level_split(int level, int *world_out, int *level_in_world_out) {
    int world = 0, liw = 0;
    if (level >= 1 && level <= pc_num_levels_ui()) {
        int n = level - 1;
        for (int w = 0; w < pc_num_worlds_ui(); w++) {
            int c = pc_levels_in_world(w);
            if (n < c) {
                world = w;
                liw = n;
                break;
            }
            n -= c;
        }
    }
    if (world_out)
        *world_out = world;
    if (level_in_world_out)
        *level_in_world_out = liw;
}

/* World names — preloaded from disk alongside the level names. They live
 * in each world's last chunk near the start, Caesar-shifted by +0x1A on
 * letters (spaces pass through unchanged). pc_preload_all_level_names()
 * decodes + caches them. */
static char g_pc_preloaded_world_names[PC_NUM_WORLDS + PC_EXTRA_WORLDS][32];
static int g_pc_preloaded_names_ready; /* defined below */
const char *pc_world_name(int world) {
    if (world < 0 || world >= PC_NUM_WORLDS + PC_EXTRA_WORLDS)
        return "?";
    if (!g_pc_preloaded_names_ready)
        pc_preload_all_level_names();
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
static char g_pc_preloaded_names[PC_MAX_LEVELS][32];
/* g_pc_preloaded_names_ready forward-declared above (used by pc_world_name). */

void pc_preload_all_level_names(void) {

    static int in_progress; /* extras accessors call back into us */
    if (g_pc_preloaded_names_ready || in_progress)
        return;
    if (!g_mem)
        return;
    in_progress = 1;
    s_extra_worlds = 0;
#define PRELOAD_WORLDS (PC_NUM_WORLDS + PC_EXTRA_WORLDS)

    /* Per-world level counts / global offsets come from the SSoT accessors
     * (pc_levels_in_world / pc_world_first_level) — never re-hardcode them. */
    /* Scratch buffer for decompression — picked to stay clear of every
     * dest used by the gameplay overlay ($577000-$589A1C) and the per-level
     * disk reads ($073880, $5AC4EA+, $5BE77E+, $5C4874+, $5D902A+, $5F1A88+).
     * g_mem is 8MB, so $700000 sits well past all live regions. */
    const uint32_t scratch = 0x700000u;

#define RD32(a)                                                                                    \
    (((uint32_t)g_mem[(a)] << 24) | ((uint32_t)g_mem[(a) + 1] << 16) |                             \
     ((uint32_t)g_mem[(a) + 2] << 8) | (uint32_t)g_mem[(a) + 3])

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
     * chunk when BENEFACTOR_DUMP_WORLDS=1 so the offline scanner (Pattern I) can
     * see the whole per-world picture.
     *
     * If $577452 is zero in chip RAM (overlay not loaded yet), load the
     * overlay's chunk-2 (Disk.1 $0689BE, len $012A1C) into scratch to
     * read the table from there. */
    uint32_t per_world_src[PRELOAD_WORLDS] = {0};
    uint32_t per_world_len[PRELOAD_WORLDS] = {0};
/* Per-world ALL-chunks snapshot for BENEFACTOR_DUMP_WORLDS. Each world has
 * up to 6 chunks per the table layout. */
#define MAX_CHUNKS 6
    uint32_t all_chunks_src[PRELOAD_WORLDS][MAX_CHUNKS] = {{0}};
    uint32_t all_chunks_len[PRELOAD_WORLDS][MAX_CHUNKS] = {{0}};
    int all_chunks_n[PRELOAD_WORLDS] = {0};
    {
        uint32_t cursor = 0x577452u;
        uint32_t cursor_end = 0x577800u;
        if (RD32(0x577452u) == 0u) {
            if (disk_boot_load(1, 0x0689BEu, scratch, 0x012A1Cu) > 0 &&
                atn_decrunch(scratch) != 0) {
                cursor = scratch + (0x577452u - 0x577000u);
                cursor_end = scratch + (0x577800u - 0x577000u);
            }
        }
        for (int world = 0; world < PRELOAD_WORLDS; world++) {
            uint32_t last_src = 0, last_len = 0;
            int cn = 0;
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
            cursor += 4; /* skip the zero terminator */
            all_chunks_n[world] = cn;
            per_world_src[world] = last_src;
            per_world_len[world] = last_len;
        }
    }

    /* Optional: dump EVERY chunk of every world so the offline pointer
     * scanner can see the full per-world data. */
    if (pc_cfg_bool("dump_worlds", 0)) {
        for (int world = 0; world < PC_NUM_WORLDS; world++) {
            for (int ci = 0; ci < all_chunks_n[world]; ci++) {
                uint32_t src = all_chunks_src[world][ci];
                uint32_t len = all_chunks_len[world][ci];
                if (src == 0 || len == 0 || len > 0x20000u)
                    continue;
                uint32_t off = src >> 8;
                int disk = (int)(src & 0xFFu) + 1;
                if (disk < 1 || disk > 3)
                    continue;
                if (disk_boot_load(disk, off, scratch, len) <= 0)
                    continue;
                uint32_t out = atn_decrunch(scratch);
                if (out == 0)
                    continue;
                char path[64];
                snprintf(path, sizeof path, "logs/world_%d_chunk_%d.bin", world, ci);
                FILE *f = fopen(path, "wb");
                if (f) {
                    fwrite(g_mem + scratch, 1, out, f);
                    fclose(f);
                    benefactor_log_write(BENEFACTOR_LOG_INFO, "game", "[world-dump] %s: %u bytes\n",
                                         path, out);
                }
            }
        }
    }
#undef MAX_CHUNKS

    /* Pass 2: per-world chunk load + name extraction. Worlds 7..12 are the
     * Disk.4 EXTRA slots: availability = the chunk loads AND carries a valid
     * crunch magic (an absent/short disk yields zeros -> no magic). Extras
     * must be contiguous from slot 7 (the table is laid out that way). */
    for (int world = 0; world < PRELOAD_WORLDS; world++) {
        uint32_t last_src = per_world_src[world];
        uint32_t last_len = per_world_len[world];
        if (last_src == 0 || last_len == 0 || last_len > 0x20000u)
            continue;

        /* src_encoded = (disk_offset << 8) | (zero-based disk index). */
        uint32_t disk_off = last_src >> 8;
        int disk_num = (int)(last_src & 0xFFu) + 1;
        if (disk_num < 1 || disk_num > 4)
            continue;

        int rc = disk_boot_load(disk_num, disk_off, scratch, last_len);
        if (rc <= 0)
            continue;
        uint32_t outlen = atn_decrunch(scratch);
        if (outlen == 0)
            continue;
        if (world >= PC_NUM_WORLDS) {
            if (world - PC_NUM_WORLDS != s_extra_worlds)
                continue; /* keep contiguous */
            s_extra_worlds = world - PC_NUM_WORLDS + 1;
        }
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
                if (c == ' ') {
                    run_len++;
                    continue;
                }
                int d = (int)c - 0x1A;
                if (d >= 'A' && d <= 'Z') {
                    run_len++;
                    continue;
                }
                break;
            }
            int j = 0;
            for (int i = 0; i < run_len && j < 31; i++) {
                uint8_t c = g_mem[scratch + 4 + i];
                g_pc_preloaded_world_names[world][j++] = (c == ' ') ? ' ' : (char)((int)c - 0x1A);
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
        int n = (world >= PC_NUM_WORLDS) ? 5 : pc_levels_in_world(world);
        long tbl = -1;
        for (uint32_t o = 0; tbl < 0 && (uint64_t)o + 12u * (uint32_t)n <= outlen; o += 2) {
            int seen[16] = {0}, ok = 1;
            for (int k = 0; k < n; k++) {
                uint32_t l0 = RD32(scratch + o + (uint32_t)k * 12u);
                uint32_t l1 = RD32(scratch + o + (uint32_t)k * 12u + 4u);
                uint32_t l2 = RD32(scratch + o + (uint32_t)k * 12u + 8u);
                if (l2 != 0 || l0 == 0 || l0 >= outlen || (l1 % 44u) != 0) {
                    ok = 0;
                    break;
                }
                uint32_t s = l1 / 44u;
                if (s >= (uint32_t)n || seen[s]) {
                    ok = 0;
                    break;
                }
                seen[s] = 1;
            }
            if (ok)
                tbl = (long)o;
        }

        for (int liw = 0; liw < n; liw++) {
            int slot = liw; /* identity fallback */
            if (tbl >= 0)
                slot = (int)(RD32(scratch + (uint32_t)tbl + (uint32_t)liw * 12u + 4u) / 44u);
            uint32_t entry = scratch + 0x60u + (uint32_t)slot * 44u;
            int qa = -1;
            for (int i = 0; i < 36; i++) {
                if (g_mem[entry + i] == '"') {
                    qa = i;
                    break;
                }
            }
            if (qa < 0)
                continue;
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
#undef PRELOAD_WORLDS
    in_progress = 0;
    g_pc_preloaded_names_ready = 1;
    benefactor_log_write(BENEFACTOR_LOG_INFO, "game",
                         "[level-names] preloaded from disk overlays"
                         " (%d extra world(s) on Disk.4):\n",
                         s_extra_worlds);
    for (int w = 0; w < pc_num_worlds_ui(); w++) {
        benefactor_log_write(BENEFACTOR_LOG_INFO, "game", "  world %d: \"%s\"\n", w,
                             g_pc_preloaded_world_names[w]);
        for (int i = 0; i < pc_levels_in_world(w); i++) {
            int gl = pc_world_first_level(w) + i;
            benefactor_log_write(BENEFACTOR_LOG_INFO, "game", "    L%-2d (w%dl%d): \"%s\"\n", gl, w,
                                 i + 1, pc_static_level_name(gl));
        }
    }
    benefactor_log_flush();
}

const char *pc_static_level_name(int level) {
    if (level < 1 || level > PC_MAX_LEVELS)
        return "?";
    if (!g_pc_preloaded_names_ready)
        pc_preload_all_level_names();
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
const char *pc_current_level_name(void) {
    if (!g_mem)
        return "?";
    int level = ((int)g_mem[0x20] << 8) | g_mem[0x21];
    if (level < 1 || level > PC_NUM_LEVELS)
        return "?";
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
int pc_is_banner_displayed(void) { return hw_get_cop1lc() == 0x003914u; }

int pc_is_title_card_displayed(void) {
    if (!g_mem)
        return 0;
    uint16_t timer = ((uint16_t)g_mem[0x57FEF6u] << 8) | g_mem[0x57FEF7u];
    return timer > 0 && pc_is_banner_displayed();
}

/* Legacy alias — old code says "level card" but means "any banner". */
int pc_is_level_card_displayed(void) { return pc_is_banner_displayed(); }
