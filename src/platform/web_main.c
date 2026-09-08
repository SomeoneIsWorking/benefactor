#include "port/port.h"

#include <emscripten/emscripten.h>

static int s_started;
static const char *s_disks[3] = {"/Disk.1", "/Disk.2", "/Disk.3"};

EMSCRIPTEN_KEEPALIVE int benefactor_web_start(void) {
    if (s_started)
        return 0;
    if (pc_init_from_disk(s_disks, 3) < 0)
        return -1;
    s_started = 1;
    return 0;
}

static void benefactor_web_step(void) {
    if (s_started && pc_step() != 0)
        emscripten_cancel_main_loop();
}

int main(void) {
    emscripten_set_main_loop(benefactor_web_step, 0, 1);
    return 0;
}
