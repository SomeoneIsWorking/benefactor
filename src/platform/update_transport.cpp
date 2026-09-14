/* Starting the update check is one operation on every host: the player's
 * setting decides whether it runs at all, the policy records that it is running,
 * and the host's own transport performs the request. Keeping that order in one
 * place means no host can start a check the player turned off, and none can
 * record a state its transport never produced. */
#include "platform/update_transport.h"

#include "port/config.h"
#include "port/update_check.h"

extern "C" void platform_update_check_start(void) {
    static bool started = false;
    if (started) {
        return;
    }
    started = true;
    if (!pc_cfg_bool("update_check", 1)) {
        return;
    }
    pc_update_reset();
    pc_update_begin_checking();
    platform_update_check_begin();
}
