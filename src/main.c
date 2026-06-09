/* pc_main.c – Native PC game entry point (single path: native disk boot) */
#include "port/port.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

static volatile int s_running = 1;
/* SIGINT/SIGTERM: exit promptly. The old handler only set s_running, which nothing
 * checked, so the process ignored TERM (needed kill -9). _exit is async-signal-safe
 * and guarantees the process actually dies. */
static void handler(int sig) { (void)sig; s_running = 0; _exit(0); }

int main(int argc, char **argv)
{
    extern void pc_pin_address_space(int, char **);
    pc_pin_address_space(argc, argv);

    const char *disks[4] = {NULL};
    int nd = 0;
    int direct_level = 0;
    const char *load_path = NULL;
    const char *dump_banks_dir = NULL;
    int headless = 0;

    /* Accept "--disk Disk.1 [Disk.2] [Disk.3]" or just "Disk.1 [..]".
     * "--level N" skips intro/title/menu and starts directly at level N.
     * "--load <path>" loads a savestate immediately after init (replaces the
     * full intro/title boot; the game resumes at the saved coroutine yield). */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--disk")) continue;
        if (!strcmp(argv[i], "--level") && i + 1 < argc) {
            direct_level = atoi(argv[++i]);
            continue;
        }
        if (!strcmp(argv[i], "--load") && i + 1 < argc) {
            load_path = argv[++i];
            continue;
        }
        if (!strcmp(argv[i], "--headless")) {
            headless = 1;
            continue;
        }
        if (!strcmp(argv[i], "--dump-banks") && i + 1 < argc) {
            dump_banks_dir = argv[++i];
            continue;
        }
        if (!strcmp(argv[i], "--force-load")) {
            extern int g_pc_force_load_identity_mismatch;
            g_pc_force_load_identity_mismatch = 1;
            continue;
        }
        if (nd < 4) disks[nd++] = argv[i];
    }

    if (nd < 1) {
        fprintf(stderr,
            "Usage:\n"
            "  %s [--disk] Disk.1 [Disk.2] [Disk.3] [--level N] [--load path]\n"
            "     N = 1..60: skip intro/title/menu and start directly at that level.\n"
            "     --load: resume from a savestate immediately after init.\n",
            argv[0]);
        return 1;
    }

    signal(SIGINT, handler);
    signal(SIGTERM, handler);
    (void)s_running;

    if (dump_banks_dir) {
        extern int pc_dump_banks_from_disk(const char **, int, const char *);
        int rc = pc_dump_banks_from_disk(disks, nd, dump_banks_dir);
        return rc < 0 ? 1 : 0;
    }

    if (headless) {
        extern void hw_request_headless(void);
        hw_request_headless();
    }
    int init_rc = direct_level > 0
        ? pc_init_to_gameplay(disks, nd, direct_level)
        : pc_init_from_disk(disks, nd);
    if (init_rc < 0) { fprintf(stderr, "[pc] init failed\n"); return 1; }

    if (load_path) {
        if (pc_loadstate(load_path) < 0) {
            fprintf(stderr, "[pc] --load %s failed\n", load_path);
            return 1;
        }
        fprintf(stderr, "[pc] resuming from savestate %s\n", load_path);
    }
    pc_http_debug_start();   /* no-op unless BENEFACTOR_HTTP=<port> is set */
    pc_run();
    fprintf(stderr, "[pc] done\n");
    pc_fini();
    return 0;
}
