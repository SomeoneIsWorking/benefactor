/* Binding the pacing rule to SDL's clock, and the one pacer the game loop uses.
 *
 * Separate from `frame_pacer.cpp` so the rule itself compiles and is tested
 * without SDL: a deadline is arithmetic, and arithmetic should not need a media
 * library to be checked.
 */
#include "engine/frame_pacer.h"

#include <SDL3/SDL_timer.h>

namespace benefactor::engine {

Nanoseconds SdlPacerHost::now() const {
    return SDL_GetTicksNS();
}

/* SDL_DelayPrecise sleeps coarsely and spins out the remainder, which is the
 * whole reason a frame lands on its deadline instead of a millisecond past it.
 * It is the one call here that costs anything, and it costs a short spin per
 * frame in exchange for the judder going away. */
void SdlPacerHost::sleep_for(Nanoseconds duration) {
    SDL_DelayPrecise(duration);
}

} // namespace benefactor::engine

namespace {

/* One game loop, one deadline, and the clock it runs on held with it: as one
 * object the host cannot outlive the pacer that borrows it, which two separate
 * statics would leave to declaration order. Function-local so it is built on
 * first use and nothing can reach it before this file has initialised. */
struct GamePacer {
    benefactor::engine::SdlPacerHost host;
    benefactor::engine::FramePacer pacer{host};
};

benefactor::engine::FramePacer &pacer() {
    static GamePacer only;
    return only.pacer;
}

} // namespace

extern "C" {

void pc_pace_set_speed_percent(unsigned percent) {
    pacer().set_speed_percent(percent);
}

unsigned pc_pace_speed_percent(void) {
    return pacer().speed_percent();
}

uint64_t pc_pace_frame_wait(void) {
    return pacer().wait();
}

void pc_pace_resync(void) {
    pacer().resync();
}

int pc_pace_display_frame_due(void) {
    return pacer().display_frame_due() ? 1 : 0;
}

void pc_pace_report(PcPacingReport *out) {
    if (out == nullptr) {
        return;
    }
    const benefactor::engine::PacingReport report = pacer().report();
    out->frames = report.frames;
    out->target_ns = report.target;
    out->mean_ns = report.mean;
    out->shortest_ns = report.shortest;
    out->longest_ns = report.longest;
    out->on_target = report.on_target;
    out->resyncs = report.resyncs;
}

void pc_pace_forget_measurements(void) {
    pacer().forget_measurements();
}

} // extern "C"
