#pragma once

#include <stddef.h>
#include <stdio.h>

/* One bounded, reusable scratch activity owns every harness run artifact. */
int harness_artifacts_prepare(void);
int harness_artifact_path(const char *name, char *output, size_t capacity);
FILE *harness_artifact_open(const char *name, const char *mode);
int harness_stage_puae_disks(const char *const *disk_paths, int disk_count);
