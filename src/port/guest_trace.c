/* guest_trace.c — readable views of the interpreter's retired-instruction ring.
 *
 * The shared 68000 runtime records every retired instruction in a bounded ring.
 * This owner turns that ring into the two forms the port needs: a log dump when
 * a flow ends somewhere it should not, and a text listing for the debug HTTP
 * server. It holds no state of its own. */
#include "port/guest_trace.h"

#include "common/log.h"
#include "runtime/guest_runtime.h"

#include <stdio.h>

enum {
    TRACE_DEPTH = 64,
    TRACE_PER_LINE = 8,
};

void pc_log_retired_instructions(const char *category) {
    uint32_t program_counters[TRACE_DEPTH];
    uint16_t opcodes[TRACE_DEPTH];
    int count = rt_insn_ring_entries(program_counters, opcodes, TRACE_DEPTH);
    for (int first = 0; first < count; first += TRACE_PER_LINE) {
        char line[256];
        int used = snprintf(line, sizeof line, "retired[%d]:", first);
        for (int index = first; index < count && index < first + TRACE_PER_LINE; index++)
            used += snprintf(line + used, sizeof line - (size_t)used, " %06X/%04X",
                             program_counters[index], opcodes[index]);
        benefactor_log_write(BENEFACTOR_LOG_INFO, category, "%s", line);
    }
}

size_t pc_format_retired_instructions(char *buffer, size_t capacity) {
    uint32_t program_counters[TRACE_DEPTH];
    uint16_t opcodes[TRACE_DEPTH];
    int count = rt_insn_ring_entries(program_counters, opcodes, TRACE_DEPTH);
    size_t used = 0;
    for (int index = 0; index < count && used + 16u < capacity; index++)
        used += (size_t)snprintf(buffer + used, capacity - used, "%06X %04X\n",
                                 program_counters[index], opcodes[index]);
    return used;
}
