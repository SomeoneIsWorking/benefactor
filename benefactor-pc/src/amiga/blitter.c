#include "blitter.h"
#include "custom.h"
#include "memory.h"

#include <stdint.h>
#include <string.h>

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static inline uint32_t reg_ptr(const uint16_t *r, int hi, int lo)
{
    return ((uint32_t)r[hi >> 1] << 16) | r[lo >> 1];
}

static inline int16_t reg_s16(const uint16_t *r, int off)
{
    return (int16_t)r[off >> 1];
}

/* ── Core blit engine ────────────────────────────────────────────────────── */

static void do_blit(const uint16_t *r, int w_words, int h_lines)
{
    /* Pointers */
    uint32_t apt = reg_ptr(r, BLTAPTH, BLTAPTL);
    uint32_t bpt = reg_ptr(r, BLTBPTH, BLTBPTL);
    uint32_t cpt = reg_ptr(r, BLTCPTH, BLTCPTL);
    uint32_t dpt = reg_ptr(r, BLTDPTH, BLTDPTL);

    /* Modulos (signed) */
    int16_t amod = reg_s16(r, BLTAMOD);
    int16_t bmod = reg_s16(r, BLTBMOD);
    int16_t cmod = reg_s16(r, BLTCMOD);
    int16_t dmod = reg_s16(r, BLTDMOD);

    /* Control words */
    uint16_t con0  = r[BLTCON0 >> 1];
    uint16_t con1  = r[BLTCON1 >> 1];
    uint16_t afwm  = r[BLTAFWM >> 1];
    uint16_t alwm  = r[BLTALWM >> 1];

    int use_a = (con0 >> 11) & 1;
    int use_b = (con0 >> 10) & 1;
    int use_c = (con0 >>  9) & 1;
    int use_d = (con0 >>  8) & 1;

    uint8_t minterm = (uint8_t)(con0 & 0xFF);

    int desc     = (con1 >> 1) & 1;   /* descending mode */
    int a_shift  = (con0 >> 12) & 0xF;
    int b_shift  = (con1 >> 12) & 0xF;

    uint16_t a_hold = 0, b_hold = 0;

    for (int y = 0; y < h_lines; y++) {
        for (int x = 0; x < w_words; x++) {
            /* First/last word masks for channel A */
            uint16_t mask = 0xFFFF;
            if (x == 0)           mask &= afwm;
            if (x == w_words - 1) mask &= alwm;

            /* Read source channels */
            uint16_t a_new = use_a ? (uint16_t)mem_read16(apt) : 0xFFFF;
            uint16_t b_new = use_b ? (uint16_t)mem_read16(bpt) : 0;
            uint16_t c_dat = use_c ? (uint16_t)mem_read16(cpt) : 0;

            /* Barrel-shift A and B */
            uint16_t a_dat, b_dat;
            if (!desc) {
                a_dat = (uint16_t)((((uint32_t)a_hold << 16) | a_new) >> (16 - a_shift));
                b_dat = (uint16_t)((((uint32_t)b_hold << 16) | b_new) >> (16 - b_shift));
            } else {
                a_dat = (uint16_t)((((uint32_t)a_new << 16) | a_hold) >> a_shift);
                b_dat = (uint16_t)((((uint32_t)b_new << 16) | b_hold) >> b_shift);
            }
            a_hold = a_new;
            b_hold = b_new;

            /* Apply first/last word mask to A */
            a_dat &= mask;

            /* Minterm logic: 8 possible bit-combinations of A,B,C map to result */
            uint16_t result = 0;
            for (int bit = 15; bit >= 0; bit--) {
                int ai = (a_dat >> bit) & 1;
                int bi = (b_dat >> bit) & 1;
                int ci = (c_dat >> bit) & 1;
                int idx = (ai << 2) | (bi << 1) | ci;
                if ((minterm >> idx) & 1)
                    result |= (uint16_t)(1 << bit);
            }

            if (use_d)
                mem_write16(dpt, result);

            /* Advance pointers */
            if (!desc) {
                if (use_a) apt += 2;
                if (use_b) bpt += 2;
                if (use_c) cpt += 2;
                if (use_d) dpt += 2;
            } else {
                if (use_a) apt -= 2;
                if (use_b) bpt -= 2;
                if (use_c) cpt -= 2;
                if (use_d) dpt -= 2;
            }
        }
        /* End of line: apply modulos */
        if (!desc) {
            if (use_a) apt = (uint32_t)((int32_t)apt + amod);
            if (use_b) bpt = (uint32_t)((int32_t)bpt + bmod);
            if (use_c) cpt = (uint32_t)((int32_t)cpt + cmod);
            if (use_d) dpt = (uint32_t)((int32_t)dpt + dmod);
        } else {
            if (use_a) apt = (uint32_t)((int32_t)apt - amod);
            if (use_b) bpt = (uint32_t)((int32_t)bpt - bmod);
            if (use_c) cpt = (uint32_t)((int32_t)cpt - cmod);
            if (use_d) dpt = (uint32_t)((int32_t)dpt - dmod);
        }
        a_hold = 0;
        b_hold = 0;
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void blitter_start(const uint16_t *r)
{
    uint16_t bltsize = r[BLTSIZE >> 1];
    int h = (bltsize >> 6) & 0x3FF;
    int w = (bltsize)      & 0x03F;
    if (h == 0) h = 1024;
    if (w == 0) w = 64;
    do_blit(r, w, h);
}

void blitter_start_ecs(const uint16_t *r)
{
    int h = r[BLTSIZV >> 1] & 0x7FFF;
    int w = r[BLTSIZH >> 1] & 0x07FF;
    if (h == 0) h = 32768;
    if (w == 0) w = 2048;
    do_blit(r, w, h);
}
