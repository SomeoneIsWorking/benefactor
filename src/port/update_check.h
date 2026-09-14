/* update_check.h — "is there a newer release than this build?" as state, not as
 * a request.
 *
 * The policy is separate from the transport on purpose: the comparison is shared
 * (lucent::version) and the answer is a small state machine any host can feed. A
 * host fetches the latest release tag and calls pc_update_report(); the pause
 * menu then draws pc_update_line(). Nothing here blocks, allocates per frame, or
 * performs I/O.
 *
 * Every state has a distinct line: a check that never ran, one that is running,
 * one that found this build current, one that found a newer release, and one
 * that failed. A check that could not run must never look like "up to date". */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PC_UPDATE_IDLE = 0, /* nothing asked for it, or the player turned it off */
    PC_UPDATE_CHECKING,
    PC_UPDATE_CURRENT,
    PC_UPDATE_AVAILABLE,
    PC_UPDATE_FAILED,
} PcUpdateState;

/* Back to IDLE. Called before a fresh check so a repeated check cannot show a
 * stale answer while it runs. */
void pc_update_reset(void);

/* The host's request is in flight. */
void pc_update_begin_checking(void);

/* A host fetch finished. `tag` is the release tag when it succeeded (NULL on
 * failure), `error` is a short reason otherwise (NULL on success). An unparsable
 * tag is a failure, not an update. May be called from any thread: it stores the
 * result for pc_update_tick(). */
void pc_update_report(const char *tag, const char *error);

/* Adopt a reported result. Called once per frame on the main thread, so every
 * value the UI reads was written by the thread that reads it. */
void pc_update_tick(void);

PcUpdateState pc_update_state(void);

/* The newer release's tag when one is available, else "". */
const char *pc_update_latest(void);

/* The reason a check failed, else "". */
const char *pc_update_detail(void);

/* One line for the UI: "" while IDLE, otherwise the state as a sentence. */
const char *pc_update_line(void);

/* The release document this title asks about, in the two forms its hosts need:
 * a URL for the clients that take one, and the host and path for the client that
 * takes them separately. The parts are the single authority here, so no host
 * parses a URL and none can ask a different service. */
const char *pc_update_release_url(void);
const char *pc_update_release_host(void);
const char *pc_update_release_path(void);

#ifdef __cplusplus
}
#endif
