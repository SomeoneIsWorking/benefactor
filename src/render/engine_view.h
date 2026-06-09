/* native_engine_view.h — the wide renderer's ONLY input.
 *
 * THE FIREWALL (de-hack keystone). The widescreen renderer must be a pure
 * function of *engine state*: it may read camera/scroll/level-geometry
 * variables and the camera-independent capture lists, and nothing else. It is
 * explicitly DENIED access to anything that represents the *displayed output* —
 * the copper list, the BPL pointers (s_anchors[]), s_fb/s_out, and raw g_mem.
 *
 * Why this exists: every bandaid the wide path ever grew was a reconciliation
 * between two sources of truth — engine state AND the displayed copper result —
 * glued together with fudge factors (e.g. reverse-projecting the camera from the
 * displayed BPL pointer). Remove the second source and curve-fitting has nothing
 * to fit to: the only way left to fix a misalignment is to derive the value
 * correctly from engine state, i.e. do the reverse engineering.
 *
 * RULES (enforced, not aspirational):
 *  1. Every field below is filled by EXACTLY ONE accessor reading ONE engine
 *     variable, with a sourced comment naming the engine address/routine it
 *     comes from. No fallbacks: a value that cannot be read is a hard failure
 *     (engine_view_capture returns 0), never a fudge default.
 *  2. No bare hex literal applied to an engine quantity without a sourced
 *     comment. (A build-time gate greps for violations — see tools/.)
 *  3. The PUAE/harness comparison is strictly DOWNSTREAM: it consumes the
 *     renderer's output and never feeds a value back in. There is no path from
 *     "the pixels didn't match" to "nudge a constant here".
 *
 * If you find yourself wanting a value that isn't here, the answer is to RE the
 * engine variable that holds it and add a sourced accessor — never to read the
 * copper/displayed frame and back-compute it.
 */
#ifndef NATIVE_ENGINE_VIEW_H
#define NATIVE_ENGINE_VIEW_H

#include <stdint.h>

/* Authoritative engine addresses (a5 = $57EE12; absolute = a5 + a5-offset).
 * Each is the SINGLE source for its quantity — cite the offset and the routine
 * that maintains it so the next reader can verify against the disassembly. */
#define EV_CAMERA_ADDR   0x0057FDBAu /* a5+$0FA8  signed screen-left world X, engine-clamped (see gameplay-engine-map.md) */
#define EV_LEVEL_LO_ADDR 0x0057FE8Cu /* a5+$107A  per-level world low bound  (clamp routine $57C79E) */
#define EV_LEVEL_HI_ADDR 0x0057FE8Eu /* a5+$107C  per-level world high bound (clamp routine $57C79E) */
#define EV_PHASETAB_ADDR 0x0057F4BCu /* row-offset/phase table; entry[1].d2adj (+4) = per-level tilemap row stride */
#define EV_GFXTAB_ADDR   0x005A539Eu /* tile graphics base table */

/* The engine's camera-clamp transform, RE'd & verified in widescreen-plan.md:91-93
 * (L9 save: lo=144,hi=1648 -> cam in [0,1392]). These are the engine's own
 * constants, NOT display fudge: min_cam = $107A(a5) - 0x90, max_cam = $107C(a5) - 0x100. */
#define EV_CLAMP_LO_BIAS 0x90
#define EV_CLAMP_HI_BIAS 0x100

/* A pure, read-only snapshot of the engine quantities the wide renderer needs.
 * Captured once per frame by engine_view_capture(); the composers receive a
 * const* and may read NOTHING else. */
typedef struct EngineView {
    int valid;          /* 0 if any sourced read failed (caller must bail; no fudge) */

    int camera;         /* EV_CAMERA_ADDR, sign-extended: screen-left world X (px) */
    int level_lo;       /* engine min camera = (EV_LEVEL_LO_ADDR) - EV_CLAMP_LO_BIAS */
    int level_hi;       /* engine max camera = (EV_LEVEL_HI_ADDR) - EV_CLAMP_HI_BIAS */
    int row_stride;     /* per-level tilemap row stride (bytes), abs(EV_PHASETAB_ADDR+4) */
    uint32_t gfxtab;    /* tile graphics base (EV_GFXTAB_ADDR) */
} EngineView;

/* Fill `ev` from current engine state. Returns 1 on success, 0 if any sourced
 * read failed or is out of range (caller must not render this frame rather than
 * substitute a default). Reads engine state ONLY — never the copper/displayed
 * frame. */
int engine_view_capture(EngineView *ev);

#endif /* NATIVE_ENGINE_VIEW_H */
