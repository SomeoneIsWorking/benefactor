/* main.c – Native PC game entry point (single path: native disk boot) */
#include "common/log.h"
#include "port/port.h"
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef BENEFACTOR_ANDROID
#include "platform/android_bridge.h"
#include <SDL.h>
#endif

/* Headless Vulkan self-test (no window/disks): render a gradient through the
 * offscreen Vulkan pipeline and compare the readback to the input. Proves the
 * GPU present path works with the display off. Returns process exit code. */
static int run_vk_selftest(void) {
#ifdef BENEFACTOR_HAVE_VULKAN
    extern int present_vulkan_selftest(const uint32_t *argb, int w, int h);
    int w = 480, h = 282;
    uint32_t *img = malloc((size_t)w * h * 4);
    if (!img)
        return 1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            img[y * w + x] = 0xFF000000u | ((uint32_t)(x * 255 / w) << 16) |
                             ((uint32_t)(y * 255 / h) << 8) | (uint32_t)((x ^ y) & 0xFF);
    int d = present_vulkan_selftest(img, w, h);
    free(img);
    if (d < 0) {
        benefactor_log_write(BENEFACTOR_LOG_ERROR, "vulkan-selftest", "Vulkan error");
        return 1;
    }
    benefactor_log_write(BENEFACTOR_LOG_INFO, "vulkan-selftest", "max channel diff = %d -> %s", d,
                         d <= 1 ? "PASS" : "FAIL");
    return d <= 1 ? 0 : 1;
#else
    benefactor_log_write(BENEFACTOR_LOG_ERROR, "vulkan-selftest",
                         "this build has no Vulkan (-DBENEFACTOR_HAVE_VULKAN off)");
    return 1;
#endif
}

static volatile int s_running = 1;
/* SIGINT/SIGTERM: exit promptly. The old handler only set s_running, which
 * nothing checked, so the process ignored TERM (needed kill -9). _exit is
 * async-signal-safe and guarantees the process actually dies. */
static void handler(int sig) {
    (void)sig;
    s_running = 0;
    _exit(0);
}

int main(int argc, char **argv) {
    const char *disks[4] = {NULL};
    int nd = 0;
    int direct_level = 0;
    const char *load_path = NULL;
    int headless = 0;

    /* Accept "--disk Disk.1 [Disk.2] [Disk.3]" or just "Disk.1 [..]".
     * "--level N" skips intro/title/menu and starts directly at level N.
     * "--load <path>" loads a savestate immediately after init (replaces the
     * full intro/title boot; the game resumes at the saved coroutine yield). */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--vk-selftest"))
            return run_vk_selftest();
        if (!strcmp(argv[i], "--disk"))
            continue;
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
        if (nd < 4)
            disks[nd++] = argv[i];
    }

#ifdef BENEFACTOR_ANDROID
    if (nd == 0) {
        if (!android_bridge_select_disks(disks, 4))
            return 1;
        nd = 3;
    }
#endif

    if (nd < 1) {
        benefactor_log_write(BENEFACTOR_LOG_ERROR, "launcher",
                             "Usage:\n"
                             "  %s [--disk] Disk.1 [Disk.2] [Disk.3] [--level N] [--load path]\n"
                             "     N = 1..60: skip intro/title/menu and start directly at that "
                             "level.\n"
                             "     --load: resume from a savestate immediately after init.\n",
                             argv[0]);
        return 1;
    }

    signal(SIGINT, handler);
    signal(SIGTERM, handler);
    (void)s_running;

    if (headless) {
        extern void hw_request_headless(void);
        hw_request_headless();
    }
    int init_rc = direct_level > 0 ? pc_init_to_gameplay(disks, nd, direct_level)
                                   : pc_init_from_disk(disks, nd);
    if (init_rc < 0) {
        benefactor_log_write(BENEFACTOR_LOG_ERROR, "app", "initialization failed");
        return 1;
    }
#ifdef BENEFACTOR_ANDROID
    if (!android_bridge_enforce_window_policy())
        return 1;
#endif

    if (load_path) {
        if (pc_loadstate(load_path) < 0) {
            benefactor_log_write(BENEFACTOR_LOG_ERROR, "savestate", "load failed: %s", load_path);
            return 1;
        }
        benefactor_log_write(BENEFACTOR_LOG_INFO, "savestate", "resuming from %s", load_path);
    }
    pc_http_debug_start(); /* no-op unless BENEFACTOR_HTTP=<port> is set */
    pc_run();
    benefactor_log_write(BENEFACTOR_LOG_INFO, "app", "stopped");
    pc_fini();
    return 0;
}
