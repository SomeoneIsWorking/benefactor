#include "copper.h"
#include "custom.h"
#include "memory.h"

#include <stdint.h>
#include <stdio.h>

/* ── State ───────────────────────────────────────────────────────────────── */

static uint32_t lc[2]   = {0, 0};   /* copper list base addresses */
static uint32_t ip       = 0;        /* instruction pointer */
static int      waiting  = 0;        /* 1 = WAIT not yet satisfied */
static uint16_t wait_vp  = 0;        /* vertical position to wait for */
static uint16_t wait_hp  = 0;        /* horizontal position to wait for */
static uint16_t wait_vpm = 0xFF;     /* vertical mask */
static uint16_t wait_hpm = 0xFE;     /* horizontal mask */
static int      blitter_finish_wait = 0; /* reserved for future use */

/* ── API ─────────────────────────────────────────────────────────────────── */

void copper_set_lc(int n, uint32_t addr)
{
    lc[n] = addr;
}

void copper_jump(int n)
{
    ip      = lc[n];
    waiting = 0;
}

/*
 * Run copper instructions until a WAIT with line > current is hit,
 * a SKIP that is satisfied, or an illegal instruction is encountered.
 *
 * The copper runs once per scanline from the main loop.
 */
void copper_run_scanline(int line, uint16_t *regs)
{
    /* If the list pointer has never been set, do nothing. */
    if (ip == 0 && lc[0] == 0)
        return;

    /* If ip == 0, initialise from copper list 1. */
    if (ip == 0)
        ip = lc[0];

    /* Resolve WAIT from a previous scanline. */
    if (waiting) {
        if ((line & wait_vpm) >= (wait_vp & wait_vpm))
            waiting = 0;
        else
            return;
    }

    /* Safety limit: at most 256 instructions per scanline. */
    for (int i = 0; i < 256; i++) {
        uint16_t ir1 = (uint16_t)mem_read16(ip);
        uint16_t ir2 = (uint16_t)mem_read16(ip + 2);
        ip += 4;

        /* Bit 0 of IR1 distinguishes MOVE (0) from WAIT/SKIP (1). */
        if (!(ir1 & 1)) {
            /* MOVE: ir1[8:1] = register offset, ir2 = data */
            uint32_t reg = ir1 & 0x01FE;
            /* Copper can only write to offsets >= $20 (safety) */
            if (reg >= 0x20) {
                regs[reg >> 1] = ir2;
                /* Debug: trace writes to DIWSTRT */
                if (reg == 0x08E) {
                    static int dbg = 0;
                    if (dbg < 10)
                        fprintf(stderr, "[Cop DIWSTRT] line=%d ip=%06x val=%04x\n",
                                line, ip - 4, ir2);
                    dbg++;
                }
            }
            /* Propagate important side effects */
            if (reg >= 0x120 && reg <= 0x17E) {
                /* Sprite pointers – just store them */
            }
        } else {
            /* WAIT or SKIP */
            uint16_t vp = (ir1 >> 8) & 0xFF;
            uint16_t hp = (ir1 >> 1) & 0x7F;
            uint16_t vpm = (ir2 >> 8) | 0x80;
            uint16_t hpm = ir2 & 0xFE;
            int blitter_done = (ir2 & 0x8000) ? 0 : 1; /* bit 15 = blitter exemption */
            (void)blitter_done;

            if (ir2 & 1) {
                /* SKIP: if beam >= position, skip next instruction */
                if (((line & vpm) > (vp & vpm)) ||
                    ((line & vpm) == (vp & vpm) && (0 & hpm) >= (hp & hpm))) {
                    ip += 4; /* skip next */
                }
            } else {
                /* WAIT */
                if (vp == 0xFF && hp == 0xFE) {
                    /* End-of-list sentinel ($FFFF $FFFE) – restart list next frame */
                    ip = lc[0];
                    return;
                }
                if ((line & vpm) < (vp & vpm)) {
                    /* Wait for a future scanline */
                    wait_vp  = vp;
                    wait_hp  = hp;
                    wait_vpm = vpm;
                    wait_hpm = hpm;
                    waiting  = 1;
                    return;
                }
                /* Beam already past the wait position – continue */
            }
        }
    }
}
