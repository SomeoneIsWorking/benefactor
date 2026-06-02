/* harness/puae_state.h
 * Shared state snapshot used by both the PUAE side and the PC-port side.
 */
#ifndef PUAE_STATE_H
#define PUAE_STATE_H

#include <stdint.h>
#include <stddef.h>

#define HARNESS_COPLIST_WORDS 1412 /* full copper list: 705 instrs = 1410 words + end marker */

/* Paula audio state for one channel, snapped once per frame.
 * All fields are RAW register values (16-bit, as written to $DFF0Ax-$DFF0Dx). */
typedef struct AudioChanSnap {
    uint32_t lc;    /* AUDxLCH:LCL — sample start address (chip RAM) */
    uint16_t len;   /* AUDxLEN — sample length in words */
    uint16_t per;   /* AUDxPER — period (raw register, 1-65535) */
    uint16_t vol;   /* AUDxVOL — volume (0-64) */
} AudioChanSnap;

typedef struct FrameState {
    int      frame;

    /* DMA / copper */
    uint32_t cop1lc;
    uint32_t cop2lc;

    /* Bitplane control */
    uint16_t bplcon0;
    uint16_t bplcon1;
    uint16_t bplcon2;
    int16_t  bpl1mod;
    int16_t  bpl2mod;
    uint16_t diwstrt;
    uint16_t diwstop;
    uint16_t ddfstrt;
    uint16_t ddfstop;

    /* Bitplane pointers (up to 6 planes for OCS) */
    uint32_t bplpt[6];

    /* Sprite pointers (8 hardware sprites) */
    uint32_t sprpt[8];

    /* ECS palette (12-bit × 32) */
    uint16_t palette[32];

    /* First bytes of the copper list the game set this frame */
    uint16_t coplist[HARNESS_COPLIST_WORDS];
    int      coplist_valid;   /* 1 if cop1lc was readable */

    /* CRC32 of full chip RAM (512 KB) — catches bitplane/sprite data divergence.
     * Set to 0 if not available. */
    uint32_t chipram_crc;

    /* CRC32 of the active bitplane data regions (Benefactor-specific).
     * Covers: $025334+4KB (title bpls), $04F6F4+4KB (gameplay bpl1+3),
     *         $070958+8KB (gameplay bpl2).
     * Used as a DIFF trigger: if this differs, bitplane content diverged. */
    uint32_t bpl_data_crc;

    /* Paula audio channels 0-3 */
    AudioChanSnap audio[4];

    /* Sprite animation state from chip RAM — used for per-frame phase tracking */
    uint16_t flip_flop;   /* $0041A2: SWAP/noswap selector toggled by $0041A4 */
    uint16_t sprite_ctr;  /* $0042FC: counter incremented by $0052A4 */
    uint32_t anim_ptr;    /* $0042FE: animation string read pointer, advances on RESET */
} FrameState;


/* Simple CRC32 (ISO 3309 / Ethernet polynomial). */
static inline uint32_t crc32_buf(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
    }
    return ~crc;
}

/* CRC32 over the active bitplane data regions (game-specific fixed addresses).
 * mem/size = chip RAM base and its total size in bytes. */
static inline uint32_t bpl_data_region_crc(const uint8_t *mem, uint32_t size)
{
    static const struct { uint32_t start; uint32_t len; } regions[] = {
        { 0x025334u, 4096u }, /* title screen bitplanes (4-plane, lines 44-89) */
        { 0x04F6F4u, 4096u }, /* gameplay bpl1+3 */
        { 0x070958u, 8192u }, /* gameplay bpl2 (larger — more blitter output here) */
    };
    uint32_t crc = 0xFFFFFFFFu;
    for (int r = 0; r < 3; r++) {
        uint32_t off = regions[r].start;
        uint32_t len = regions[r].len;
        if (off + len > size) continue;
        const uint8_t *p = mem + off;
        for (uint32_t i = 0; i < len; i++) {
            crc ^= p[i];
            for (int b = 0; b < 8; b++)
                crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
        }
    }
    return ~crc;
}

/* ── PUAE side ── */
void puae_snap_state(FrameState *s);          /* implemented via custom.c include */
uint32_t puae_get_cop1lc(void);               /* lightweight boot-detection accessor */
int  puae_dump_chipram(void *buf, int maxbytes); /* copy chip RAM out of PUAE */
int  puae_dump_mem(uint32_t addr, void *buf, int len); /* copy any Amiga addr range (chip+fast) */
void puae_poke_mem(uint32_t addr, const void *buf, int len); /* write any Amiga addr range */
void puae_dump_audio_regs(const char *path);  /* save audio sync state to file */

#endif /* PUAE_STATE_H */
