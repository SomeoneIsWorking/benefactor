/* pc_overrides_hw.c — Hardware wait loop eliminators and blitter init
 *
 * Overrides for functions that poll Amiga hardware registers in tight loops.
 * The PC port uses a synchronous blitter; all BBUSY polls become no-ops.
 */
#include "pc_internal.h"

/* ─────────────────────────────────────────────────────────────────────────────
 * $0030C2 — hardware-wait loop eliminator
 *
 * Original M68K: tight loop polling bit 6 of DMACONR ($DFF002) until clear.
 * Synchronous blitter never sets BBUSY, so this is always a no-op.
 * ───────────────────────────────────────────────────────────────────────────── */
void native_hw_wait(M68KCtx *ctx) { (void)ctx; }

/* ─────────────────────────────────────────────────────────────────────────────
 * $0031A0 — wait for blitter done + initialise blitter state for frame
 *
 * Original M68K: polls BBUSY until clear, then sets up a D-only blitter fill
 * to zero 512 rows × 60 words starting at $070930.
 *
 * The BBUSY poll is a no-op (synchronous blitter).  Register writes are kept.
 *
 * Register map (A6 = $DFF002):
 *   BLTDPTH/BLTDPTL  ($DFF054/$DFF056) = $070930
 *   BLTCON0          ($DFF040)          = $0100 (D-only), BLTCON1=$0000
 *   BLTDMOD          ($DFF066)          = 0
 *   BLTSIZE          ($DFF058)          = $803C  → triggers 512×60-word fill
 * ───────────────────────────────────────────────────────────────────────────── */
void native_blitter_wait_clear(M68KCtx *ctx)
{
    (void)ctx;
    hw_write32(0xdff054u, 0x00070930u);  /* BLTDPTH/BLTDPTL = $070930        */
    hw_write32(0xdff040u, 0x01000000u);  /* BLTCON0=$0100 (D only), BLTCON1=0 */
    hw_write16(0xdff066u, 0x0000u);      /* BLTDMOD = 0                       */
    hw_write16(0xdff058u, 0x803cu);      /* BLTSIZE = 512×60 words → go       */
}
