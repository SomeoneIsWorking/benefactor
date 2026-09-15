/* crash_report.h — what the process says when it dies unexpectedly.
 *
 * The frame watchdog already reports a frame that never finishes. A frame that
 * faults reported nothing at all: no handler was installed for SIGSEGV and its
 * siblings, so the process vanished and the only evidence was the player saying
 * it closed. This installs that handler, and gives the report somewhere to land
 * besides standard error, which nobody sees unless they launched from a shell.
 *
 * Everything the handler does is async-signal-safe: no allocation, no stdio,
 * `write` only. The path is resolved and opened here, before any fault can
 * happen, because a handler may not open a file.
 */
#pragma once

/* Install handlers for the fatal signals and, when one can be named, open the
 * file the reports are appended to. Safe to call twice; the second call
 * replaces the file. Returns 1 when a crash file is open, 0 when the reports
 * will only reach standard error. */
int pc_crash_report_install(void);

/* Where the reports go, or NULL when only standard error gets them. Reported at
 * startup so the file's location is known before it is needed rather than
 * hunted for afterwards. */
const char *pc_crash_report_path(void);
