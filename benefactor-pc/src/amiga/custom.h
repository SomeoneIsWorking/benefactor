#pragma once
/*
 * amiga/custom.h  –  OCS/ECS custom chip register definitions and interface.
 *
 * The custom chips occupy $DFF000–$DFFFFF.
 * Registers follow the standard Amiga Hardware Reference Manual layout.
 */

#include <stdint.h>

/* ── Register offsets from $DFF000 ───────────────────────────────────────── */
#define BLTDDAT   0x000
#define DMACONR   0x002
#define VPOSR     0x004
#define VHPOSR    0x006
#define DSKDATR   0x008
#define JOY0DAT   0x00A
#define JOY1DAT   0x00C
#define CLXDAT    0x00E
#define ADKCONR   0x010
#define POT0DAT   0x012
#define POT1DAT   0x014
#define POTINP    0x016
#define SERDATR   0x018
#define DSKBYTR   0x01A
#define INTENAR   0x01C
#define INTREQR   0x01E
/* write-only from here */
#define DSKPTH    0x020
#define DSKPTL    0x022
#define DSKLEN    0x024
#define DSKDAT    0x026
#define REFPTR    0x028
#define VPOSW     0x02A
#define VHPOSW    0x02C
#define COPCON    0x02E
#define SERDAT    0x030
#define SERPER    0x032
#define POTGO     0x034
#define JOYTEST   0x036
#define STREQU    0x038
#define STRVBL    0x03A
#define STRHOR    0x03C
#define STRLONG   0x03E
#define BLTCON0   0x040
#define BLTCON1   0x042
#define BLTAFWM   0x044
#define BLTALWM   0x046
#define BLTCPTH   0x048
#define BLTCPTL   0x04A
#define BLTBPTH   0x04C
#define BLTBPTL   0x04E
#define BLTAPTH   0x050
#define BLTAPTL   0x052
#define BLTDPTH   0x054
#define BLTDPTL   0x056
#define BLTSIZE   0x058
#define BLTCON0L  0x05A   /* ECS */
#define BLTSIZV   0x05C   /* ECS */
#define BLTSIZH   0x05E   /* ECS */
#define BLTCMOD   0x060
#define BLTBMOD   0x062
#define BLTAMOD   0x064
#define BLTDMOD   0x066
#define BLTCDAT   0x070
#define BLTBDAT   0x072
#define BLTADAT   0x074
#define SPRHDAT   0x078   /* ECS */
#define BPLHDAT   0x07A   /* ECS */
#define LISAID    0x07C   /* ECS */
#define DENISEID  0x07C
#define DSKSYNC   0x07E
#define COP1LCH   0x080
#define COP1LCL   0x082
#define COP2LCH   0x084
#define COP2LCL   0x086
#define COPJMP1   0x088
#define COPJMP2   0x08A
#define COPINS    0x08C
#define DIWSTRT   0x08E
#define DIWSTOP   0x090
#define DDFSTRT   0x092
#define DDFSTOP   0x094
#define DMACON    0x096
#define CLXCON    0x098
#define INTENA    0x09A
#define INTREQ    0x09C
#define ADKCON    0x09E
/* Audio channels */
#define AUD0LCH   0x0A0
#define AUD0LCL   0x0A2
#define AUD0LEN   0x0A4
#define AUD0PER   0x0A6
#define AUD0VOL   0x0A8
#define AUD0DAT   0x0AA
#define AUD1LCH   0x0B0
#define AUD1LCL   0x0B2
#define AUD1LEN   0x0B4
#define AUD1PER   0x0B6
#define AUD1VOL   0x0B8
#define AUD1DAT   0x0BA
#define AUD2LCH   0x0C0
#define AUD2LCL   0x0C2
#define AUD2LEN   0x0C4
#define AUD2PER   0x0C6
#define AUD2VOL   0x0C8
#define AUD2DAT   0x0CA
#define AUD3LCH   0x0D0
#define AUD3LCL   0x0D2
#define AUD3LEN   0x0D4
#define AUD3PER   0x0D6
#define AUD3VOL   0x0D8
#define AUD3DAT   0x0DA
/* Bitplane pointers */
#define BPL1PTH   0x0E0
#define BPL1PTL   0x0E2
#define BPL2PTH   0x0E4
#define BPL2PTL   0x0E6
#define BPL3PTH   0x0E8
#define BPL3PTL   0x0EA
#define BPL4PTH   0x0EC
#define BPL4PTL   0x0EE
#define BPL5PTH   0x0F0
#define BPL5PTL   0x0F2
#define BPL6PTH   0x0F4
#define BPL6PTL   0x0F6
/* Display control */
#define BPLCON0   0x100
#define BPLCON1   0x102
#define BPLCON2   0x104
#define BPLCON3   0x106   /* ECS */
#define BPL1MOD   0x108
#define BPL2MOD   0x10A
#define BPL1DAT   0x110
#define BPL2DAT   0x112
#define BPL3DAT   0x114
#define BPL4DAT   0x116
#define BPL5DAT   0x118
#define BPL6DAT   0x11A
/* Sprite pointers */
#define SPR0PTH   0x120
#define SPR0PTL   0x122
#define SPR1PTH   0x124
#define SPR1PTL   0x126
#define SPR2PTH   0x128
#define SPR2PTL   0x12A
#define SPR3PTH   0x12C
#define SPR3PTL   0x12E
#define SPR4PTH   0x130
#define SPR4PTL   0x132
#define SPR5PTH   0x134
#define SPR5PTL   0x136
#define SPR6PTH   0x138
#define SPR6PTL   0x13A
#define SPR7PTH   0x13C
#define SPR7PTL   0x13E
/* Sprite data */
#define SPR0DATA  0x140
#define SPR0DATB  0x142
/* ... (omitted for brevity, use addr arithmetic) */
/* Color table */
#define COLOR00   0x180
/* … COLOR01–COLOR31 at COLOR00 + n*2 */
/* ECS */
#define HTOTAL    0x1C0
#define HSSTOP    0x1C2
#define HBSTRT    0x1C4
#define HBSTOP    0x1C6
#define VTOTAL    0x1C8
#define VSSTOP    0x1CA
#define VBSTRT    0x1CC
#define VBSTOP    0x1CE
#define BEAMCON0  0x1DC
#define HSSTRT    0x1DE
#define VSSTRT    0x1E0
#define HCENTER   0x1E2
#define DIWHIGH   0x1E4   /* ECS */

/* ── DMACON / INTENA bits ──────────────────────────────────────────────────── */
#define DMAEN     (1<<9)
#define BPLEN     (1<<8)
#define COPEN     (1<<7)
#define BLTEN     (1<<6)
#define SPREN     (1<<5)
#define DSKEN     (1<<4)
#define AUD3EN    (1<<3)
#define AUD2EN    (1<<2)
#define AUD1EN    (1<<1)
#define AUD0EN    (1<<0)

#define INTEN     (1<<14)  /* master interrupt enable */
#define EXTER     (1<<13)
#define DSKSYN    (1<<12)
#define RBF       (1<<11)
#define AUD3_INT  (1<<10)
#define AUD2_INT  (1<<9)
#define AUD1_INT  (1<<8)
#define AUD0_INT  (1<<7)
#define BLIT      (1<<6)
#define VERTB     (1<<5)  /* vertical blank */
#define COPER     (1<<4)
#define PORTS     (1<<3)
#define SOFT      (1<<2)
#define DSKBLK    (1<<1)
#define TBE       (1<<0)

/* ── Public API ──────────────────────────────────────────────────────────── */
void     custom_init(void);
uint16_t custom_read(uint32_t reg);
void     custom_write(uint32_t reg, uint16_t val);

/* Called each scanline by the main loop. */
void custom_tick_scanline(int scanline);

/* Returns non-zero when a vertical-blank interrupt is pending. */
int custom_vblank_pending(void);
void custom_vblank_ack(void);

/* State accessors used by display/audio modules. */
uint16_t custom_dmacon(void);
uint16_t custom_intena(void);
uint16_t custom_intreq(void);
uint32_t custom_bplptr(int n);   /* n = 0..5 */
uint16_t custom_bplcon0(void);
uint16_t custom_bplcon1(void);
uint16_t custom_bplcon2(void);
uint16_t custom_color(int n);    /* n = 0..31 */
uint16_t custom_diwstrt(void);
uint16_t custom_diwstop(void);
uint16_t custom_ddfstrt(void);
uint16_t custom_ddfstop(void);
uint16_t custom_bpl1mod(void);
uint16_t custom_bpl2mod(void);
uint32_t custom_cop1lc(void);
uint32_t custom_cop2lc(void);
