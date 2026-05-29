/*
 * recomp_main.c  –  Benefactor PC port entry point (recompiled path)
 *
 * Startup sequence (mirrors what the WHDLoad slave did under emulation):
 *   1. Load the pre-extracted game binary into g_mem at $003000
 *   2. Set up the CPU context (SP, A5=base, etc.) matching what the slave set
 *   3. Jump to the recompiled game entry function gfn_003000 (or whichever
 *      entry point the recompiler was given)
 *   4. Inside the game loop, hardware register writes hit hw.c → SDL2
 *   5. When the game polls VPOSR for vblank, hw_present_frame() is called
 *
 * Build:
 *   cmake --build build --parallel 4
 *
 * Run:
 *   ./benefactor-pc  chip_ram_dump.bin  Disk.1 Disk.2 Disk.3
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "recomp/rt.h"
#include "recomp/hw.h"
#include "generated/game.h"      /* gfn_XXXXXX declarations + dispatch table */
#include "engine.h"

static volatile int s_running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    s_running = 0;
}

/* Number of entries in the dispatch table (defined in game.c) */
const int g_fn_count = GAME_FN_COUNT;

/* ─────────────────────────────────────────────────────────────────────────── */
/* Helpers                                                                       */
/* ─────────────────────────────────────────────────────────────────────────── */

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s <chip_ram_dump.bin> <Disk.1> [Disk.2] [Disk.3]\n"
        "\n"
        "  chip_ram_dump.bin  PUAE chip RAM dump\n"
        "  Disk.N           WHDLoad pre-processed disk image files\n",
        argv0);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Main                                                                          */
/* ─────────────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }

    const char *binary_path = argv[1];
    const char *disk_paths[4] = {NULL};
    int n_disks = 0;
    for (int i = 2; i < argc && n_disks < 4; i++)
        disk_paths[n_disks++] = argv[i];

    /* ── Initialise SDL2 hardware layer ── */
    if (hw_init("Benefactor (1994)", disk_paths, n_disks) < 0) {
        fprintf(stderr, "[main] hw_init failed\n");
        return 1;
    }

    /* ── Load chip RAM state ──
     * argv[1] is a PUAE chip RAM dump.
     */
#define GAME_LOAD_ADDR  0x000000u
#define STACK_TOP       0x080000u

    const char *chip_ram_path = "<repo>/chip_ram_dump.bin";
    if (rt_init(chip_ram_path, GAME_LOAD_ADDR, STACK_TOP) < 0) {
        fprintf(stderr, "[main] rt_init failed\n");
        hw_fini();
        return 1;
    }

    /* ── Initial CPU state (mirrors WHDLoad slave setup) ──
     *
     * A5 = base of WHDLoad "resident" data structure ($76000 area).
     * We point it to a zeroed area in fast RAM; the game will store
     * its data there (e.g. high scores, copper lists).
     *
     * SP  = $07FFF0  (top of chip RAM minus 16 bytes for safety)
     * A5  = $076000  (WHDLoad resident base – game uses offsets from here)
     */
    M68KCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.A[7] = STACK_TOP - 16;   /* SP */
    ctx.A[5] = 0x076000u;        /* WHDLoad resbase (game uses $xxx(A5) for config) */

    /* Zero-initialise the WHDLoad data area so the game finds sane defaults */
    memset(g_mem + 0x076000, 0, 0x2000);

    /* Patch: write the magic signature the game expects at $61FBC
     * (gfn_00531C checks for 'SNT!' = 0x534E5421). Without this the
     * init function returns early and the game hangs. */
    {
        g_mem[0x61FBC + 0] = 0x53;
        g_mem[0x61FBC + 1] = 0x4E;
        g_mem[0x61FBC + 2] = 0x54;
        g_mem[0x61FBC + 3] = 0x21;
    }

    /* Signal handlers for graceful exit */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Watchdog for testing – disabled by default, use BENEFACTOR_LIMIT env to set */

    /* ── Test mode (env vars) ───────────────────────────────────────────── */
    /* BENEFACTOR_TEST=<frames>       – run N frames then dump/exit        */
    /* BENEFACTOR_PRESS=<frame>       – press fire at this frame           */
    /* BENEFACTOR_DUMP=ADDR1:SZ1,... – hex dump these memory regions      */
    const char *test_env = getenv("BENEFACTOR_TEST");
    const char *press_env = getenv("BENEFACTOR_PRESS");
    const char *dump_env = getenv("BENEFACTOR_DUMP");
    int test_frames = test_env ? atoi(test_env) : 0;
    int press_frame = press_env ? atoi(press_env) : -1;
    int frame_count = 0;

    /* ── Run the game ──
     *
     * The recompiler was given entry point $003000 (start of game binary).
     * The game initialises itself and then loops forever reading hardware.
     * VPOSR reads in hw.c advance synthetic beam data and trigger
     * hw_present_frame() once per 50 Hz cycle.
     */
    engine_install_overrides();

    fprintf(stderr, "[main] Entering native engine boot at $003000\n");
    engine_run(&ctx);

    fprintf(stderr, "[main] Game exited\n");

    /* Should not return, but clean up just in case */
    rt_fini();
    hw_fini();
    return 0;
}
