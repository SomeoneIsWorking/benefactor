#pragma once

#include <stddef.h>

typedef enum PcProjectPathKind {
    PC_PROJECT_PATH_SCRATCH_ROOT = 0,
    PC_PROJECT_PATH_HARNESS_ACTIVITY = 1,
    PC_PROJECT_PATH_HARNESS_WHDLOAD = 2,
} PcProjectPathKind;

/* Resolve a project-owned path from the repository root discovered at runtime.
 * `leaf` is optional and must be one filename, never an absolute or nested path. */
int pc_project_path(PcProjectPathKind kind, const char *leaf, char *output, size_t capacity);
