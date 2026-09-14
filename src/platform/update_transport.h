/* update_transport.h — the host's single request for the latest release tag.
 *
 * update_check.cpp owns what the answer means; this owns how the bytes arrive,
 * because that is the part every platform answers differently: the desktop uses
 * the host's HTTP client (libcurl, or WinHTTP on Windows), Android asks the
 * activity (which has the platform's own client), and the browser asks the page
 * to fetch it.
 *
 * The call must return immediately and report later through pc_update_report().
 * A host that cannot start a request reports the failure itself, so a check that
 * did nothing never leaves the UI in CHECKING forever. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* The whole start-up path for one launch: honour the player's setting, record
 * that a check is running, and hand off to this host's transport below. Safe to
 * call more than once; only the first call does anything, and only when the
 * check is enabled. */
void platform_update_check_start(void);

void platform_update_check_begin(void);

#ifdef __cplusplus
}
#endif
