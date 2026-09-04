/* harness_compat.h – compatibility macros for building PUAE sources in-tree.
 * Force-included by the future PUAE oracle harness composition.
 */
#ifndef HARNESS_COMPAT_H
#define HARNESS_COMPAT_H

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#endif /* HARNESS_COMPAT_H */
