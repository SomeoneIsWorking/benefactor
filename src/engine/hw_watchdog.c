#include "common/log.h"
#include "engine/hw_private.h"
#include "port/frame_accounting.h"
#include "runtime/guest_runtime.h"
#include <signal.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#endif

volatile uint32_t g_hw_last_read = 0; /* last hw register address read */

/* Beam-boundary accounting, reported by the watchdog. See hw_step_register_beam. */
volatile uint32_t g_hw_beam_crossed = 0;
volatile uint32_t g_hw_beam_taken = 0;
volatile uint32_t g_hw_beam_declined = 0;
volatile uint32_t g_hw_beam_declined_off_flow = 0;

/* Arm before stepping a single frame; if that frame doesn't finish within a few
 * seconds it's an infinite loop (typically an interrupt/beam busy-wait that never
 * gets satisfied). The platform timer reports the guest PC, the loop the
 * interpreter is circling, the last hardware register read, and the recent call
 * targets, then kills the app. */
static volatile const char *s_wd_what = NULL;

#define WD_TRACE_DEPTH 64
#define WD_TRACE_PER_LINE 16
#define WD_CALLS_MAX 8

/* Report the retired-instruction ring oldest-first, in bounded chunks that fit
 * the signal-safe line buffer. A frame that never finishes is a loop, and the
 * ordered tail names every address in it — which a summary of distinct
 * addresses cannot, once the loop is longer than the summary. */
static void hw_watchdog_report_trace(void) {
    uint32_t trace[WD_TRACE_DEPTH];
    int traced = rt_insn_ring_snapshot(trace, (int)(sizeof trace / sizeof trace[0]));
    for (int first = 0; first < traced; first += WD_TRACE_PER_LINE) {
        int remaining = traced - first;
        int count = remaining < WD_TRACE_PER_LINE ? remaining : WD_TRACE_PER_LINE;
        benefactor_log_signal_hex("retired", &trace[first], (size_t)count);
    }
}

static void hw_watchdog_handler(int sig) {
    (void)sig;
    uint32_t cop = ((uint32_t)s_regs[0x080 >> 1] << 16) | s_regs[0x082 >> 1];
    const uint32_t values[] = {rt_get_pc(), rt_get_active_call_address(), g_hw_last_read, cop};
    benefactor_log_signal_hex(s_wd_what ? (const char *)s_wd_what : "frame watchdog", NULL, 0);
    benefactor_log_signal_hex("frame never finished: pc / call / last hw read / cop1lc", values,
                              sizeof values / sizeof values[0]);

    const uint32_t beam[] = {g_hw_beam_crossed, g_hw_beam_taken, g_hw_beam_declined,
                             g_pc_guest_owner};
    benefactor_log_signal_hex("beam crossed / taken / declined, guest owner (0=flow,3=vbl,6=timer)",
                              beam, sizeof beam / sizeof beam[0]);

    hw_watchdog_report_trace();

    uint32_t calls[WD_CALLS_MAX];
    int call_count = rt_recent_snapshot(calls, (int)(sizeof calls / sizeof calls[0]));
    if (call_count > 0)
        benefactor_log_signal_hex("recent guest calls", calls, (size_t)call_count);
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
