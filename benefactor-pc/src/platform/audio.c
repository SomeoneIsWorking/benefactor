#include "audio.h"
#include "../amiga/paula.h"

#include "SDL2/SDL.h"
#include <stdio.h>
#include <string.h>

#define AUDIO_SAMPLE_RATE   44100
#define AUDIO_BUFFER_FRAMES 512     /* frames per callback */

static SDL_AudioDeviceID audio_dev = 0;

/* SDL2 audio callback – runs on a separate thread. */
static void audio_callback(void *userdata, uint8_t *stream, int len)
{
    (void)userdata;
    int n_frames = len / (2 * (int)sizeof(float)); /* stereo float32 */
    paula_fill_audio((float *)stream, n_frames);
}

int audio_init(void)
{
    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL_Init(AUDIO): %s\n", SDL_GetError());
        return -1;
    }

    paula_init(AUDIO_SAMPLE_RATE);

    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq     = AUDIO_SAMPLE_RATE;
    want.format   = AUDIO_F32SYS;
    want.channels = 2;
    want.samples  = AUDIO_BUFFER_FRAMES;
    want.callback = audio_callback;

    audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!audio_dev) {
        fprintf(stderr, "SDL_OpenAudioDevice: %s\n", SDL_GetError());
        return -1;
    }

    SDL_PauseAudioDevice(audio_dev, 0);  /* start playing */
    return 0;
}

void audio_fini(void)
{
    if (audio_dev) {
        SDL_CloseAudioDevice(audio_dev);
        audio_dev = 0;
    }
}

void audio_pause(int pause)
{
    if (audio_dev)
        SDL_PauseAudioDevice(audio_dev, pause);
}
