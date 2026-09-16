/* main.c – Native PC game entry point (single path: native disk boot) */
#include "common/log.h"
#include "common/version.h"
#include "engine/hw.h"
#include "platform/update_transport.h"
#include "port/config.h"
#include "port/control/control_server.h"
#include "port/crash_report.h"
#include "port/port.h"
#ifndef BENEFACTOR_ANDROID
#include "platform/desktop_setup.h"
#endif
#include "render/present_backend.h"
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef BENEFACTOR_ANDROID
#include "platform/android_bridge.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#endif

/* Headless Vulkan self-test (no window/disks): render a gradient through the
 * offscreen Vulkan pipeline and compare the readback to the input. Proves the
 * GPU present path works with the display off. Returns process exit code. */
static int run_vk_selftest(void) {
#ifdef BENEFACTOR_HAVE_VULKAN
    int w = 480, h = 282;
    uint32_t *img = malloc((size_t)w * h * 4);
    if (!img) {
        return 1;
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            img[y * w + x] = 0xFF000000u | ((uint32_t)(x * 255 / w) << 16) |
                             ((uint32_t)(y * 255 / h) << 8) | (uint32_t)((x ^ y) & 0xFF);
        }
    }
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
    int http_port = 0;

    /* Accept "--disk Disk.1 [Disk.2] [Disk.3]" or just "Disk.1 [..]".
     * "--level N" skips intro/title/menu and starts directly at level N.
     * "--load <path>" loads a savestate immediately after init (replaces the
     * full intro/title boot; the game resumes at the saved coroutine yield). */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--vk-selftest")) {
            return run_vk_selftest();
        }
        if (!strcmp(argv[i], "--disk")) {
            continue;
        }
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
        /* Not the same thing as --headless: the game still renders, composes
         * and presents, the window just never appears. See
         * present_backend_set_hidden. */
        if (!strcmp(argv[i], "--hidden")) {
            present_backend_set_hidden(1);
            continue;
        }
        if (!strcmp(argv[i], "--http") && i + 1 < argc) {
            http_port = atoi(argv[++i]);
            continue;
        }
        if (nd < 4) {
            disks[nd++] = argv[i];
        }
    }

#ifdef BENEFACTOR_ANDROID
    if (nd == 0) {
        if (!android_bridge_select_disks(disks, 4)) {
            return 1;
        }
        nd = 3;
    }
#endif

#ifndef BENEFACTOR_ANDROID
    if (nd == 0) {
        if (!desktop_setup_disks(disks, 4)) {
            return 1;
        }
        nd = 3;
    }
#endif

    if (nd < 1) {
        benefactor_log_write(BENEFACTOR_LOG_ERROR, "launcher",
                             "Usage:\n"
                             "  %s [--disk] Disk.1 [Disk.2] [Disk.3] [--level N] [--load path]\n"
                             "     N = 1..60: skip intro/title/menu and start directly at that "
                             "level.\n"
                             "     --load: resume from a savestate immediately after init.\n"
                             "     --http N: open the diagnostic control channel on port N.\n",
                             argv[0]);
        return 1;
    }

    signal(SIGINT, handler);
    signal(SIGTERM, handler);
    /* Before anything can fault. A crash that reports nothing is a crash nobody
     * can act on, and until this was installed that was every crash. */
    if (pc_crash_report_install()) {
        benefactor_log_write(BENEFACTOR_LOG_INFO, "app", "crash reports append to %s",
                             pc_crash_report_path());
    } else {
        benefactor_log_write(BENEFACTOR_LOG_INFO, "app",
                             "crash reports go to standard error only; no crash file could be "
                             "opened (set BENEFACTOR_CRASH_LOG to name one)");
    }
    (void)s_running;

    if (headless) {
        hw_request_headless();
    }
    benefactor_log_write(BENEFACTOR_LOG_INFO, "app", "Benefactor %s", pc_version());
    int init_rc = direct_level > 0 ? pc_init_to_gameplay(disks, nd, direct_level)
                                   : pc_init_from_disk(disks, nd);
    if (init_rc < 0) {
        benefactor_log_write(BENEFACTOR_LOG_ERROR, "app", "initialization failed");
        return 1;
    }
#ifdef BENEFACTOR_ANDROID
    if (!android_bridge_enforce_window_policy()) {
        return 1;
    }
#endif

    if (load_path) {
        if (pc_loadstate(load_path) < 0) {
            benefactor_log_write(BENEFACTOR_LOG_ERROR, "savestate", "load failed: %s", load_path);
            return 1;
        }
        benefactor_log_write(BENEFACTOR_LOG_INFO, "savestate", "resuming from %s", load_path);
    }
    if (http_port > 0) {
        /* The session layer, which is what the env var also feeds; --http is how
         * a host without an environment to set (Android) reaches the channel. */
        char port_text[8];
        snprintf(port_text, sizeof port_text, "%d", http_port);
        pc_cfg_set("http", port_text);
    }
    pc_control_server_start(); /* no-op unless BENEFACTOR_HTTP=<port> or --http N */
    /* The update check runs once per launch, in the background; its result
     * reaches the pause menu. */
    platform_update_check_start();
    pc_run();
    benefactor_log_write(BENEFACTOR_LOG_INFO, "app", "stopped");
    pc_fini();
    return 0;
}
