#include "port/project_paths.h"

#include <assert.h>
#include <string.h>

int main(void) {
    char path[4096];
    assert(pc_project_path(PC_PROJECT_PATH_HARNESS_ACTIVITY, "state.bin", path, sizeof path));
    assert(strstr(path, "/scratch/harness-puae/state.bin") != NULL);
    assert(pc_project_path(PC_PROJECT_PATH_HARNESS_WHDLOAD, NULL, path, sizeof path));
    assert(strstr(path, "/scratch/harness-puae/WHDLoad") != NULL);
    assert(!pc_project_path(PC_PROJECT_PATH_HARNESS_ACTIVITY, "/state.bin", path, sizeof path));
    assert(
        !pc_project_path(PC_PROJECT_PATH_HARNESS_ACTIVITY, "nested/state.bin", path, sizeof path));
    assert(!pc_project_path(PC_PROJECT_PATH_HARNESS_ACTIVITY, "..", path, sizeof path));
    return 0;
}
