/* src/port/lockstep_digest.h — the lockstep protocol, in one place.
 *
 * The interpreter and the retired reference product are driven frame by frame
 * against each other by `tools/lockstep.py`: each stops at the end of every
 * presented frame, says what its guest memory hashes to, and waits. The first
 * frame on which the two descriptions differ is where a divergence was born.
 *
 * That comparison only means anything if BOTH products describe the same
 * bytes the same way, so there is exactly one implementation of it — this
 * header. This product includes it from `src/port/lockstep.c`;
 * `tools/oracle_diff.py` copies this same file into the reference worktree and
 * makes its `hw.c` include it. Never fork the algorithm into the reference
 * patch: a hash that differs by implementation reports a divergence on frame
 * zero and every frame after it.
 *
 * It is deliberately free-standing — no product headers, no logger, no
 * configuration — because the reference is a different program with a
 * different engine, and the only thing the two share is C and the guest's
 * memory. The caller supplies the memory and the frame's hardware state.
 *
 * Region digests rather than the memory itself: eight megabytes down a pipe
 * every frame is not a measurement, it is a bottleneck. A region hash says
 * WHICH 32K changed; the byte-exact answer comes from the one full dump the
 * driver asks for at the frame that diverged, over the same channel. */
#ifndef BENEFACTOR_PORT_LOCKSTEP_DIGEST_H
#define BENEFACTOR_PORT_LOCKSTEP_DIGEST_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped whenever the line format changes, so a stale build says so instead of
 * being diffed against a fresh one. */
#define LOCKSTEP_PROTOCOL_VERSION 2

/* 8MB of guest memory in 32K regions. Finer regions cost line length and
 * nothing else; coarser ones make the first report vaguer. */
#define LOCKSTEP_REGIONS 256u

/* What the frame showed and played, beside the memory: hardware state that
 * never lands in guest memory and so cannot be seen in a region digest. */
typedef struct {
    int frame;
    uint32_t cop1lc;
    uint32_t palette; /* hash of the copper list's COLORxx writes */
    uint32_t audio_pointer[4];
    uint32_t audio_period[4];
    uint32_t audio_volume[4];
    uint32_t dmacon;    /* DMACON without the audio enables */
    uint32_t audio_dma; /* AUD0..3 DMA enables, reported apart from the rest */
} LockstepFrame;

/* FNV-1a, widened to a word at a time: this runs over the whole address space
 * twice a frame, and byte-at-a-time FNV over 8MB is most of a frame's budget.
 * The extra shift-xor is what keeps the word-wide version sensitive to where
 * inside the word a byte moved. */
static inline uint64_t lockstep_hash(const unsigned char *bytes, size_t count) {
    uint64_t hash = 1469598103934665603ULL;
    size_t index = 0;
    for (; index + 8u <= count; index += 8u) {
        uint64_t word;
        memcpy(&word, bytes + index, sizeof word);
        hash ^= word;
        hash *= 1099511628211ULL;
        hash ^= hash >> 29u;
    }
    for (; index < count; index++) {
        hash ^= (uint64_t)bytes[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

/* Continue a hash over another span, so a region split around an excluded
 * range still folds down to one number. */
static inline uint64_t lockstep_hash_into(uint64_t hash, const unsigned char *bytes, size_t count) {
    size_t index = 0;
    for (; index + 8u <= count; index += 8u) {
        uint64_t word;
        memcpy(&word, bytes + index, sizeof word);
        hash ^= word;
        hash *= 1099511628211ULL;
        hash ^= hash >> 29u;
    }
    for (; index < count; index++) {
        hash ^= (uint64_t)bytes[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

/* Guest addresses left out of the comparison, because the two products differ
 * there for a reason that is not a fault — see STRUCTURAL_DIFFERENCES in
 * tools/lockstep.py, which is where the reasons are written down. BOTH
 * products are given the same spec, so both skip the same bytes and their
 * digests stay comparable. */
#define LOCKSTEP_MAX_RANGES 16
typedef struct {
    size_t count;
    uint32_t start[LOCKSTEP_MAX_RANGES];
    uint32_t end[LOCKSTEP_MAX_RANGES]; /* one past the last excluded byte */
} LockstepRanges;

/* Parse `78-7B,78000-7FFFF` (hex, both ends inclusive). Returns how many
 * ranges were read; an absent or empty spec is none, which is the normal case. */
static inline size_t lockstep_parse_ranges(LockstepRanges *ranges, const char *spec) {
    memset(ranges, 0, sizeof *ranges);
    if (spec == NULL)
        return 0u;
    const char *cursor = spec;
    while (*cursor && ranges->count < LOCKSTEP_MAX_RANGES) {
        char *end = NULL;
        const unsigned long low = strtoul(cursor, &end, 16);
        if (end == cursor)
            break;
        unsigned long high = low;
        if (*end == '-')
            high = strtoul(end + 1, &end, 16);
        ranges->start[ranges->count] = (uint32_t)low;
        ranges->end[ranges->count] = (uint32_t)(high + 1u);
        ranges->count++;
        while (*end == ',' || *end == ' ')
            end++;
        cursor = end;
    }
    return ranges->count;
}

/* Hash `bytes` of memory into `regions` equal digests, skipping the excluded
 * ranges. A trailing partial region (when the size does not divide) is folded
 * into the last one. */
static inline void lockstep_digest_regions_excluding(const unsigned char *memory, size_t bytes,
                                                     uint64_t *digests, size_t regions,
                                                     const LockstepRanges *skip) {
    if (regions == 0u)
        return;
    const size_t span = bytes / regions;
    for (size_t index = 0; index < regions; index++) {
        const size_t start = index * span;
        const size_t stop = (index + 1u == regions) ? bytes : (start + span);
        uint64_t hash = 1469598103934665603ULL;
        size_t cursor = start;
        while (cursor < stop) {
            /* The next excluded range that begins at or after the cursor. */
            size_t next_start = stop, next_end = stop;
            for (size_t range = 0; skip && range < skip->count; range++) {
                if (skip->end[range] <= cursor || skip->start[range] >= stop)
                    continue;
                const size_t begins = skip->start[range] < cursor ? cursor : skip->start[range];
                if (begins < next_start) {
                    next_start = begins;
                    next_end = skip->end[range] > stop ? stop : skip->end[range];
                }
            }
            if (next_start > cursor)
                hash = lockstep_hash_into(hash, memory + cursor, next_start - cursor);
            cursor = (next_end > next_start) ? next_end : next_start;
        }
        digests[index] = hash;
    }
}

/* No exclusions — the plain form, and what the tests hold the hash to. */
static inline void lockstep_digest_regions(const unsigned char *memory, size_t bytes,
                                           uint64_t *digests, size_t regions) {
    lockstep_digest_regions_excluding(memory, bytes, digests, regions, NULL);
}

/* The copper list's colour writes, hashed — the fade instrument. Identical to
 * src/port/frame_signature.c's palette_hash, and to the reference's copy of
 * it, on purpose: same bytes, same number. */
#define LOCKSTEP_COPLIST_SCAN_WORDS 2048u
static inline uint32_t lockstep_palette_hash(const unsigned char *memory, uint32_t cop1lc) {
    uint32_t hash = 2166136261u;
    const uint32_t list = cop1lc & 0xFFFFFFu;
    if (!list || memory == NULL)
        return hash;
    for (uint32_t index = 0; index + 1u < LOCKSTEP_COPLIST_SCAN_WORDS; index += 2u) {
        const unsigned char *word = memory + list + index * 2u;
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

/* Format the hello line. Returns its length, or -1 if it would not fit. */
static inline int lockstep_format_hello(char *out, size_t cap, size_t regions, size_t bytes) {
    const int written = snprintf(out, cap, "lockstep hello version=%d regions=%zu bytes=%zu",
                                 LOCKSTEP_PROTOCOL_VERSION, regions, bytes);
    return (written > 0 && (size_t)written < cap) ? written : -1;
}

/* Format one frame's line. Returns its length, or -1 if it would not fit —
 * never a truncated line, which would read to the driver as a real difference
 * in the regions the truncation removed. */
static inline int lockstep_format_frame(char *out, size_t cap, const LockstepFrame *state,
                                        const uint64_t *digests, size_t regions) {
    int written =
        snprintf(out, cap,
                 "lockstep frame=%d cop1lc=%06X pal=%08X "
                 "alc=%06X,%06X,%06X,%06X aper=%u,%u,%u,%u avol=%u,%u,%u,%u "
                 "adma=%03X aaud=%X mem=",
                 state->frame, state->cop1lc, state->palette, state->audio_pointer[0],
                 state->audio_pointer[1], state->audio_pointer[2], state->audio_pointer[3],
                 state->audio_period[0], state->audio_period[1], state->audio_period[2],
                 state->audio_period[3], state->audio_volume[0], state->audio_volume[1],
                 state->audio_volume[2], state->audio_volume[3], state->dmacon, state->audio_dma);
    if (written <= 0 || (size_t)written >= cap)
        return -1;
    for (size_t index = 0; index < regions; index++) {
        const int more = snprintf(out + written, cap - (size_t)written, "%s%016llX",
                                  index ? "," : "", (unsigned long long)digests[index]);
        if (more <= 0 || (size_t)(written + more) >= cap)
            return -1;
        written += more;
    }
    return written;
}

/* ── The channel ─────────────────────────────────────────────────────────────
 *
 * Two inherited file descriptors, given as "<read>:<write>" — the driver's own
 * pipe pair. Not a socket and not a file: the product must BLOCK on the read,
 * because that block is what holds it at the frame boundary while the other
 * product catches up. Nothing else in either product does that, which is why
 * this is the whole mechanism.
 *
 * Losing the channel disables it rather than killing the game: the driver has
 * either finished with this product or gone away, and a product that keeps
 * running is one a person can still look at. */
typedef struct {
    int enabled;
    FILE *incoming;
    FILE *outgoing;
} LockstepChannel;

/* Parse the spec, say hello, and return 1 when the channel is live. An absent
 * or malformed spec is the normal case — lockstep is off — and is not an
 * error. */
static inline int lockstep_open(LockstepChannel *channel, const char *spec, size_t regions,
                                size_t bytes) {
    memset(channel, 0, sizeof *channel);
    if (spec == NULL || spec[0] == 0)
        return 0;
    char *end = NULL;
    const long read_fd = strtol(spec, &end, 10);
    if (end == NULL || *end != ':')
        return 0;
    const long write_fd = strtol(end + 1, &end, 10);
    if (end == NULL || *end != 0 || read_fd < 0 || write_fd < 0)
        return 0;
    channel->incoming = fdopen((int)read_fd, "r");
    channel->outgoing = fdopen((int)write_fd, "w");
    if (channel->incoming == NULL || channel->outgoing == NULL)
        return 0;
    char hello[128];
    if (lockstep_format_hello(hello, sizeof hello, regions, bytes) < 0)
        return 0;
    fprintf(channel->outgoing, "%s\n", hello);
    fflush(channel->outgoing);
    channel->enabled = 1;
    return 1;
}

static inline void lockstep_close(LockstepChannel *channel) {
    if (channel->incoming)
        fclose(channel->incoming);
    if (channel->outgoing)
        fclose(channel->outgoing);
    channel->incoming = NULL;
    channel->outgoing = NULL;
    channel->enabled = 0;
}

/* Report this frame, then serve the driver until it says `go`. `dump <path>`
 * writes the whole guest address space and keeps waiting — that is how the
 * driver turns "these 32K differ" into an address, at the one frame where it
 * matters. */
static inline void lockstep_exchange(LockstepChannel *channel, const char *line,
                                     const unsigned char *memory, size_t bytes) {
    if (!channel->enabled)
        return;
    fprintf(channel->outgoing, "%s\n", line);
    fflush(channel->outgoing);
    for (;;) {
        char command[640];
        if (fgets(command, (int)sizeof command, channel->incoming) == NULL) {
            channel->enabled = 0; /* the driver is gone; run on unmeasured */
            return;
        }
        char *newline = strchr(command, '\n');
        if (newline)
            *newline = 0;
        if (strncmp(command, "dump ", 5u) == 0) {
            FILE *sink = fopen(command + 5, "wb");
            const size_t wrote = sink ? fwrite(memory, 1u, bytes, sink) : 0u;
            if (sink)
                fclose(sink);
            fprintf(channel->outgoing, "lockstep dumped bytes=%zu\n", wrote);
            fflush(channel->outgoing);
            continue;
        }
        if (strcmp(command, "stop") == 0) {
            channel->enabled = 0;
            return;
        }
        return; /* `go`, or anything else: carry on */
    }
}

#ifdef __cplusplus
}
#endif

#endif /* BENEFACTOR_PORT_LOCKSTEP_DIGEST_H */
