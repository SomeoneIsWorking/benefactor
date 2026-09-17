#include "port/frame_signature.h"

#include <stdint.h>

#include "common/game_state.h"
#include "common/log.h"
#include "engine/hw.h"
#include "port/frame_accounting.h"
#include "port/lockstep_digest.h"
#include "runtime/guest_runtime.h"

/* AUD0..AUD3 base offsets in the custom-chip register file. */
static const unsigned kAudioBase[4] = {0x0A0u, 0x0B0u, 0x0C0u, 0x0D0u};

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
    /* The fade instrument reads the COPPER LIST, not the s_palette shadow: the
     * renderer takes the frame's colours from the copper writes and only
     * falls back to the shadow for a register the list never writes.
     * Measured against the reference product, the shadow did not change once
     * in 9000 frames while the logos were visibly fading.
     * `lockstep_palette_hash` owns the scan (it must already stay
     * byte-identical to the reference's copy of the same header), so this
     * file calls it instead of keeping a second copy. */
    const uint32_t pal = g_mem ? lockstep_palette_hash(g_mem, hw_get_cop1lc()) : 2166136261u;
    const uint32_t dma = (uint32_t)(s_dmacon & 0x020Fu);

    /* One emitted line per change — and while music plays, that is every frame,
     * because the stream IS the melody and the fade. It is a differential
     * diagnostic, so it belongs below the level a host logs by default: at info
     * it buries the host's own output, which in a browser is its only readout. */
    static uint32_t last_pal = 0xFFFFFFFFu, last_dma = 0xFFFFFFFFu;
    static uint32_t last_lc[4], last_per[4], last_vol[4];
    int same = (pal == last_pal) && (dma == last_dma);
    for (unsigned c = 0; c < 4 && same; c++) {
        same = (lc[c] == last_lc[c]) && (per[c] == last_per[c]) && (vol[c] == last_vol[c]);
    }
    if (same) {
        return;
    }
    last_pal = pal;
    last_dma = dma;
    for (unsigned c = 0; c < 4; c++) {
        last_lc[c] = lc[c];
        last_per[c] = per[c];
        last_vol[c] = vol[c];
    }

    benefactor_log_write(BENEFACTOR_LOG_DEBUG, "sig",
                         "frame=%u pal=%08X alc=%06X,%06X,%06X,%06X "
                         "aper=%u,%u,%u,%u avol=%u,%u,%u,%u adma=%03X",
                         pc_presented_frame_num(), pal, lc[0], lc[1], lc[2], lc[3], per[0], per[1],
                         per[2], per[3], vol[0], vol[1], vol[2], vol[3], dma);
}
