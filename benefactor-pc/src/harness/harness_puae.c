/* harness_puae.c – PUAE boot and frame capture
 * Extracted from harness_main.c to reduce monolithic size
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* Prevent libretro VFS from redefining FILE/fprintf/fflush/etc. */
#define SKIP_STDIO_REDEFINES

#include "sysconfig.h"
#include "sysdeps.h"
#include "libretro-core.h"
#include "libretro.h"

#include "harness/puae_state.h"
#include "harness/trace.h"
#include "harness/harness_internal.h"

/* PUAE trace init */
extern void puae_trace_init(void);

/* Frontend */
extern int g_harness_fast_forward;
extern char full_path[RETRO_PATH_MAX];
extern char harness_system_dir[RETRO_PATH_MAX];
extern char harness_save_dir[RETRO_PATH_MAX];
extern void harness_frontend_init(void);
extern void harness_combined_init(void);
extern void harness_combined_present(void);
extern void retro_run(void);
extern size_t retro_serialize_size(void);
extern bool retro_serialize(void *data, size_t size);
extern bool retro_unserialize(const void *data, size_t size);
extern uint32_t s_puae_fb[FB_W * FB_H];

/* Sync breakpoint (defined in newcpu.c): exit the CPU run loop when about to
 * execute g_benefactor_sync_pc, after skipping g_benefactor_sync_skip prior hits. */
extern uint32_t g_benefactor_sync_pc;
extern int      g_benefactor_sync_hit;
extern int      g_benefactor_sync_skip;

/* $003742 (tst.w $41A2): once-per-iteration boundary, just before cop1lc is
 * selected from the double-buffer toggle. NOT $003732 — that's the wait-vblank
 * spin, hit many times per frame. Stopping here matches PC's pc_step, which
 * reads $41A2 and sets cop1lc as its first action. */
#define TITLE_LOOP_TOP 0x003742u

/* Run PUAE forward until the CPU is about to execute the title-loop top, having
 * passed it `skip` times first (skip=0 → next hit; skip=1 → one full iteration
 * from a boundary). Returns 1 if the boundary was reached. */
int puae_run_to_loop_top(int skip)
{
    g_benefactor_sync_pc   = TITLE_LOOP_TOP;
    g_benefactor_sync_skip = skip;
    g_benefactor_sync_hit  = 0;
    for (int i = 0; i < 16 && !g_benefactor_sync_hit; i++)
        retro_run();
    g_benefactor_sync_pc = 0;   /* disarm */
    return g_benefactor_sync_hit;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/* PUAE boot and capture */

int run_puae_phase(const char *kick_dir, const char *whdload_path,
                   int boot_frames, int n_frames,
                   char *chipram_out_path, int chipram_out_len,
                   int display_only, int interactive)
{
    (void)n_frames;
    (void)interactive;
    trace_reset();
    printf("[harness] === PUAE phase: booting to game-state sync ===\n");
    fflush(stdout);

    snprintf(harness_system_dir, RETRO_PATH_MAX, "%s", kick_dir);
    snprintf(harness_save_dir,   RETRO_PATH_MAX, "/tmp");
    harness_frontend_init();
    snprintf(full_path, RETRO_PATH_MAX, "%s", whdload_path);
    retro_init();

    struct retro_game_info gi = { whdload_path, NULL, 0, NULL };
    if (!retro_load_game(&gi)) {
        GLOBAL_LOG( "[harness] retro_load_game failed\n");
        return -1;
    }

    puae_trace_init();
    extern bool libretro_runloop_active;
    libretro_runloop_active = true;

    /* ── Boot phase ──────────────────────────────────────────────────────────
     * A live boot is NON-DETERMINISTIC: the disk-load wait completes ~1 emulated
     * frame sooner/later run-to-run (host I/O timing), so PUAE's non-chip state
     * (CPU regs, CIA timers, video beam) at the sync point differs each run and
     * leaks into the comparison. The chip RAM is identical, but that hidden state
     * is not. To make the PUAE reference deterministic we FREEZE a full
     * save-state at the sync point once, then RESTORE it every subsequent run.
     * Set BENEFACTOR_REFREEZE=1 to force a fresh boot+freeze (e.g. after the
     * disk image or core changes). */
    g_harness_fast_forward = 1;

    FrameState tmp;
    const char *state_path = "logs/puae_sync.state";
    int restored = 0;

    if (!getenv("BENEFACTOR_REFREEZE")) {
        FILE *sf = fopen(state_path, "rb");
        if (sf) {
            fseek(sf, 0, SEEK_END);
            long sz = ftell(sf);
            fseek(sf, 0, SEEK_SET);
            void *buf = (sz > 0) ? malloc((size_t)sz) : NULL;
            if (buf && fread(buf, 1, (size_t)sz, sf) == (size_t)sz &&
                retro_unserialize(buf, (size_t)sz)) {
                restored = 1;
                printf("[harness]   Restored frozen PUAE sync state from %s (%ld bytes) "
                       "— deterministic reference\n", state_path, sz);
            }
            free(buf);
            fclose(sf);
        }
    }

    /* retro_unserialize re-installs the original memory bank handlers, so re-wrap
     * the chip-write traces after a restore (no-op cost if already wrapped). */
    if (restored) puae_trace_init();

    if (!restored) {
        int sync_frame = -1;
        uint32_t last_cop1lc = 0;
        for (int f = 0; f < boot_frames; f++) {
            retro_run();

            /* Use lightweight cop1lc read — avoids CRC32-ing 512KB every frame */
            uint32_t cur_cop1lc = puae_get_cop1lc();

            /* Only log on changes or at coarse intervals */
            if (cur_cop1lc != last_cop1lc) {
                printf("[harness]   PUAE boot f%4d: cop1lc=$%06X (CHANGED)\n", f + 1, cur_cop1lc);
                last_cop1lc = cur_cop1lc;
            }

            if (sync_frame < 0 && (cur_cop1lc == 0x7BC8u || cur_cop1lc == 0x86CCu)) {
                sync_frame = f;
                printf("[harness]   *** PUAE reached game state at boot-frame %d (cop1lc=$%06X) ***\n",
                       f, cur_cop1lc);
                break;  /* Stop boot as soon as game state is reached */
            }
        }

        if (sync_frame < 0) {
            printf("[harness] WARNING: PUAE never reached game state before safety limit\n");
            return -1;  /* Fail if boot didn't complete */
        }

        /* Freeze the full machine state at the sync point for deterministic reuse. */
        size_t need = retro_serialize_size();
        void *buf = need ? malloc(need) : NULL;
        if (buf && retro_serialize(buf, need)) {
            FILE *sf = fopen(state_path, "wb");
            if (sf) {
                fwrite(buf, 1, need, sf);
                fclose(sf);
                printf("[harness]   Froze PUAE sync state -> %s (%zu bytes)\n", state_path, need);
            }
        } else {
            printf("[harness]   WARNING: retro_serialize failed (size=%zu); "
                   "PUAE reference stays non-deterministic\n", need);
        }
        free(buf);
    }

    /* Advance to the title-loop TOP ($003732) before dumping. The cop1lc sync
     * fires mid-iteration; if we dump there, PC starts a fresh full iteration
     * while PUAE only finishes a partial one (the trace showed PUAE running the
     * loop 3x in "frame 0" vs PC's 1x), permanently offsetting the double-buffer
     * parity. Dumping at the loop top makes both sides begin an identical full
     * iteration, so they step in lockstep. */
    if (!puae_run_to_loop_top(0)) {
        printf("[harness] WARNING: could not reach title-loop top $%06X\n", TITLE_LOOP_TOP);
    } else {
        printf("[harness]   Advanced to title-loop top $%06X (clean iteration boundary)\n",
               TITLE_LOOP_TOP);
    }

    /* Dump chip RAM NOW — at the loop boundary, before any post-sync frames.
     * The PC will load this dump and produce frame 0 from the same initial state. */
    chipram_out_path[0] = '\0';
    {
        static uint8_t s_chipram_buf[2 * 1024 * 1024];
        int bytes = puae_dump_chipram(s_chipram_buf, sizeof(s_chipram_buf));
        if (bytes > 524288) bytes = 524288;
        if (bytes > 0) {
            snprintf(chipram_out_path, chipram_out_len, "logs/harness_puae_chipram.bin");
            FILE *fp = fopen(chipram_out_path, "wb");
            if (fp) {
                fwrite(s_chipram_buf, 1, bytes, fp);
                fclose(fp);
                printf("[harness]   Dumped PUAE chip RAM to %s (at sync point)\n", chipram_out_path);
                memcpy(s_puae_chipram_snap, s_chipram_buf,
                       bytes < CHIP_RAM_SIZE ? (size_t)bytes : CHIP_RAM_SIZE);
                s_puae_chipram_valid = 1;
            }
        }
    }

    /* Run one post-sync frame from the loop boundary — this is PUAE frame 0 that
     * the PC must match. Starting at the boundary, retro_run does exactly one
     * iteration (vs 3 from the mid-iteration sync point), aligning with PC. */
    retro_run();
    puae_snap_state(&tmp);
    s_puae_log[0] = tmp;
    s_puae_log[0].frame = 0;
    s_puae_fb_count = 0;
    if (MAX_FB_FRAMES > 0) {
        memcpy(s_puae_fb_log[0], s_puae_fb, FB_W * FB_H * sizeof(uint32_t));
        s_puae_fb_count = 1;
    }
    printf("[harness]   PUAE frame 0: cop1lc=$%06X\n", s_puae_log[0].cop1lc);

    /* ── Post-boot stepping mode ── */
    g_harness_fast_forward = 0;

    if (display_only) {
        printf("[harness] --display-only mode: running PUAE forever\n");
        fflush(stdout);
        for (;;) retro_run();
    }

    printf("[harness] PUAE boot phase done\n");
    return 1;
}
