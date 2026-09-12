#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Resolve the persisted desktop disk set or show the SDL3 native setup flow.
 * The returned paths remain valid until the next call in this process. */
int desktop_setup_disks(const char **disks, int capacity);

#ifdef __cplusplus
}
#endif
