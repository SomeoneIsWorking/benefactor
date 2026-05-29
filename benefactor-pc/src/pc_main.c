/* pc_main.c – Native PC game entry point (single path: native disk boot) */
#include "pc.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

static volatile int s_running = 1;
static void handler(int sig) { (void)sig; s_running = 0; }

int main(int argc, char **argv)
{
    const char *disks[4] = {NULL};
    int nd = 0;

    /* Accept "--disk Disk.1 [Disk.2] [Disk.3]" or just "Disk.1 [..]". */
    int i = 1;
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == '-') i = 2;
    for (; i < argc && nd < 4; i++) disks[nd++] = argv[i];

    if (nd < 1) {
        fprintf(stderr,
            "Usage:\n"
            "  %s [--disk] Disk.1 [Disk.2] [Disk.3]   (boot natively from the disk images)\n",
            argv[0]);
        return 1;
    }

    signal(SIGINT, handler);
    signal(SIGTERM, handler);
    (void)s_running;

    if (pc_init_from_disk(disks, nd) < 0) { fprintf(stderr, "[pc] disk boot failed\n"); return 1; }
    pc_run();
    fprintf(stderr, "[pc] done\n");
    pc_fini();
    return 0;
}
