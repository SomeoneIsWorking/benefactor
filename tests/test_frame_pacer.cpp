/* The pacing rule, tested against a clock the test moves by hand.
 *
 * Nothing here sleeps. `FakeHost` is a clock and a sleep the test controls, so
 * the questions the pacer actually has to answer — does a frame land on its
 * deadline, does an early frame get held back, does a late frame get the time
 * given back to it, does a stall get abandoned rather than sprinted off — are
 * asked directly instead of being inferred from a wall-clock run.
 */
#include "engine/frame_pacer.h"

#include <cassert>
#include <vector>

using benefactor::engine::FramePacer;
using benefactor::engine::Nanoseconds;
using benefactor::engine::NS_PER_SECOND;
using benefactor::engine::PacerHost;
using benefactor::engine::PacingReport;

namespace {

constexpr Nanoseconds kPal = NS_PER_SECOND / 50; /* 20 ms */

/* A clock the test owns. `sleep_for` advances it by exactly what was asked, so
 * a test that wants an imperfect sleep says so with `overshoot`. */
class FakeHost final : public PacerHost {
  public:
    [[nodiscard]] Nanoseconds now() const override { return now_; }

    void sleep_for(Nanoseconds duration) override {
        slept.push_back(duration);
        now_ += duration + overshoot;
    }

    void advance(Nanoseconds by) { now_ += by; }

    Nanoseconds overshoot = 0;
    std::vector<Nanoseconds> slept;

  private:
    Nanoseconds now_ = 1000000000ULL; /* not zero, so an epoch bug cannot pass */
};

void the_first_frame_starts_the_clock_and_does_not_sleep() {
    FakeHost host;
    FramePacer pacer(host);
    assert(pacer.wait() == 0);
    assert(host.slept.empty());
}

void a_frame_that_took_no_time_is_held_to_the_full_period() {
    FakeHost host;
    FramePacer pacer(host);
    pacer.wait();
    const Nanoseconds period = pacer.wait();
    assert(host.slept.size() == 1);
    assert(host.slept[0] == kPal);
    assert(period == kPal);
}

void a_frame_that_took_part_of_its_period_sleeps_only_the_rest() {
    FakeHost host;
    FramePacer pacer(host);
    pacer.wait();
    host.advance(12000000ULL); /* the frame's own work took 12 ms */
    const Nanoseconds period = pacer.wait();
    assert(host.slept.size() == 1);
    assert(host.slept[0] == kPal - 12000000ULL);
    assert(period == kPal);
}

void a_frame_that_overran_does_not_sleep_and_the_next_one_gives_the_time_back() {
    FakeHost host;
    FramePacer pacer(host);
    pacer.wait();
    host.advance(26000000ULL); /* 6 ms over its 20 ms */
    const Nanoseconds late = pacer.wait();
    assert(host.slept.empty()); /* already past the deadline: nothing to wait for */
    assert(late == 26000000ULL);

    /* The deadline for the next frame is unchanged, so this one is short by the
     * 6 ms the last one borrowed, and two frames together still take 40 ms.
     * A pacer that restarted from "now" after every late frame would let the
     * game run permanently slow, which is what a stall limit is for and what a
     * six-millisecond overrun is not. */
    const Nanoseconds recovered = pacer.wait();
    assert(host.slept.size() == 1);
    assert(host.slept[0] == 14000000ULL);
    assert(late + recovered == 2 * kPal);
}

void a_host_that_stalled_abandons_the_deadline_instead_of_sprinting() {
    FakeHost host;
    FramePacer pacer(host);
    pacer.wait();
    host.advance(2 * NS_PER_SECOND); /* two seconds gone: suspend, or a load */
    pacer.wait();
    assert(host.slept.empty());

    /* The next frame is a full period, not one of a hundred catch-up frames. */
    const Nanoseconds after = pacer.wait();
    assert(host.slept.size() == 1);
    assert(host.slept[0] == kPal);
    assert(after == kPal);
    assert(pacer.report().resyncs == 1);
}

void a_period_that_is_not_a_whole_nanosecond_does_not_drift() {
    /* 120% of PAL is 16,666,666.666… ns. A pacer that adds a rounded period
     * every frame loses two thirds of a nanosecond each time; one that derives
     * the deadline from the frame index loses nothing. Over a minute at 120%
     * the difference is measurable in whole microseconds, and the test asserts
     * the exact arrival of the last frame rather than a tolerance. */
    FakeHost host;
    FramePacer pacer(host);
    pacer.set_speed_percent(120);
    pacer.wait();
    constexpr std::uint64_t frames = 3600; /* a minute at 60 fps */
    for (std::uint64_t i = 0; i < frames; i++)
        pacer.wait();

    const Nanoseconds expected = 1000000000ULL + frames * NS_PER_SECOND * 100ULL / (50ULL * 120ULL);
    assert(host.now() == expected);
}

void a_speed_change_does_not_produce_a_short_frame_at_the_seam() {
    FakeHost host;
    FramePacer pacer(host);
    pacer.wait();
    pacer.wait();
    const Nanoseconds at_change = host.now();

    pacer.set_speed_percent(500); /* hold-to-fast-forward */
    const Nanoseconds first_fast = pacer.wait();
    assert(first_fast == kPal / 5);
    assert(host.now() == at_change + kPal / 5);
}

void the_speed_is_a_speed_and_zero_is_refused() {
    FakeHost host;
    FramePacer pacer(host);
    pacer.set_speed_percent(0);
    assert(pacer.speed_percent() == 1);
    assert(pacer.target_period() == NS_PER_SECOND * 100ULL / 50ULL);
}

void the_report_counts_frames_on_target_and_names_the_worst() {
    FakeHost host;
    FramePacer pacer(host);
    pacer.wait();
    for (int i = 0; i < 10; i++)
        pacer.wait();

    /* A sleep that overshoots by a millisecond is outside the half-millisecond
     * on-target band, so the count must stop rising. */
    host.overshoot = 1000000ULL;
    pacer.wait();

    const PacingReport report = pacer.report();
    assert(report.frames == 11);
    assert(report.target == kPal);
    assert(report.on_target == 10);
    assert(report.longest >= kPal + 1000000ULL - 1);
    assert(report.shortest <= kPal);
}

void the_measurement_can_be_started_again_without_disturbing_the_pacing() {
    FakeHost host;
    FramePacer pacer(host);
    pacer.wait();
    pacer.wait();
    pacer.forget_measurements();
    const Nanoseconds after = pacer.wait();
    assert(after == kPal); /* the deadline survived */
    assert(pacer.report().frames == 1);
}

} // namespace

int main() {
    the_first_frame_starts_the_clock_and_does_not_sleep();
    a_frame_that_took_no_time_is_held_to_the_full_period();
    a_frame_that_took_part_of_its_period_sleeps_only_the_rest();
    a_frame_that_overran_does_not_sleep_and_the_next_one_gives_the_time_back();
    a_host_that_stalled_abandons_the_deadline_instead_of_sprinting();
    a_period_that_is_not_a_whole_nanosecond_does_not_drift();
    a_speed_change_does_not_produce_a_short_frame_at_the_seam();
    the_speed_is_a_speed_and_zero_is_refused();
    the_report_counts_frames_on_target_and_names_the_worst();
    the_measurement_can_be_started_again_without_disturbing_the_pacing();
    return 0;
}
