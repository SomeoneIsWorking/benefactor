#include "harness/artifacts.h"

#include "common/log.h"
#include "port/project_paths.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int ensure_directory(const char *path) {
    if (mkdir(path, 0755) == 0)
        return 1;
    if (errno != EEXIST)
        return 0;
    struct stat status;
    return stat(path, &status) == 0 && S_ISDIR(status.st_mode);
}

int harness_artifacts_prepare(void) {
    char scratch[4096];
    char activity[4096];
    if (!pc_project_path(PC_PROJECT_PATH_SCRATCH_ROOT, NULL, scratch, sizeof scratch) ||
        !pc_project_path(PC_PROJECT_PATH_HARNESS_ACTIVITY, NULL, activity, sizeof activity) ||
        !ensure_directory(scratch) || !ensure_directory(activity)) {
        benefactor_log_write(BENEFACTOR_LOG_ERROR, "harness",
                             "cannot prepare project scratch activity directory");
        return 0;
    }
    return 1;
}

int harness_artifact_path(const char *name, char *output, size_t capacity) {
    return pc_project_path(PC_PROJECT_PATH_HARNESS_ACTIVITY, name, output, capacity);
}

FILE *harness_artifact_open(const char *name, const char *mode) {
    char path[4096];
    if (!harness_artifact_path(name, path, sizeof path))
        return NULL;
    FILE *file = fopen(path, mode);
    if (!file)
        benefactor_log_write(BENEFACTOR_LOG_ERROR, "harness", "cannot open %s: %s", path,
                             strerror(errno));
    return file;
}

static int copy_file(const char *source_path, const char *destination_path) {
    struct stat source_status;
    struct stat destination_status;
    if (stat(source_path, &source_status) == 0 &&
        stat(destination_path, &destination_status) == 0 &&
        source_status.st_dev == destination_status.st_dev &&
        source_status.st_ino == destination_status.st_ino)
        return 1;
    FILE *source = fopen(source_path, "rb");
    if (!source)
        return 0;
    FILE *destination = fopen(destination_path, "wb");
    if (!destination) {
        fclose(source);
        return 0;
    }
    char buffer[65536];
    int ok = 1;
    for (;;) {
        size_t count = fread(buffer, 1, sizeof buffer, source);
        if (count > 0 && fwrite(buffer, 1, count, destination) != count) {
            ok = 0;
            break;
        }
        if (count < sizeof buffer) {
            if (ferror(source))
                ok = 0;
            break;
        }
    }
    if (fclose(destination) != 0)
        ok = 0;
    fclose(source);
    return ok;
}

int harness_stage_puae_disks(const char *const *disk_paths, int disk_count) {
    char mount_path[4096];
    struct stat status;
    if (!disk_paths || disk_count < 1 || disk_count > 4 ||
        !pc_project_path(PC_PROJECT_PATH_HARNESS_WHDLOAD, NULL, mount_path, sizeof mount_path) ||
        stat(mount_path, &status) != 0 || !S_ISDIR(status.st_mode)) {
        benefactor_log_write(BENEFACTOR_LOG_ERROR, "harness",
                             "PUAE did not prepare its project-local WHDLoad mount");
        return 0;
    }
    for (int index = 0; index < 4; ++index) {
        char destination[4096];
        char staging[4096];
        int length = snprintf(destination, sizeof destination, "%s/Disk.%d", mount_path, index + 1);
        int staging_length = snprintf(staging, sizeof staging, "%s.incoming", destination);
        if (length < 0 || (size_t)length >= sizeof destination || staging_length < 0 ||
            (size_t)staging_length >= sizeof staging)
            return 0;
        if (index >= disk_count) {
            if ((unlink(destination) != 0 && errno != ENOENT) ||
                (unlink(staging) != 0 && errno != ENOENT))
                return 0;
            continue;
        }
        if (!disk_paths[index] || !copy_file(disk_paths[index], staging) ||
            rename(staging, destination) != 0) {
            unlink(staging);
            benefactor_log_write(BENEFACTOR_LOG_ERROR, "harness",
                                 "cannot stage oracle Disk.%d from %s", index + 1,
                                 disk_paths[index] ? disk_paths[index] : "(missing)");
            return 0;
        }
    }
    return 1;
}
