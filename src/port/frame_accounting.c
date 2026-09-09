#include "port/frame_accounting.h"

uint64_t g_pc_cycles_flow = 0, g_pc_cycles_flow_max = 0;
uint64_t g_pc_cycles_irq3 = 0, g_pc_cycles_irq3_max = 0;
uint64_t g_pc_cycles_irq6 = 0, g_pc_cycles_irq6_max = 0;
uint64_t g_pc_cycles_frame = 0;
uint64_t g_pc_cycles_present = 0;
uint64_t g_pc_cycles_outside = 0, g_pc_cycles_iter_max = 0;

volatile uint32_t g_pc_guest_owner = 0;
volatile uint32_t g_pc_yield_calls = 0, g_pc_yield_refused = 0, g_pc_yield_parks = 0;

void pc_account(uint64_t *slot, uint64_t *peak, uint64_t cycles) {
    *slot = cycles;
    if (cycles > *peak)
        *peak = cycles;
}
