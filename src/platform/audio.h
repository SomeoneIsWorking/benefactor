#pragma once
/*
 * platform/audio.h  –  SDL2 audio output backed by Paula emulation.
 */

#include <stdint.h>

/* Initialise SDL2 audio, output rate ~44100 Hz stereo float32. */
int  audio_init(void);
void audio_fini(void);

/* Pause / resume (called before / after vblank to avoid starvation). */
void audio_pause(int pause);
