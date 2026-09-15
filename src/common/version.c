#include "common/version.h"

#ifndef BENEFACTOR_VERSION
#error "BENEFACTOR_VERSION must come from version.txt via the build system"
#endif

const char *pc_version(void) {
    return BENEFACTOR_VERSION;
}
