#pragma once
/*
 * amiga/paula.h  –  Paula chip: 4-channel audio DMA.
 *
 * We model the four audio channels (DMA-driven) and expose a callback
 * that the SDL2 audio thread uses to pull samples.
 */

#include <stdint.h>

/* Called once at startup. */
void paula_init(int output_sample_rate);

/* Called by custom.c when DMACON is updated. */
void paula_dmacon_update(uint16_t dmacon);

/* Called by custom.c on any audio register write. */
void paula_reg_write(uint32_t reg, uint16_t val, const uint16_t *all_regs);

/*
 * Fill `out` with `n` stereo float samples [-1, 1].
 * Called from the SDL2 audio callback (separate thread – lock held by caller).
 */
void paula_fill_audio(float *out, int n);
