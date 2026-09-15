/* What the process says on its way down.
 *
 * Until this existed a fault produced nothing at all: no trace, no guest PC,
 * not even a line naming the signal. The player who hit it had a window vanish
 * and nothing to send. The report is written from inside the handler, so it
 * uses only `write` through `benefactor_log_signal_hex`, and it goes to a file
 * opened at startup as well as to standard error, because a player did not
 * launch from a terminal.
 */
#include "port/crash_report.h"

#include "common/log.h"
#include "engine/hw.h"
#include "port/config.h"
#include "port/project_paths.h"

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

/* Everything above the fault, in host code. The guest-side report says which
 * 68000 address the interpreter was working on, which is the wrong half of the
 * answer when the fault is in a native override: `pc $000150` names the overlay
 * loader and says nothing about which line of it died. backtrace_symbols_fd is
 * the one symbolising call that writes with write() and allocates nothing, so
 * it is the one that may run here. */
#if defined(__APPLE__) || defined(__linux__)
#define CRASH_HOST_BACKTRACE 1
#include <execinfo.h>
#endif

/* Resolved once at install. A handler cannot build a path, so the empty string
 * here means the reports go to standard error alone. */
static char s_path[1024];

/* The faults worth a report. SIGABRT covers a failed assertion and a C++
 * exception nobody caught, both of which reach abort(). SIGINT and SIGTERM are
 * not here: main owns those, and being asked to stop is not a crash.
 * SIGBUS does not exist in the Windows C runtime. */
static const struct {
    int number;
    const char *name;
} kFatalSignals[] = {
    {SIGSEGV, "crash: SIGSEGV (bad memory access)"},
#ifdef SIGBUS
    {SIGBUS, "crash: SIGBUS (bad address)"},
#endif
    {SIGILL, "crash: SIGILL (illegal instruction)"},
    {SIGFPE, "crash: SIGFPE (arithmetic fault)"},
    {SIGABRT, "crash: SIGABRT (abort, failed assertion, or uncaught exception)"},
};

#define FATAL_SIGNAL_COUNT (sizeof kFatalSignals / sizeof kFatalSignals[0])

static const char *signal_name(int number) {
    for (size_t index = 0; index < FATAL_SIGNAL_COUNT; ++index) {
        if (kFatalSignals[index].number == number) {
            return kFatalSignals[index].name;
        }
    }
    return "crash: unexpected signal";
}

static void report_host_stack(void) {
#ifdef CRASH_HOST_BACKTRACE
    void *frames[64];
    const int count = backtrace(frames, (int)(sizeof frames / sizeof frames[0]));
    if (count <= 0) {
        return;
    }
    benefactor_log_signal_hex("host stack, innermost first:", NULL, 0);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
    const int handle = benefactor_log_signal_fd();
    if (handle >= 0) {
        backtrace_symbols_fd(frames, count, handle);
    }
#endif
}

/* Restore the defaults first, every time: if reporting itself faults, the
 * second fault kills the process instead of re-entering this handler forever. */
static void report_and_die(int number, const uint32_t *fault, size_t fault_count) {
    for (size_t index = 0; index < FATAL_SIGNAL_COUNT; ++index) {
        (void)signal(kFatalSignals[index].number, SIG_DFL);
    }
    benefactor_log_signal_hex(signal_name(number), NULL, 0);
    if (fault_count > 0) {
        benefactor_log_signal_hex("fault address (high / low) / signal / code", fault, fault_count);
    }
    hw_report_guest_state("what the guest was doing");
    report_host_stack();
    _exit(3);
}

#ifdef _WIN32
static void crash_handler(int number) {
    report_and_die(number, NULL, 0);
}
#else
/* The address that faulted is the single most useful number in the report: a
 * small one is a null dereference, a plausible-looking one is a stale pointer.
 * It only arrives through SA_SIGINFO, which is why this is not plain signal(). */
static void crash_handler(int number, siginfo_t *info, void *context) {
    (void)context;
    if (info == NULL) {
        report_and_die(number, NULL, 0);
    }
    const uintptr_t where = (uintptr_t)info->si_addr;
    const uint32_t fault[] = {(uint32_t)(where >> 32), (uint32_t)where, (uint32_t)number,
                              (uint32_t)info->si_code};
    report_and_die(number, fault, sizeof fault / sizeof fault[0]);
}
#endif

static void install_handlers(void) {
#ifdef _WIN32
    for (size_t index = 0; index < FATAL_SIGNAL_COUNT; ++index) {
        (void)signal(kFatalSignals[index].number, crash_handler);
    }
#else
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_sigaction = crash_handler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    for (size_t index = 0; index < FATAL_SIGNAL_COUNT; ++index) {
        (void)sigaction(kFatalSignals[index].number, &action, NULL);
    }
#endif
}

int pc_crash_report_install(void) {
    install_handlers();

    /* The handlers are in place whatever happens below: a report on standard
     * error alone still beats the silence this replaced. */
    /* `crash_log` through the config owner rather than getenv, so the same knob
     * works as BENEFACTOR_CRASH_LOG, as a line in benefactor.json, and as a
     * session override — a player being asked where their crashes went should
     * not have to learn a second way of saying it. Empty means the default
     * beside the rest of the project's scratch output. */
    if (!pc_cfg_string("crash_log", "", s_path, (int)sizeof s_path) || s_path[0] == '\0') {
        if (!pc_project_path(PC_PROJECT_PATH_SCRATCH_ROOT, "crash.log", s_path, sizeof s_path)) {
            s_path[0] = '\0';
        }
    }
    if (s_path[0] == '\0' || !benefactor_log_signal_file(s_path)) {
        s_path[0] = '\0';
        return 0;
    }
    return 1;
}

const char *pc_crash_report_path(void) {
    return s_path[0] != '\0' ? s_path : NULL;
}
