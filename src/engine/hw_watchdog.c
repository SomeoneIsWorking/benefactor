#include "common/log.h"
#include "engine/hw_private.h"
#include <signal.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#endif

volatile uint32_t g_hw_last_read = 0; /* last hw register address read */

/* Arm before stepping a single frame; if that frame doesn't finish within a few
 * seconds it's an infinite loop (typically an interrupt/beam busy-wait that never
 * gets satisfied). The platform timer reports the likely cause and kills the app. */
static volatile const char *s_wd_what = NULL;
static void hw_watchdog_handler(int sig) {
    (void)sig;
    uint32_t cop = ((uint32_t)s_regs[0x080 >> 1] << 16) | s_regs[0x082 >> 1];
    const uint32_t values[] = {rt_get_active_call_address(), g_hw_last_read, cop};
    benefactor_log_signal_hex(s_wd_what ? (const char *)s_wd_what : "frame watchdog", values,
                              sizeof values / sizeof values[0]);
    _exit(2);
}
#ifdef _WIN32
static HANDLE s_wd_timer = NULL;
static VOID CALLBACK hw_watchdog_timer_callback(PVOID context, BOOLEAN timer_fired) {
    (void)context;
    (void)timer_fired;
    hw_watchdog_handler(0);
}
#endif

void hw_watchdog_arm(const char *what, int seconds) {
#ifdef _WIN32
    if (s_wd_timer != NULL) {
        DeleteTimerQueueTimer(NULL, s_wd_timer, INVALID_HANDLE_VALUE);
        s_wd_timer = NULL;
    }
    if (!CreateTimerQueueTimer(&s_wd_timer, NULL, hw_watchdog_timer_callback, NULL,
                               (DWORD)(seconds > 0 ? seconds : 2) * 1000, 0, 0)) {
        s_wd_timer = NULL;
        return;
    }
#else
    static int inited = 0;
    if (!inited) {
        signal(SIGALRM, hw_watchdog_handler);
        inited = 1;
    }
#endif
    s_wd_what = what;
#ifndef _WIN32
    alarm((unsigned)(seconds > 0 ? seconds : 2));
#endif
}

void hw_watchdog_disarm(void) {
#ifdef _WIN32
    if (s_wd_timer != NULL) {
        DeleteTimerQueueTimer(NULL, s_wd_timer, INVALID_HANDLE_VALUE);
        s_wd_timer = NULL;
    }
#else
    alarm(0);
#endif
}
