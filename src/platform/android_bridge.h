#pragma once

#include <stddef.h>

/* Initializes Lucent's platform resolver with Android's app-private files
 * directory, then imports and validates a complete Benefactor disk set. */
int android_bridge_select_disks(const char **disks, size_t capacity);
int android_bridge_enforce_window_policy(void);
