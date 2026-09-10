#include "port/frame_signature.h"

#include <stdint.h>

#include "common/game_state.h"
#include "common/log.h"
#include "engine/hw.h"
#include "runtime/guest_runtime.h"

/* AUD0..AUD3 base offsets in the custom-chip register file. */
static const unsigned kAudioBase[4] = {0x0A0u, 0x0B0u, 0x0C0u, 0x0D0u};

/* The fade instrument reads the COPPER LIST, not the s_palette shadow: the
 * renderer takes the frame's colours from the copper writes and only falls
 * back to the shadow for a register the list never writes. Measured against
 * the reference product, the shadow did not change once in 9000 frames while
 * the logos were visibly fading. Scan the list for MOVEs to COLOR00..31 and
 * fold register and value into the hash. */
#define COPLIST_SCAN_WORDS 2048u

static uint32_t palette_hash(void) {
    const uint32_t list = hw_get_cop1lc() & 0xFFFFFFu;
    uint32_t hash = 2166136261u;
    if (!list || !g_mem)
        return hash;
    for (uint32_t i = 0; i + 1 < COPLIST_SCAN_WORDS; i += 2) {
        const uint8_t *word = g_mem + list + i * 2u;
        const uint16_t control = (uint16_t)((word[0] << 8) | word[1]);
        const uint16_t value = (uint16_t)((word[2] << 8) | word[3]);
        if (control == 0xFFFFu)
            break;
        if (control & 1u)
            continue; /* WAIT/SKIP — the colours after it still count */
        const uint16_t reg = control & 0x01FEu;
        if (reg < 0x180u || reg > 0x1BEu)
            continue;
        hash ^= (uint32_t)reg;
        hash *= 16777619u;
        hash ^= (uint32_t)(value & 0x0FFFu);
        hash *= 16777619u;
    }
    return hash;
}

static uint32_t audio_pointer(unsigned channel) {
    const unsigned base = kAudioBase[channel];
    return (((uint32_t)s_regs[base >> 1] << 16) | s_regs[(base + 2) >> 1]) & 0xFFFFFFu;
}

void pc_note_frame_signature(void) {
    uint32_t lc[4], per[4], vol[4];
    for (unsigned c = 0; c < 4; c++) {
        lc[c] = audio_pointer(c);
        per[c] = s_regs[(kAudioBase[c] + 6) >> 1];
        vol[c] = s_regs[(kAudioBase[c] + 8) >> 1];
    }
    const uint32_t pal = palette_hash();
    const uint32_t dma = (uint32_t)(s_dmacon & 0x020Fu);

    /* One emitted line per change: the stream IS the melody and the fade. */
    static uint32_t last_pal = 0xFFFFFFFFu, last_dma = 0xFFFFFFFFu;
    static uint32_t last_lc[4], last_per[4], last_vol[4];
    int same = (pal == last_pal) && (dma == last_dma);
    for (unsigned c = 0; c < 4 && same; c++)
        same = (lc[c] == last_lc[c]) && (per[c] == last_per[c]) && (vol[c] == last_vol[c]);
    if (same)
        return;
    last_pal = pal;
    last_dma = dma;
    for (unsigned c = 0; c < 4; c++) {
        last_lc[c] = lc[c];
        last_per[c] = per[c];
        last_vol[c] = vol[c];
    }

    benefactor_log_write(BENEFACTOR_LOG_INFO, "sig",
                         "frame=%d pal=%08X alc=%06X,%06X,%06X,%06X "
                         "aper=%u,%u,%u,%u avol=%u,%u,%u,%u adma=%03X",
                         hw_get_frame_num(), pal, lc[0], lc[1], lc[2], lc[3], per[0], per[1],
                         per[2], per[3], vol[0], vol[1], vol[2], vol[3], dma);
}
