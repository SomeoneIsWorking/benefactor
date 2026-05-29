#include "paula.h"
#include "custom.h"
#include "memory.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

/*
 * Paula audio:
 *   4 channels, each with: sample pointer, length (words), period, volume.
 *   Sample rate = 3546895 / period  (PAL clock)
 *   Volume is 0..64.
 *   Channels 0 and 3 → left; 1 and 2 → right.
 */

#define PAL_CLOCK   3546895.0

typedef struct {
    uint32_t  base;       /* current DMA pointer */
    uint32_t  reload;     /* start address (reloaded on wrap) */
    uint16_t  len;        /* length in words */
    uint16_t  len_reload;
    uint16_t  period;     /* divider for sample rate */
    uint8_t   vol;        /* 0..64 */
    int       enabled;

    double    phase;      /* fractional position within current sample */
    double    step;       /* advance per output sample */
    int8_t    cur_sample; /* current output value */
    uint32_t  pos;        /* byte position within DMA buffer */
    uint32_t  end;        /* byte end of DMA buffer */
} AudChan;

static AudChan chans[4];
static int output_rate;

/* ── Init ────────────────────────────────────────────────────────────────── */

void paula_init(int rate)
{
    output_rate = rate;
    memset(chans, 0, sizeof(chans));
    for (int i = 0; i < 4; i++)
        chans[i].period = 400;  /* default safe period */
}

/* ── DMACON update ───────────────────────────────────────────────────────── */

void paula_dmacon_update(uint16_t dmacon)
{
    for (int i = 0; i < 4; i++) {
        int was_enabled = chans[i].enabled;
        chans[i].enabled = (dmacon >> i) & 1;

        /* Channel just enabled: (re)start DMA from reload address */
        if (chans[i].enabled && !was_enabled) {
            chans[i].base  = chans[i].reload;
            chans[i].pos   = chans[i].base;
            chans[i].end   = chans[i].base + (uint32_t)chans[i].len_reload * 2;
            chans[i].phase = 0.0;
            uint16_t per   = chans[i].period ? chans[i].period : 1;
            chans[i].step  = PAL_CLOCK / (per * output_rate);
        }
    }
}

/* ── Register writes ─────────────────────────────────────────────────────── */

void paula_reg_write(uint32_t reg, uint16_t val, const uint16_t *all_regs)
{
    (void)all_regs;

    /* channel index from register offset */
    int ch = -1;
    uint32_t base_offs = 0;
    if      (reg >= AUD0LCH && reg <= AUD0DAT) { ch = 0; base_offs = AUD0LCH; }
    else if (reg >= AUD1LCH && reg <= AUD1DAT) { ch = 1; base_offs = AUD1LCH; }
    else if (reg >= AUD2LCH && reg <= AUD2DAT) { ch = 2; base_offs = AUD2LCH; }
    else if (reg >= AUD3LCH && reg <= AUD3DAT) { ch = 3; base_offs = AUD3LCH; }
    if (ch < 0) return;

    uint32_t sub = reg - base_offs;
    switch (sub) {
    case 0:  /* LCH */ chans[ch].reload  = (uint32_t)val << 16 | (chans[ch].reload & 0xFFFF); break;
    case 2:  /* LCL */ chans[ch].reload  = (chans[ch].reload & 0xFFFF0000) | val; break;
    case 4:  /* LEN */ chans[ch].len_reload = val; break;
    case 6:  /* PER */
        chans[ch].period = val ? val : 1;
        chans[ch].step   = PAL_CLOCK / (chans[ch].period * output_rate);
        break;
    case 8:  /* VOL */ chans[ch].vol = (uint8_t)(val > 64 ? 64 : val); break;
    default: break;
    }
}

/* ── Audio fill (called from SDL2 audio thread) ──────────────────────────── */

void paula_fill_audio(float *out, int n)
{
    for (int s = 0; s < n; s++) {
        float left  = 0.0f;
        float right = 0.0f;

        for (int ch = 0; ch < 4; ch++) {
            AudChan *c = &chans[ch];
            if (!c->enabled || c->period == 0)
                continue;

            /* Advance phase */
            c->phase += c->step;
            while (c->phase >= 1.0) {
                c->phase -= 1.0;
                /* Fetch next sample byte */
                if (c->pos >= c->end) {
                    /* Wrap around: reload from base address */
                    c->pos = c->base;
                    c->end = c->base + (uint32_t)c->len_reload * 2;
                }
                if (c->pos < c->end) {
                    c->cur_sample = (int8_t)mem_read8(c->pos);
                    c->pos++;
                }
            }

            float sample = (float)c->cur_sample / 128.0f * ((float)c->vol / 64.0f);
            /* Channels 0 and 3 → left; 1 and 2 → right */
            if (ch == 0 || ch == 3) left  += sample * 0.5f;
            else                    right += sample * 0.5f;
        }

        /* Clamp */
        if (left  >  1.0f) left  =  1.0f;
        if (left  < -1.0f) left  = -1.0f;
        if (right >  1.0f) right =  1.0f;
        if (right < -1.0f) right = -1.0f;

        out[s * 2]     = left;
        out[s * 2 + 1] = right;
    }
}
