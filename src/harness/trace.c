/* harness/trace.c – Ring-buffer write trace shared between PUAE and PC */
#include "trace.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static TraceEntry s_buf[TRACE_MAX_ENTRIES];
static volatile uint32_t s_head = 0;   /* monotonically increasing index */

void trace_reset(void)
{
    s_head = 0;
}

void trace_write(int side, uint32_t addr, uint32_t old_val, uint32_t new_val,
                 uint8_t size, uint32_t pc, uint8_t is_blitter)
{
    uint32_t idx = s_head;
    if (idx < TRACE_MAX_ENTRIES) {
        TraceEntry *e = &s_buf[idx];
        e->addr       = addr;
        e->old_val    = old_val;
        e->new_val    = new_val;
        e->pc         = pc;
        e->size       = size;
        e->side       = (uint8_t)side;
        e->is_blitter = is_blitter;
    }
    s_head++;
}

int trace_count(void)
{
    return (s_head > TRACE_MAX_ENTRIES) ? TRACE_MAX_ENTRIES : (int)s_head;
}

static uint32_t _idx(uint32_t i)
{
    /* If we've wrapped, return the wrapped index; otherwise return as-is */
    if (s_head > TRACE_MAX_ENTRIES)
        return i % TRACE_MAX_ENTRIES;
    return i;
}

void trace_dump_range(FILE *fp, uint32_t addr_start, uint32_t addr_end, int max_entries)
{
    uint32_t count = (uint32_t)trace_count();
    if (count == 0) {
        fprintf(fp, "  (trace buffer empty)\n");
        return;
    }
    int printed = 0;
    for (uint32_t i = 0; i < count && printed < max_entries; i++) {
        const TraceEntry *e = &s_buf[_idx(count - 1 - i)];
        if (addr_start == 0 && addr_end == 0) {
            /* dump tail */
        } else if (e->addr < addr_start || e->addr >= addr_end) {
            continue;
        }
        const char *side_str = (e->side == TRACE_SIDE_PUAE) ? "PUAE" : "PC  ";
        const char *blit_str = e->is_blitter ? " BLT" : "";
        fprintf(fp, "  [%s%s] $%06X: %0*X -> %0*X  pc=$%06X sz=%d\n",
                side_str, blit_str,
                e->addr,
                e->size * 2, e->old_val,
                e->size * 2, e->new_val,
                e->pc, e->size);
        printed++;
    }
    if (printed == 0)
        fprintf(fp, "  (no trace entries for range $%06X-$%06X)\n", addr_start, addr_end);
}

void trace_dump_side_by_side(FILE *fp, uint32_t addr, int max_entries)
{
    uint32_t count = (uint32_t)trace_count();
    if (count == 0) {
        fprintf(fp, "  (trace buffer empty)\n");
        return;
    }

    /* Collect matching entries */
    TraceEntry puae_ent[64];
    TraceEntry pc_ent[64];
    int puae_n = 0, pc_n = 0;

    for (uint32_t i = 0; i < count && (puae_n < 64 || pc_n < 64); i++) {
        const TraceEntry *e = &s_buf[_idx(i)];
        if (e->addr != addr)
            continue;
        if (e->side == TRACE_SIDE_PUAE && puae_n < 64)
            puae_ent[puae_n++] = *e;
        else if (e->side == TRACE_SIDE_PC && pc_n < 64)
            pc_ent[pc_n++] = *e;
    }

    int limit = (puae_n < pc_n) ? puae_n : pc_n;
    if (limit > max_entries) limit = max_entries;

    fprintf(fp, "\n  Side-by-side write trace for $%06X (first %d matched pairs):\n",
            addr, limit);
    fprintf(fp, "  %-50s | %-50s\n", "PUAE", "PC");
    fprintf(fp, "  %s-+-%s\n",
            "--------------------------------------------------",
            "--------------------------------------------------");

    for (int i = 0; i < limit; i++) {
        const TraceEntry *p = &puae_ent[i];
        const TraceEntry *c = &pc_ent[i];
        char pl[56], cl[56];
        const char *pblit = p->is_blitter ? "BLT " : "";
        const char *cblit = c->is_blitter ? "BLT " : "";
        snprintf(pl, sizeof(pl), "%s%04X->%04X pc=%06X sz=%d",
                 pblit, p->old_val, p->new_val, p->pc, p->size);
        snprintf(cl, sizeof(cl), "%s%04X->%04X pc=%06X sz=%d",
                 cblit, c->old_val, c->new_val, c->pc, c->size);
        fprintf(fp, "  %-50s | %-50s\n", pl, cl);
    }

    if (puae_n > limit)
        fprintf(fp, "  ... (%d more PUAE entries)\n", puae_n - limit);
    if (pc_n > limit)
        fprintf(fp, "  ... (%d more PC entries)\n", pc_n - limit);
    if (limit == 0)
        fprintf(fp, "  (no matching trace entries for $%06X)\n", addr);
}
