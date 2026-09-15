// The Windows update transport against a real HTTP client. This runs on Windows,
// and on any host with a MinGW cross-compiler plus Wine, so the one transport no
// Linux build can see is still exercised rather than assumed: WinHTTP opens the
// TLS connection, reads the release document, and the shared policy turns the
// tag it finds into a state.
//
// It is not part of the product: a build of it links only the transport, the
// policy, and lucent's version parsing.

#include "platform/update_transport.h"

#include "common/log.h"
#include "port/update_check.h"

#include <windows.h>

// The policy compares against this build's version; this test states its own.
extern "C" const char *pc_version(void) {
    return "0.0.1";
}

int main() {
    // The shipping start path records that a check is running before the host
    // begins it; this test does the same two steps.
    pc_update_begin_checking();
    platform_update_check_begin();
    // The request is detached; wait for the result to be adopted.
    for (int attempt = 0; attempt < 300 && pc_update_state() == PC_UPDATE_CHECKING; ++attempt) {
        Sleep(100);
        pc_update_tick();
    }
    // This process's own log boundary, so a failure says what happened.
    benefactor_log_write(BENEFACTOR_LOG_INFO, "update", "state=%d line=%s detail=%s latest=%s",
                         static_cast<int>(pc_update_state()), pc_update_line(), pc_update_detail(),
                         pc_update_latest());
    if (pc_update_state() == PC_UPDATE_CHECKING) {
        benefactor_log_write(BENEFACTOR_LOG_ERROR, "update", "the check never finished");
        return 1;
    }
    if (pc_update_state() == PC_UPDATE_FAILED) {
        benefactor_log_write(BENEFACTOR_LOG_ERROR, "update",
                             "the transport could not complete a real check");
        return 1;
    }
    // Version 0.0.1 against the published release must find a newer one, which is
    // the answer that proves the tag was parsed and compared rather than merely
    // received.
    if (pc_update_state() != PC_UPDATE_AVAILABLE || pc_update_latest()[0] == '\0') {
        benefactor_log_write(BENEFACTOR_LOG_ERROR, "update",
                             "an older build is not told a newer release exists");
        return 1;
    }
    benefactor_log_write(BENEFACTOR_LOG_INFO, "update", "winhttp transport: OK");
    return 0;
}
