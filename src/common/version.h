/* version.h — the product version, as the build system read it from version.txt.
 *
 * One number for the desktop binary, the console banner, the pause menu, and
 * Android's versionName; the release tag is checked against the same file. A
 * missing definition is a build defect, not something to paper over with a
 * placeholder: pc_version() would then lie about which build is running. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* MAJOR.MINOR.PATCH, without a leading "v". */
const char *pc_version(void);

#ifdef __cplusplus
}
#endif
