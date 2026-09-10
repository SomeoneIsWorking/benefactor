/* The one place a frame's numbers live. See frame_accounting.h for why they
 * are kept at all, and why every field is a plain integer. */
#include "port/frame_accounting.h"

#include "common/log.h"
#include "engine/hw.h"
#include "port/guest_profile.h"

namespace benefactor::diag {

FrameAccounting &FrameAccounting::instance() {
    /* Constructed on first use and never destroyed: the frame watchdog's signal
     * handler reads these, and it can fire during shutdown. */
    static FrameAccounting the_accounting;
    return the_accounting;
}

OwnerAccount &FrameAccounting::owner(PcOwner which) noexcept {
    return which == PC_OWNER_LEVEL3_VBLANK ? level3_ : level6_;
}

const OwnerAccount &FrameAccounting::owner(PcOwner which) const noexcept {
    return which == PC_OWNER_LEVEL3_VBLANK ? level3_ : level6_;
}

} // namespace benefactor::diag

using benefactor::diag::FrameAccounting;

extern "C" {

void pc_account_owner(PcOwner owner, uint64_t cycles) {
    FrameAccounting::instance().owner(owner).delivered(cycles);
}

void pc_account_flow(uint64_t cycles) { FrameAccounting::instance().flow().record(cycles); }

void pc_account_iteration(uint64_t cycles) {
    FrameAccounting::instance().iteration().record(cycles);
}

void pc_set_frame_cycles(uint64_t cycles) { FrameAccounting::instance().set_frame_cycles(cycles); }

void pc_set_present_cycles(uint64_t cycles) {
    FrameAccounting::instance().set_present_cycles(cycles);
}

PcOwner pc_running_owner(void) { return FrameAccounting::instance().running(); }

void pc_set_running_owner(PcOwner owner) { FrameAccounting::instance().set_running(owner); }

void pc_note_wait_reached(void) { FrameAccounting::instance().waits().reached(); }
void pc_note_wait_refused(void) { FrameAccounting::instance().waits().refused(); }
void pc_note_wait_parked(void) { FrameAccounting::instance().waits().parked(); }
void pc_note_title_draw(void) { FrameAccounting::instance().note_title_draw(); }

void pc_frame_accounting(PcFrameAccounting *out) {
    if (out == nullptr) {
        return;
    }
    const FrameAccounting &a = FrameAccounting::instance();
    const auto &level3 = a.owner(PC_OWNER_LEVEL3_VBLANK);
    const auto &level6 = a.owner(PC_OWNER_LEVEL6_TIMER);
    *out = PcFrameAccounting{
        a.flow().last(),           a.flow().peak(),           level3.cycles().last(),
        level3.cycles().peak(),    level6.cycles().last(),    level6.cycles().peak(),
        a.frame_cycles(),          a.present_cycles(),        a.iteration().last(),
        a.iteration().peak(),      level3.deliveries(),       level6.deliveries(),
        a.waits().reached_count(), a.waits().refused_count(), a.waits().parked_count(),
        a.title_draws(),           (uint32_t)a.running(),
    };
}

void pc_note_frame_phase(void) {
    static std::uint32_t last = 0xFFFFFFFFu;
    const std::uint32_t cop1lc = hw_get_cop1lc() & 0xFFFFFFu;
    if (cop1lc == last) {
        return;
    }
    last = cop1lc;
    benefactor_log_write(BENEFACTOR_LOG_INFO, "phase", "frame=%d cop1lc=%06X", hw_get_frame_num(),
                         cop1lc);
    /* One screen's hot loop must not be read as the next screen's. Report the
     * screen that is ending before clearing it. */
    char hot[512];
    if (pc_profile_report(hot, (int)sizeof hot) > 0) {
        benefactor_log_write(BENEFACTOR_LOG_INFO, "hot", "%s", hot);
    }
    pc_profile_reset();
}

} /* extern "C" */
