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

/* One game loop, one deadline. Function-local so the construction order is
 * defined and nothing can reach them before this file has initialised. */
benefactor::engine::FramePacer &pacer() {
    static benefactor::engine::SdlPacerHost host;
    static benefactor::engine::FramePacer instance(host);
    return instance;
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
