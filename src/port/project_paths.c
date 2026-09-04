#include "port/project_paths.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
    PROJECT_PATH_CAPACITY = 4096,
    PROJECT_ROOT_SEARCH_LIMIT = 16,
};

static char s_project_root[PROJECT_PATH_CAPACITY];

static int path_exists(const char *path) {
    struct stat status;
    return stat(path, &status) == 0;
}

static int is_project_root(const char *candidate) {
    char marker[PROJECT_PATH_CAPACITY];
    int length = snprintf(marker, sizeof marker, "%s/pyproject.toml", candidate);
    if (length < 0 || (size_t)length >= sizeof marker || !path_exists(marker))
        return 0;
    length = snprintf(marker, sizeof marker, "%s/src/harness", candidate);
    return length >= 0 && (size_t)length < sizeof marker && path_exists(marker);
}

static int discover_project_root(void) {
    if (s_project_root[0])
        return 1;
    if (!getcwd(s_project_root, sizeof s_project_root))
        return 0;
    for (int depth = 0; depth < PROJECT_ROOT_SEARCH_LIMIT; ++depth) {
        if (is_project_root(s_project_root))
            return 1;
        char *separator = strrchr(s_project_root, '/');
        if (!separator)
            break;
        if (separator == s_project_root) {
            s_project_root[1] = '\0';
            break;
        }
        *separator = '\0';
    }
    s_project_root[0] = '\0';
    return 0;
}

static const char *relative_path(PcProjectPathKind kind) {
    switch (kind) {
    case PC_PROJECT_PATH_SCRATCH_ROOT:
        return "scratch";
    case PC_PROJECT_PATH_HARNESS_ACTIVITY:
        return "scratch/harness-puae";
    case PC_PROJECT_PATH_HARNESS_WHDLOAD:
        return "scratch/harness-puae/WHDLoad";
    }
    return NULL;
}

static int valid_leaf(const char *leaf) {
    return !leaf || (leaf[0] && leaf[0] != '/' && !strchr(leaf, '/') && !strchr(leaf, '\\') &&
                     strcmp(leaf, ".") != 0 && strcmp(leaf, "..") != 0);
}

int pc_project_path(PcProjectPathKind kind, const char *leaf, char *output, size_t capacity) {
    const char *relative = relative_path(kind);
    if (!output || capacity == 0 || !relative || !valid_leaf(leaf) || !discover_project_root())
        return 0;
    int length = leaf ? snprintf(output, capacity, "%s/%s/%s", s_project_root, relative, leaf)
                      : snprintf(output, capacity, "%s/%s", s_project_root, relative);
    if (length < 0 || (size_t)length >= capacity) {
        output[0] = '\0';
        return 0;
    }
    return 1;
}
