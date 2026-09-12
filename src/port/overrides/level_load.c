/* Level-entry hooks around the guest's per-level data dispatcher at $59DC02. */
#include "port/port_internal.h"

/* The object capture and level-card owners retain their own state. The guest
 * dispatcher also installs stream callbacks and updates decoder state; a
 * standalone =SB= decode cannot replace that contract. */
extern void native_wsobj_commit_reset(void);
extern void native_lc_text_set(void);

void native_level_load(M68KCtx *ctx) {
    native_wsobj_commit_reset();
    native_lc_text_set();
    rt_call(ctx, ctx->image, 0x0059DC02u);
}
