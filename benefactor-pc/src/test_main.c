/* test_main.c – Headless test harness with tracing */
#include "recomp/rt.h"
#include "recomp/hw.h"
#include "pc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

static volatile int s_running = 1;

static void sigint_handler(int sig)
{
    (void)sig;
    s_running = 0;
    hw_running = 0;
}

int main(int argc, char **argv)
{
    fprintf(stderr, "[test] MAIN ENTRY\n");
    fflush(stderr);

    if (argc < 2) {
        fprintf(stderr, "Usage: %s chip_ram_dump.bin [Disk.1] [Disk.2] [Disk.3]\n", argv[0]);
        fprintf(stderr, "Environment:\n");
        fprintf(stderr, "  BENEFACTOR_HEADLESS=1  – no SDL window\n");
        fprintf(stderr, "  RT_CALLS=1             – trace function calls\n");
        fprintf(stderr, "  RT_INSNS=1             – trace every instruction\n");
        fprintf(stderr, "  RT_WATCHDOG=N          – abort after N instructions\n");
        return 1;
    }

    const char *chip_path = argv[1];
    const char *disks[4] = {NULL};
    int nd = 0;
    for (int i = 2; i < argc && nd < 4; i++)
        disks[nd++] = argv[i];

    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    fprintf(stderr, "[test] calling pc_init...\n");
    fflush(stderr);

    if (pc_init(chip_path, disks, nd) < 0) {
        fprintf(stderr, "[test] init failed\n");
        return 1;
    }

    fprintf(stderr, "[test] starting run (headless=%s)\n",
            getenv("BENEFACTOR_HEADLESS") ? "yes" : "no");
    fflush(stderr);

    pc_run();

    fprintf(stderr, "[test] run complete (insn_count=%llu)\n",
            (unsigned long long)rt_insn_count);
    pc_fini();
    return 0;
}
