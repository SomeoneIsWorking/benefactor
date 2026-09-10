#include "port/frame_accounting.h"

#include "common/log.h"
#include "engine/hw.h"
#include "port/guest_profile.h"

uint64_t g_pc_cycles_flow = 0, g_pc_cycles_flow_max = 0;
uint64_t g_pc_cycles_irq3 = 0, g_pc_cycles_irq3_max = 0;
uint64_t g_pc_cycles_irq6 = 0, g_pc_cycles_irq6_max = 0;
uint64_t g_pc_cycles_frame = 0;
uint64_t g_pc_cycles_present = 0;
uint64_t g_pc_cycles_outside = 0, g_pc_cycles_iter_max = 0;

volatile uint32_t g_pc_guest_owner = 0;
volatile uint32_t g_pc_title_draws = 0;
volatile uint32_t g_pc_irq3_calls = 0, g_pc_irq6_calls = 0;
volatile uint32_t g_pc_yield_calls = 0, g_pc_yield_refused = 0, g_pc_yield_parks = 0;

void pc_account(uint64_t *slot, uint64_t *peak, uint64_t cycles) {
    *slot = cycles;
    if (cycles > *peak)
        *peak = cycles;
}

void pc_note_frame_phase(void) {
    static uint32_t last = 0xFFFFFFFFu;
    const uint32_t cop1lc = hw_get_cop1lc() & 0xFFFFFFu;
    if (cop1lc == last)
        return;
    last = cop1lc;
    benefactor_log_write(BENEFACTOR_LOG_INFO, "phase", "frame=%d cop1lc=%06X", hw_get_frame_num(),
                         cop1lc);
    /* One screen's hot loop must not be read as the next screen's. Report the
     * screen that is ending before clearing it. */
    {
        char hot[512];
        if (pc_profile_report(hot, (int)sizeof hot) > 0)
            benefactor_log_write(BENEFACTOR_LOG_INFO, "hot", "%s", hot);
        pc_profile_reset();
    }
}
