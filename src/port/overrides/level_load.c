/* Native owner for the bounded per-level data stream at $59DC02. */
#include "engine/sb_decompress.h"
#include "port/port_internal.h"

/* The object capture and level-card owners retain their own state; this
 * boundary only coordinates their per-level reset hooks with the decoder. */
extern void native_wsobj_commit_reset(void);
extern void native_lc_text_set(void);

void native_level_load(M68KCtx *ctx) {
    native_wsobj_commit_reset();
    native_lc_text_set();
    benefactor_log_write(BENEFACTOR_LOG_DEBUG, "level-load",
                         "$59DC02: d0=%08X d1=%08X a0=%08X a1=%08X a2=%08X a3=%08X", ctx->D[0],
                         ctx->D[1], ctx->A[0], ctx->A[1], ctx->A[2], ctx->A[3]);
    if (sb_decompress(ctx->D[0], ctx->A[0]) != 0u) {
        if (rt_return_from_native(ctx) != 0)
            benefactor_log_write(BENEFACTOR_LOG_ERROR, "level-load",
                                 "=SB= native return failed at $%06X", ctx->D[0]);
        return;
    }
    benefactor_log_write(BENEFACTOR_LOG_ERROR, "level-load",
                         "=SB= decode failed at $%06X; preserving guest decoder", ctx->D[0]);
    rt_call_original_subroutine(ctx, ctx->image, 0x0059DC02u);
}
