#include "port/lockstep.h"

#include "common/game_state.h"
#include "common/log.h"
#include "engine/hw.h"
#include "port/config.h"
#include "port/lockstep_digest.h"
#include "runtime/guest_runtime.h"

/* AUD0..AUD3 base offsets in the custom-chip register file — the same four the
 * frame signature reads (src/port/frame_signature.c). */
static const unsigned kAudioBase[4] = {0x0A0u, 0x0B0u, 0x0C0u, 0x0D0u};

/* Room for the fixed head plus "%016llX," per region. */
#define LOCKSTEP_LINE_BYTES (256u + LOCKSTEP_REGIONS * 17u)

static struct {
    int loaded;
    LockstepChannel channel;
    LockstepRanges excluded;
} s_lockstep;

static void lockstep_load(void) {
    char spec[64];
    s_lockstep.loaded = 1;
    if (!pc_cfg_string("lockstep", "", spec, sizeof spec) || spec[0] == 0)
        return;
    if (!lockstep_open(&s_lockstep.channel, spec, LOCKSTEP_REGIONS, (size_t)RT_MEM_SIZE)) {
        benefactor_log_write(BENEFACTOR_LOG_ERROR, "lockstep",
                             "lockstep=%s is not a <readfd>:<writefd> pair this process "
                             "inherited; running unmeasured",
                             spec);
        return;
    }
    char excluded[256];
    pc_cfg_string("lockstep_ignore", "", excluded, sizeof excluded);
    lockstep_parse_ranges(&s_lockstep.excluded, excluded);
    benefactor_log_write(BENEFACTOR_LOG_INFO, "lockstep",
                         "holding every frame for the driver: %u regions over %u bytes, "
                         "%u address ranges left out (%s)",
                         (unsigned)LOCKSTEP_REGIONS, (unsigned)RT_MEM_SIZE,
                         (unsigned)s_lockstep.excluded.count, excluded[0] ? excluded : "none");
}

static void lockstep_gather(LockstepFrame *state) {
    state->frame = hw_get_frame_num();
    state->cop1lc = hw_get_cop1lc() & 0xFFFFFFu;
    state->palette = lockstep_palette_hash(g_mem, state->cop1lc);
    for (unsigned channel = 0; channel < 4u; channel++) {
        const unsigned base = kAudioBase[channel];
        state->audio_pointer[channel] =
            (((uint32_t)s_regs[base >> 1] << 16) | s_regs[(base + 2u) >> 1]) & 0xFFFFFFu;
        state->audio_period[channel] = s_regs[(base + 6u) >> 1];
        state->audio_volume[channel] = s_regs[(base + 8u) >> 1];
    }
    state->dmacon = (uint32_t)(s_dmacon & 0x0200u);
    state->audio_dma = (uint32_t)(s_dmacon & 0x000Fu);
}

void pc_lockstep_frame(void) {
    if (!s_lockstep.loaded)
        lockstep_load();
    if (!s_lockstep.channel.enabled || g_mem == NULL)
        return;

    static uint64_t digests[LOCKSTEP_REGIONS];
    static char line[LOCKSTEP_LINE_BYTES];
    lockstep_digest_regions_excluding(g_mem, (size_t)RT_MEM_SIZE, digests, LOCKSTEP_REGIONS,
                                      &s_lockstep.excluded);
    LockstepFrame state;
    lockstep_gather(&state);
    if (lockstep_format_frame(line, sizeof line, &state, digests, LOCKSTEP_REGIONS) < 0) {
        /* A truncated line would read to the driver as a real difference in
         * whatever it cut off, so say nothing rather than something wrong. */
        benefactor_log_write(BENEFACTOR_LOG_ERROR, "lockstep",
                             "frame %d does not fit in %u bytes; stopping the comparison",
                             state.frame, (unsigned)sizeof line);
        lockstep_close(&s_lockstep.channel);
        return;
    }
    lockstep_exchange(&s_lockstep.channel, line, g_mem, (size_t)RT_MEM_SIZE);
}
