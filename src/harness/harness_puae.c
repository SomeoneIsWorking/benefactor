/* harness_puae.c – PUAE boot and frame capture
 * Extracted from harness_main.c to reduce monolithic size
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Prevent libretro VFS from redefining FILE/fprintf/fflush/etc. */
#define SKIP_STDIO_REDEFINES

#include "libretro-core.h"
#include "libretro.h"
#include "sysconfig.h"
#include "sysdeps.h"

#include "harness/harness_internal.h"
#include "harness/puae_state.h"
#include "harness/trace.h"
#include "port/project_paths.h"

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

static int state_contract_matches(const char *marker_path, const char *mount_path) {
    char expected[4096];
    int expected_length =
        snprintf(expected, sizeof expected, "benefactor-puae-scratch-state-v1\n%s\n", mount_path);
    if (expected_length < 0 || (size_t)expected_length >= sizeof expected)
        return 0;
    char actual[4096];
    FILE *marker = fopen(marker_path, "rb");
    if (!marker)
        return 0;
    size_t length = fread(actual, 1, sizeof actual, marker);
    fclose(marker);
    return length == (size_t)expected_length && memcmp(actual, expected, length) == 0;
}

static int write_state_contract(const char *marker_path, const char *mount_path) {
    char marker_text[4096];
    int marker_length = snprintf(marker_text, sizeof marker_text,
                                 "benefactor-puae-scratch-state-v1\n%s\n", mount_path);
    if (marker_length < 0 || (size_t)marker_length >= sizeof marker_text)
        return 0;
    FILE *marker = fopen(marker_path, "wb");
    if (!marker)
        return 0;
    size_t written = fwrite(marker_text, 1, (size_t)marker_length, marker);
    return fclose(marker) == 0 && written == (size_t)marker_length;
}

static int invalidate_state_contract(const char *marker_path) {
    return unlink(marker_path) == 0 || errno == ENOENT;
}

/* Sync breakpoint (defined in newcpu.c): exit the CPU run loop when about to
 * execute g_benefactor_sync_pc, after skipping g_benefactor_sync_skip prior hits. */
extern uint32_t g_benefactor_sync_pc;
extern int g_benefactor_sync_hit;
extern int g_benefactor_sync_skip;

/* $003742 (tst.w $41A2): once-per-iteration boundary, just before cop1lc is
 * selected from the double-buffer toggle. NOT $003732 — that's the wait-vblank
 * spin, hit many times per frame. Stopping here matches PC's pc_step, which
 * reads $41A2 and sets cop1lc as its first action. */
#define TITLE_LOOP_TOP 0x003742u

/* Run PUAE forward until the CPU is about to execute the title-loop top, having
 * passed it `skip` times first (skip=0 → next hit; skip=1 → one full iteration
 * from a boundary). Returns 1 if the boundary was reached. */
int puae_run_to_loop_top(int skip) {
    g_benefactor_sync_pc = TITLE_LOOP_TOP;
    g_benefactor_sync_skip = skip;
    g_benefactor_sync_hit = 0;
    for (int i = 0; i < 16 && !g_benefactor_sync_hit; i++)
        retro_run();
    g_benefactor_sync_pc = 0; /* disarm */
    return g_benefactor_sync_hit;
}

/* Run PUAE until its CPU is about to execute `pc` (after `skip` prior hits), or
 * until `max_frames` retro_run()s elapse. Returns 1 if the PC was reached.
 * Generalises puae_run_to_loop_top to any address — used to stop at the gameplay
 * engine entry ($577000) so the level word can be poked before level setup. */
int puae_run_to_pc(uint32_t pc, int skip, int max_frames) {
    g_benefactor_sync_pc = pc;
    g_benefactor_sync_skip = skip;
    g_benefactor_sync_hit = 0;
    for (int i = 0; i < max_frames && !g_benefactor_sync_hit; i++)
        retro_run();
    g_benefactor_sync_pc = 0; /* disarm */
    return g_benefactor_sync_hit;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/* PUAE boot and capture */

int run_puae_phase(const char *kick_dir, const char *whdload_path, int boot_frames, int n_frames,
                   char *chipram_out_path, int chipram_out_len, int display_only, int interactive,
                   const char *const *disk_paths, int disk_count) {
    (void)n_frames;
    (void)interactive;
    trace_reset();
    benefactor_log_write(BENEFACTOR_LOG_INFO, "harness",
                         "[harness] === PUAE phase: booting to game-state sync ===\n");
    benefactor_log_flush();

    char state_path[4096];
    char state_contract_path[4096];
    char whdload_mount_path[4096];
    if (!harness_artifacts_prepare() ||
        !pc_project_path(PC_PROJECT_PATH_HARNESS_ACTIVITY, NULL, harness_save_dir,
                         RETRO_PATH_MAX) ||
        !harness_artifact_path("puae_sync.state", state_path, sizeof state_path) ||
        !harness_artifact_path("puae_sync.contract", state_contract_path,
                               sizeof state_contract_path) ||
        !pc_project_path(PC_PROJECT_PATH_HARNESS_WHDLOAD, NULL, whdload_mount_path,
                         sizeof whdload_mount_path))
        return -1;
    snprintf(harness_system_dir, RETRO_PATH_MAX, "%s", kick_dir);
    harness_frontend_init();
    snprintf(full_path, RETRO_PATH_MAX, "%s", whdload_path);
    retro_init();

    struct retro_game_info gi = {whdload_path, NULL, 0, NULL};
    if (!retro_load_game(&gi)) {
        benefactor_log_write(BENEFACTOR_LOG_INFO, "harness", "[harness] retro_load_game failed\n");
        return -1;
    }
    if (!harness_stage_puae_disks(disk_paths, disk_count))
        return -1;

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
    /* BENEFACTOR_BOOT_SLOW=1: keep video rendering on during the boot phase so
     * the periodic boot_fb snapshots show real frames (fast-forward skips
     * rendering — snapshots come out black). */
    g_harness_fast_forward = pc_cfg_bool("boot_slow", 0) ? 0 : 1;

    FrameState tmp;
    int restored = 0;
    int force_refreeze = pc_cfg_bool("refreeze", 0);
    if (force_refreeze && !invalidate_state_contract(state_contract_path)) {
        benefactor_log_write(BENEFACTOR_LOG_ERROR, "harness",
                             "cannot invalidate the previous PUAE state contract");
        return -1;
    }

    if (!force_refreeze && state_contract_matches(state_contract_path, whdload_mount_path)) {
        /* Only states frozen with the project scratch contract are eligible.
         * Older states embed a different host mount and are deliberately ignored. */
        FILE *sf = fopen(state_path, "rb");
        if (sf) {
            fseek(sf, 0, SEEK_END);
            long sz = ftell(sf);
            fseek(sf, 0, SEEK_SET);
            void *buf = (sz > 0) ? malloc((size_t)sz) : NULL;
            if (buf && fread(buf, 1, (size_t)sz, sf) == (size_t)sz &&
                retro_unserialize(buf, (size_t)sz)) {
                restored = 1;
                benefactor_log_write(
                    BENEFACTOR_LOG_INFO, "harness",
                    "[harness]   Restored frozen PUAE sync state from %s (%ld bytes) "
                    "— deterministic reference\n",
                    state_path, sz);
            }
            free(buf);
            fclose(sf);
        }
    }

    /* retro_unserialize re-installs the original memory bank handlers, so re-wrap
     * the chip-write traces after a restore (no-op cost if already wrapped). */
    if (restored)
        puae_trace_init();

    if (!restored) {
        int sync_frame = -1;
        uint32_t last_cop1lc = 0;
        for (int f = 0; f < boot_frames; f++) {
            retro_run();

            /* Use lightweight cop1lc read — avoids CRC32-ing 512KB every frame */
            uint32_t cur_cop1lc = puae_get_cop1lc();

            /* Boot-progress heartbeat: emulated CPU PC every 250 frames, so a
             * stalled boot says WHERE it idles (ROM insert-disk loop vs DOS). */
            if ((f % 250) == 0) {
                extern uint32_t puae_get_cpu_pc(void);
                benefactor_log_write(BENEFACTOR_LOG_INFO, "harness",
                                     "[harness]   PUAE boot f%4d: cpu pc=$%08X cop1lc=$%06X\n", f,
                                     puae_get_cpu_pc(), cur_cop1lc);
            }
            /* Periodic screen snapshots — lets a stalled boot be SEEN
             * (CLI error text, requester, insert-disk screen, ...). */
            if (f > 0 && (f % 500) == 0) {
                char name[64];
                char sp[4096];
                snprintf(name, sizeof name, "boot_fb_%04d.bin", f);
                if (!harness_artifact_path(name, sp, sizeof sp))
                    return -1;
                FILE *sf2 = fopen(sp, "wb");
                if (sf2) {
                    fwrite(s_puae_fb, 4, FB_W * FB_H, sf2);
                    fclose(sf2);
                }
            }
            /* Only log on changes or at coarse intervals */
            if (cur_cop1lc != last_cop1lc) {
                benefactor_log_write(BENEFACTOR_LOG_INFO, "harness",
                                     "[harness]   PUAE boot f%4d: cop1lc=$%06X (CHANGED)\n", f + 1,
                                     cur_cop1lc);
                last_cop1lc = cur_cop1lc;
            }

            if (sync_frame < 0 && (cur_cop1lc == 0x7BC8u || cur_cop1lc == 0x86CCu)) {
                sync_frame = f;
                benefactor_log_write(
                    BENEFACTOR_LOG_INFO, "harness",
                    "[harness]   *** PUAE reached game state at boot-frame %d (cop1lc=$%06X) ***\n",
                    f, cur_cop1lc);
                break; /* Stop boot as soon as game state is reached */
            }
        }

        if (sync_frame < 0) {
            benefactor_log_write(
                BENEFACTOR_LOG_INFO, "harness",
                "[harness] WARNING: PUAE never reached game state before safety limit\n");
            /* Leave evidence of WHERE the boot stalled (splash? requester?
             * kickstart?): dump the final framebuffer for offline viewing. */
            {
                char path[4096];
                if (!harness_artifact_path("refreeze_fail_fb.bin", path, sizeof path))
                    return -1;
                FILE *ff = fopen(path, "wb");
                if (ff) {
                    fwrite(s_puae_fb, 4, FB_W * FB_H, ff);
                    fclose(ff);
                    benefactor_log_write(BENEFACTOR_LOG_INFO, "harness",
                                         "[harness]   final fb -> %s\n", path);
                }
            }
            return -1; /* Fail if boot didn't complete */
        }

        /* Freeze the full machine state at the sync point for deterministic reuse. */
        size_t need = retro_serialize_size();
        void *buf = need ? malloc(need) : NULL;
        if (buf && retro_serialize(buf, need)) {
            FILE *sf =
                invalidate_state_contract(state_contract_path) ? fopen(state_path, "wb") : NULL;
            if (sf) {
                size_t written = fwrite(buf, 1, need, sf);
                int close_result = fclose(sf);
                if (written == need && close_result == 0 &&
                    write_state_contract(state_contract_path, whdload_mount_path))
                    benefactor_log_write(BENEFACTOR_LOG_INFO, "harness",
                                         "[harness]   Froze PUAE sync state -> %s (%zu bytes)\n",
                                         state_path, need);
                else
                    benefactor_log_write(
                        BENEFACTOR_LOG_ERROR, "harness",
                        "[harness]   Could not persist complete PUAE sync state\n");
            }
        } else {
            benefactor_log_write(BENEFACTOR_LOG_INFO, "harness",
                                 "[harness]   WARNING: retro_serialize failed (size=%zu); "
                                 "PUAE reference stays non-deterministic\n",
                                 need);
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
        benefactor_log_write(BENEFACTOR_LOG_INFO, "harness",
                             "[harness] WARNING: could not reach title-loop top $%06X\n",
                             TITLE_LOOP_TOP);
    } else {
        benefactor_log_write(
            BENEFACTOR_LOG_INFO, "harness",
            "[harness]   Advanced to title-loop top $%06X (clean iteration boundary)\n",
            TITLE_LOOP_TOP);
    }

    /* Dump chip RAM NOW — at the loop boundary, before any post-sync frames.
     * The PC will load this dump and produce frame 0 from the same initial state. */
    chipram_out_path[0] = '\0';
    {
        static uint8_t s_chipram_buf[2 * 1024 * 1024];
        int bytes = puae_dump_chipram(s_chipram_buf, sizeof(s_chipram_buf));
        if (bytes > 524288)
            bytes = 524288;
        if (bytes > 0) {
            if (!harness_artifact_path("harness_puae_chipram.bin", chipram_out_path,
                                       (size_t)chipram_out_len))
                return -1;
            FILE *fp = fopen(chipram_out_path, "wb");
            if (fp) {
                fwrite(s_chipram_buf, 1, bytes, fp);
                fclose(fp);
                benefactor_log_write(BENEFACTOR_LOG_INFO, "harness",
                                     "[harness]   Dumped PUAE chip RAM to %s (at sync point)\n",
                                     chipram_out_path);
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
    benefactor_log_write(BENEFACTOR_LOG_INFO, "harness", "[harness]   PUAE frame 0: cop1lc=$%06X\n",
                         s_puae_log[0].cop1lc);

    /* ── Post-boot stepping mode ── */
    g_harness_fast_forward = 0;

    if (display_only) {
        benefactor_log_write(BENEFACTOR_LOG_INFO, "harness",
                             "[harness] --display-only mode: running PUAE forever\n");
        benefactor_log_flush();
        for (;;)
            retro_run();
    }

    benefactor_log_write(BENEFACTOR_LOG_INFO, "harness", "[harness] PUAE boot phase done\n");
    return 1;
}
