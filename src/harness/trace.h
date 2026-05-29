/* harness/trace.h
 * Shared write-trace ring buffer used by both the PUAE side and the PC-port
 * side.  When HARNESS_BUILD is defined every interesting memory write is
 * recorded so we can produce a side-by-side diff at divergence time.
 */
#ifndef HARNESS_TRACE_H
#define HARNESS_TRACE_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#define TRACE_SIDE_PUAE 0
#define TRACE_SIDE_PC   1

typedef struct TraceEntry {
    uint32_t addr;       /* chip RAM address written */
    uint32_t old_val;    /* value before write (zero-extended) */
    uint32_t new_val;    /* value after write  (zero-extended) */
    uint32_t pc;         /* M68K PC (or 0 if unknown) */
    uint8_t  size;       /* 1, 2 or 4 */
    uint8_t  side;       /* TRACE_SIDE_PUAE or TRACE_SIDE_PC */
    uint8_t  is_blitter; /* 1 if write came from blitter hardware */
} TraceEntry;

/* Maximum entries in the ring buffer (≈ 32 MB) */
#define TRACE_MAX_ENTRIES (2 * 1024 * 1024)

/* Reset both ring buffers (call at start of each frame) */
void trace_reset(void);

/* Record a write.  Safe to call from any thread (both sides are single-threaded). */
void trace_write(int side, uint32_t addr, uint32_t old_val, uint32_t new_val,
                 uint8_t size, uint32_t pc, uint8_t is_blitter);

/* Convenience wrappers */
static inline void trace_write_puae(uint32_t addr, uint32_t old_v, uint32_t new_v,
                                    uint8_t size, uint32_t pc, uint8_t is_blitter)
{
    trace_write(TRACE_SIDE_PUAE, addr, old_v, new_v, size, pc, is_blitter);
}
static inline void trace_write_pc(uint32_t addr, uint32_t old_v, uint32_t new_v,
                                  uint8_t size, uint32_t pc, uint8_t is_blitter)
{
    trace_write(TRACE_SIDE_PC, addr, old_v, new_v, size, pc, is_blitter);
}

/* Dump all entries for a given address range to fp.
 * If addr_start==addr_end==0, dump the tail of the buffer (last N entries). */
void trace_dump_range(FILE *fp, uint32_t addr_start, uint32_t addr_end, int max_entries);

/* Dump entries around a specific address, interleaving PUAE and PC.
 * Shows the first N entries from each side. */
void trace_dump_side_by_side(FILE *fp, uint32_t addr, int max_entries);

/* Return the number of recorded entries. */
int trace_count(void);

#endif /* HARNESS_TRACE_H */
