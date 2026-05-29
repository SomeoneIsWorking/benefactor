#include "custom.h"
#include "blitter.h"
#include "copper.h"
#include "paula.h"

#include <string.h>
#include <stdio.h>
#include "m68k.h"

/* ── Internal register file ──────────────────────────────────────────────── */

/* Shadow copy of all writable custom-chip registers (word-wide, 512 entries).
 * Index = (offset from $DFF000) >> 1  */
static uint16_t regs[512];

static int vblank_pending = 0;
static int current_scanline = 0;

/* DMACON is a set/clear register: bit 15 = 1 → set bits, 0 → clear bits.
 * We keep the live value in regs[DMACON>>1]. */
static uint16_t dmacon_val = 0;
static uint16_t intena_val = 0;
static uint16_t intreq_val = 0;
static uint16_t adkcon_val = 0;

/* Beam position (11-bit VPOS, 8-bit HPOS) */
static uint16_t vposr_val = 0;
static uint16_t vhposr_val = 0;

/* ── Helpers ──────────────────────────────────────────────────────────────── */

static inline uint32_t ptr32(int hi_reg, int lo_reg)
{
    return ((uint32_t)regs[hi_reg >> 1] << 16) | regs[lo_reg >> 1];
}

/* ── Init ────────────────────────────────────────────────────────────────── */

void custom_init(void)
{
    memset(regs, 0, sizeof(regs));
    dmacon_val  = 0;
    intena_val  = 0;
    intreq_val  = 0;
    adkcon_val  = 0;
    vblank_pending   = 0;
    current_scanline = 0;
}

/* ── Read ────────────────────────────────────────────────────────────────── */

uint16_t custom_read(uint32_t reg)
{
    switch (reg) {
    case VPOSR:    return vposr_val;
    case VHPOSR:   return vhposr_val;
    case DMACONR:  return dmacon_val;
    case INTENAR:  return intena_val;
    case INTREQR:  return intreq_val;
    case ADKCONR:  return adkcon_val;
    /* JOY0DAT / JOY1DAT are managed by input.c via the regs[] shadow */
    case JOY0DAT:  return regs[JOY0DAT >> 1];
    case JOY1DAT:  return regs[JOY1DAT >> 1];
    case CLXDAT:   return 0;           /* no collision detection needed */
    case POTINP:   return 0xFF00;
    case POT0DAT:  return 0;
    case POT1DAT:  return 0;
    case DSKBYTR:  return 0x8000;      /* disk byte ready, no actual data */
    case LISAID:   return 0x00F8;      /* pretend to be ECS Agnus / ECS Denise */
    default:
        return regs[reg >> 1];
    }
}

/* ── Write ───────────────────────────────────────────────────────────────── */

void custom_write(uint32_t reg, uint16_t val)
{
    switch (reg) {
    /* ── DMA control (set/clear) ───────────────────────────────────────── */
    case DMACON:
        if (val & 0x8000)
            dmacon_val |=  (val & 0x7FFF);
        else
            dmacon_val &= ~(val & 0x7FFF);
        regs[DMACON >> 1] = dmacon_val;
        /* Notify Paula of audio DMA changes */
        paula_dmacon_update(dmacon_val);
        return;

    /* ── Interrupt enable (set/clear) ─────────────────────────────────── */
    case INTENA:
        if (val & 0x8000)
            intena_val |=  (val & 0x7FFF);
        else
            intena_val &= ~(val & 0x7FFF);
        regs[INTENA >> 1] = intena_val;
        return;

    /* ── Interrupt request (acknowledge = write 0 bits) ──────────────── */
    case INTREQ:
        if (val & 0x8000)
            intreq_val |=  (val & 0x7FFF);
        else
            intreq_val &= ~(val & 0x7FFF);
        regs[INTREQ >> 1] = intreq_val;
        return;

    /* ── ADKCON (set/clear) ──────────────────────────────────────────── */
    case ADKCON:
        if (val & 0x8000)
            adkcon_val |=  (val & 0x7FFF);
        else
            adkcon_val &= ~(val & 0x7FFF);
        regs[ADKCON >> 1] = adkcon_val;
        return;

    /* ── Copper ──────────────────────────────────────────────────────── */
    case COP1LCH: regs[COP1LCH >> 1] = val; return;
    case COP1LCL: regs[COP1LCL >> 1] = val; copper_set_lc(0, ptr32(COP1LCH, COP1LCL)); return;
    case COP2LCH: regs[COP2LCH >> 1] = val; return;
    case COP2LCL: regs[COP2LCL >> 1] = val; copper_set_lc(1, ptr32(COP2LCH, COP2LCL)); return;
    case COPJMP1: copper_jump(0); return;
    case COPJMP2: copper_jump(1); return;

    /* ── Blitter ─────────────────────────────────────────────────────── */
    case BLTCON0: regs[BLTCON0 >> 1] = val; return;
    case BLTCON1: regs[BLTCON1 >> 1] = val; return;
    case BLTAFWM: regs[BLTAFWM >> 1] = val; return;
    case BLTALWM: regs[BLTALWM >> 1] = val; return;
    case BLTCPTH: regs[BLTCPTH >> 1] = val; return;
    case BLTCPTL: regs[BLTCPTL >> 1] = val; return;
    case BLTBPTH: regs[BLTBPTH >> 1] = val; return;
    case BLTBPTL: regs[BLTBPTL >> 1] = val; return;
    case BLTAPTH: regs[BLTAPTH >> 1] = val; return;
    case BLTAPTL: regs[BLTAPTL >> 1] = val; return;
    case BLTDPTH: regs[BLTDPTH >> 1] = val; return;
    case BLTDPTL: regs[BLTDPTL >> 1] = val; return;
    case BLTCMOD: regs[BLTCMOD >> 1] = val; return;
    case BLTBMOD: regs[BLTBMOD >> 1] = val; return;
    case BLTAMOD: regs[BLTAMOD >> 1] = val; return;
    case BLTDMOD: regs[BLTDMOD >> 1] = val; return;
    case BLTCDAT: regs[BLTCDAT >> 1] = val; return;
    case BLTBDAT: regs[BLTBDAT >> 1] = val; return;
    case BLTADAT: regs[BLTADAT >> 1] = val; return;
    case BLTSIZE:
        /* Writing BLTSIZE starts the blit. */
        regs[BLTSIZE >> 1] = val;
        blitter_start(regs);
        return;
    case BLTSIZV:   /* ECS: set V size */
        regs[BLTSIZV >> 1] = val; return;
    case BLTSIZH:   /* ECS: set H size and start */
        regs[BLTSIZH >> 1] = val;
        blitter_start_ecs(regs);
        return;

    /* ── Paula audio ─────────────────────────────────────────────────── */
    case AUD0LCH: case AUD0LCL: case AUD0LEN: case AUD0PER: case AUD0VOL: case AUD0DAT:
    case AUD1LCH: case AUD1LCL: case AUD1LEN: case AUD1PER: case AUD1VOL: case AUD1DAT:
    case AUD2LCH: case AUD2LCL: case AUD2LEN: case AUD2PER: case AUD2VOL: case AUD2DAT:
    case AUD3LCH: case AUD3LCL: case AUD3LEN: case AUD3PER: case AUD3VOL: case AUD3DAT:
        regs[reg >> 1] = val;
        paula_reg_write(reg, val, regs);
        return;

    default:
        if (reg == DIWSTRT) {
            static int diwstrt_log = 0;
            if (diwstrt_log < 20)
                fprintf(stderr, "[DIWSTRT write] val=%04x PC=%06x\n", val,
                        m68k_get_reg(NULL, M68K_REG_PC));
            diwstrt_log++;
        }
        regs[reg >> 1] = val;
        return;
    }
}

/* ── Scanline tick ───────────────────────────────────────────────────────── */

/* PAL: 312 scanlines per frame, ~7.09 MHz pixel clock.
 * We run the copper each scanline and fire VBLANK at line 0. */
#define PAL_LINES   312

void custom_tick_scanline(int scanline)
{
    current_scanline = scanline;

    /* Update beam position registers */
    vposr_val  = (uint16_t)((scanline >> 8) & 1);
    vhposr_val = (uint16_t)((scanline & 0xFF) << 8);

    /* Run copper for this scanline */
    if (dmacon_val & COPEN)
        copper_run_scanline(scanline, regs);

    /* Vertical blank at start of frame */
    if (scanline == 0) {
        vblank_pending = 1;
        intreq_val |= VERTB;
        regs[INTREQ >> 1] = intreq_val;
    }
}

int custom_vblank_pending(void)  { return vblank_pending; }
void custom_vblank_ack(void)     { vblank_pending = 0; }

/* ── State accessors ─────────────────────────────────────────────────────── */

uint16_t custom_dmacon(void)  { return dmacon_val; }
uint16_t custom_intena(void)  { return intena_val; }
uint16_t custom_intreq(void)  { return intreq_val; }

uint32_t custom_bplptr(int n)
{
    static const int hi[] = {BPL1PTH, BPL2PTH, BPL3PTH, BPL4PTH, BPL5PTH, BPL6PTH};
    static const int lo[] = {BPL1PTL, BPL2PTL, BPL3PTL, BPL4PTL, BPL5PTL, BPL6PTL};
    return ptr32(hi[n], lo[n]);
}

uint16_t custom_bplcon0(void) { return regs[BPLCON0 >> 1]; }
uint16_t custom_bplcon1(void) { return regs[BPLCON1 >> 1]; }
uint16_t custom_bplcon2(void) { return regs[BPLCON2 >> 1]; }
uint16_t custom_color(int n)  { return regs[(COLOR00 + n * 2) >> 1]; }
uint16_t custom_diwstrt(void) { return regs[DIWSTRT >> 1]; }
uint16_t custom_diwstop(void) { return regs[DIWSTOP >> 1]; }
uint16_t custom_ddfstrt(void) { return regs[DDFSTRT >> 1]; }
uint16_t custom_ddfstop(void) { return regs[DDFSTOP >> 1]; }
uint16_t custom_bpl1mod(void) { return regs[BPL1MOD >> 1]; }
uint16_t custom_bpl2mod(void) { return regs[BPL2MOD >> 1]; }
uint32_t custom_cop1lc(void)  { return ptr32(COP1LCH, COP1LCL); }
uint32_t custom_cop2lc(void)  { return ptr32(COP2LCH, COP2LCL); }

/* Expose the full register array for modules that need direct access
   (blitter reads BLTCON0, copper writes bitplane pointers, etc.) */
uint16_t *custom_regs_ptr(void) { return regs; }
