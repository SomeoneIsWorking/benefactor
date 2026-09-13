#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Establishes Lucent's Android app-private storage, then provides a complete
 * Benefactor disk set: the committed set when one is installed, otherwise the
 * in-app setup screen backed by Android's own document picker.
 * Returns 1 and fills disks[0..2] on success; 0 on failure with a log line. */
int android_bridge_select_disks(const char **disks, size_t capacity);

/* Restores the title window policy after SDL rewrites it. */
int android_bridge_enforce_window_policy(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
