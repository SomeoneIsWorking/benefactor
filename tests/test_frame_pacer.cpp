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
    [[nodiscard]] Nanoseconds now() const override {
        return now_;
    }

    void sleep_for(Nanoseconds duration) override {
        slept.push_back(duration);
        now_ += duration + overshoot;
    }

    void advance(Nanoseconds by) {
        now_ += by;
    }

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
    for (std::uint64_t i = 0; i < frames; i++) {
        pacer.wait();
    }

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
    for (int i = 0; i < 10; i++) {
        pacer.wait();
    }

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

void every_frame_is_shown_at_ordinary_speed() {
    /* Nothing is skipped when the guest is running at PAL: a frame takes a
     * frame, so the display deadline is always the one that just came due. */
    FakeHost host;
    FramePacer pacer(host);
    pacer.wait(); /* starts the clock */
    for (int i = 0; i < 10; i++) {
        assert(pacer.display_frame_due());
        pacer.wait();
    }
}

void fast_forward_shows_one_frame_per_pal_frame_and_skips_the_rest() {
    /* Hold-to-fast-forward is 500%, so the guest produces five frames in the
     * time the display gets one. The old rule compared whole milliseconds
     * against a constant 16, which is neither PAL nor the host's refresh; this
     * asserts the exact cadence instead. */
    FakeHost host;
    FramePacer pacer(host);
    pacer.set_speed_percent(500);
    pacer.wait(); /* starts the clock */

    int shown = 0;
    constexpr int frames = 50; /* ten PAL frames' worth of guest time */
    for (int i = 0; i < frames; i++) {
        if (pacer.display_frame_due()) {
            shown++;
        }
        pacer.wait();
    }
    assert(shown == frames / 5);
}

void the_display_cadence_does_not_drift_over_a_long_fast_forward() {
    /* The whole reason the deadline is derived from an index: a cadence that
     * adds a period per frame accumulates its rounding, and fast-forward is
     * held for minutes at a time. A second of guest time at 500% is 250 frames
     * and must still be exactly 50 shown ones, not 49 or 51. */
    FakeHost host;
    FramePacer pacer(host);
    pacer.set_speed_percent(500);
    pacer.wait();

    int shown = 0;
    for (int i = 0; i < 250 * 60; i++) {
        if (pacer.display_frame_due()) {
            shown++;
        }
        pacer.wait();
    }
    assert(shown == 50 * 60);
}

void a_host_that_stalled_does_not_show_the_frames_it_missed_as_a_burst() {
    FakeHost host;
    FramePacer pacer(host);
    assert(pacer.display_frame_due()); /* the first frame of the run */

    host.advance(NS_PER_SECOND); /* a second gone: a disk load, or a suspend */
    assert(pacer.display_frame_due());

    /* The fifty frames it missed cannot be shown after the fact, so the cadence
     * starts again from the frame just shown: the next one is a whole PAL frame
     * away, not a backlog presented as fast as the loop can go. */
    host.advance(kPal - 1);
    assert(!pacer.display_frame_due());
    host.advance(1);
    assert(pacer.display_frame_due());
}

void the_audio_and_display_samples_do_not_consume_each_other() {
    /* Both run at PAL and both are asked once per produced frame, but either
     * may be skipped on its own. If they shared a counter, whichever was asked
     * first would take the other's turn and the song would play at half rate
     * through a fast-forward. */
    FakeHost host;
    FramePacer pacer(host);
    assert(pacer.display_frame_due());
    assert(pacer.audio_frame_due());

    host.advance(kPal / 5); /* four more produced frames at 500% */
    assert(!pacer.display_frame_due());
    assert(!pacer.audio_frame_due());

    host.advance(kPal - kPal / 5);
    assert(pacer.display_frame_due());
    assert(pacer.audio_frame_due());

    /* And a frame the display skipped is still a frame the audio can take. */
    host.advance(kPal);
    assert(pacer.audio_frame_due());
    assert(pacer.display_frame_due());
}

void the_audio_cadence_does_not_drift_over_a_long_fast_forward() {
    FakeHost host;
    FramePacer pacer(host);
    int fed = 0;
    /* Five minutes of guest frames at 500%: the mixer should have been handed
     * one frame of samples per real PAL frame, no more. */
    for (std::uint64_t frame = 0; frame < 5ULL * 60ULL * 50ULL * 5ULL; ++frame) {
        if (pacer.audio_frame_due()) {
            fed++;
        }
        host.advance(kPal / 5);
    }
    assert(fed == 5 * 60 * 50);
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

void a_matched_display_holds_every_frame_for_the_same_whole_number_of_refreshes() {
    /* The judder the pacer cannot otherwise reach. 150% of PAL is 13.333 ms and
     * a 120 Hz refresh is 8.333 ms, so a frame is shown for 1.6 refreshes —
     * 2, 2, 1 over and over, whatever the pacing does. Matching rounds the
     * period to two whole refreshes, and every frame is then held the same. */
    constexpr Nanoseconds refresh120 = NS_PER_SECOND / 120;
    FakeHost host;
    FramePacer pacer(host);
    pacer.set_speed_percent(150);
    pacer.set_display_match(FramePacer::DisplayMatch::Always);
    assert(pacer.target_period() == NS_PER_SECOND * 100ULL / (50ULL * 150ULL));

    pacer.set_display_refresh(refresh120);
    assert(pacer.refreshes_per_frame() == 2);
    assert(pacer.target_period() == 2 * refresh120);
    /* And the speed asked for is still reported as asked for: the setting the
     * player chose has not silently become a different one. */
    assert(pacer.speed_percent() == 150);
}

void a_matched_frame_is_paced_to_the_matched_period_not_the_speed() {
    constexpr Nanoseconds refresh120 = NS_PER_SECOND / 120;
    FakeHost host;
    FramePacer pacer(host);
    pacer.set_speed_percent(150);
    pacer.set_display_match(FramePacer::DisplayMatch::Always);
    pacer.set_display_refresh(refresh120);
    pacer.wait();
    const Nanoseconds period = pacer.wait();
    assert(host.slept.size() == 1);
    assert(host.slept[0] == 2 * refresh120);
    assert(period == 2 * refresh120);
}

void a_matched_cadence_does_not_drift_over_a_long_run() {
    /* A refresh interval is a whole number of nanoseconds, so a minute of
     * frames must arrive on an exact multiple of it. The point of matching is
     * that a frame lands on a refresh; a cadence that slid by a microsecond a
     * minute would slide off one every few minutes and stutter. */
    constexpr Nanoseconds refresh120 = NS_PER_SECOND / 120;
    FakeHost host;
    FramePacer pacer(host);
    pacer.set_speed_percent(150);
    pacer.set_display_match(FramePacer::DisplayMatch::Always);
    pacer.set_display_refresh(refresh120);
    pacer.wait();
    constexpr std::uint64_t frames = 3600;
    for (std::uint64_t i = 0; i < frames; i++) {
        pacer.wait();
    }
    assert(host.now() == 1000000000ULL + frames * 2 * refresh120);
}

void a_display_that_already_divides_the_speed_changes_nothing() {
    /* 120% of PAL is 60 frames a second and a 120 Hz display shows each of them
     * for exactly two refreshes already. Matching must be a no-op there, not a
     * rounding that nudges the period by a nanosecond. */
    constexpr Nanoseconds refresh120 = NS_PER_SECOND / 120;
    FakeHost host;
    FramePacer pacer(host);
    pacer.set_speed_percent(120);
    const Nanoseconds before = pacer.target_period();
    pacer.set_display_refresh(refresh120);
    assert(pacer.refreshes_per_frame() == 2);
    assert(pacer.target_period() == before);
}

void a_speed_faster_than_the_refresh_is_still_held_for_one_whole_refresh() {
    /* 500% of PAL is 4 ms, less than half a 120 Hz refresh. There is no
     * fraction of a refresh to hold a frame for, so the floor is one. In the
     * product a fast-forward passes no refresh at all and is never matched;
     * this only says the arithmetic cannot divide by zero or ask for none. */
    constexpr Nanoseconds refresh120 = NS_PER_SECOND / 120;
    FakeHost host;
    FramePacer pacer(host);
    pacer.set_speed_percent(500);
    pacer.set_display_match(FramePacer::DisplayMatch::Always);
    pacer.set_display_refresh(refresh120);
    assert(pacer.refreshes_per_frame() == 1);
    assert(pacer.target_period() == refresh120);
}

void letting_go_of_the_display_returns_to_the_exact_speed() {
    constexpr Nanoseconds refresh120 = NS_PER_SECOND / 120;
    FakeHost host;
    FramePacer pacer(host);
    pacer.set_speed_percent(150);
    const Nanoseconds unmatched = pacer.target_period();
    pacer.set_display_match(FramePacer::DisplayMatch::Always);
    pacer.set_display_refresh(refresh120);
    pacer.set_display_refresh(0);
    assert(pacer.refreshes_per_frame() == 0);
    assert(pacer.target_period() == unmatched);
    assert(pacer.report().display_refresh == 0);
}

void matching_the_display_does_not_produce_a_short_frame_at_the_seam() {
    /* Same seam as a speed change, and the same rule: the frame spanning the
     * change is a whole frame of the OLD period. A display change arrives when
     * the window is dragged to another monitor, mid-game and mid-frame. */
    constexpr Nanoseconds refresh120 = NS_PER_SECOND / 120;
    FakeHost host;
    FramePacer pacer(host);
    pacer.set_speed_percent(150);
    pacer.wait();
    pacer.wait();
    const Nanoseconds at_change = host.now();
    pacer.set_display_match(FramePacer::DisplayMatch::Always);
    pacer.set_display_refresh(refresh120);
    const Nanoseconds first = pacer.wait();
    assert(first == 2 * refresh120);
    assert(host.now() == at_change + 2 * refresh120);
}

void the_report_says_what_the_display_is_doing() {
    constexpr Nanoseconds refresh120 = NS_PER_SECOND / 120;
    FakeHost host;
    FramePacer pacer(host);
    pacer.set_speed_percent(150);
    pacer.set_display_match(FramePacer::DisplayMatch::Always);
    pacer.set_display_refresh(refresh120);
    const PacingReport report = pacer.report();
    assert(report.display_refresh == refresh120);
    assert(report.refreshes_per_frame == 2);
    assert(report.target == 2 * refresh120);
}

void a_match_that_would_cost_real_speed_is_refused_by_default() {
    /* PAL on a 120 Hz display: the nearest whole-refresh period is 16.667 ms,
     * which is the game running twenty percent fast. Nobody asked for that, so
     * the default limit leaves the speed exactly where it was and the player
     * keeps the judder they can at least recognise as the display's. */
    constexpr Nanoseconds refresh120 = NS_PER_SECOND / 120;
    FakeHost host;
    FramePacer pacer(host);
    pacer.set_display_refresh(refresh120);
    assert(pacer.refreshes_per_frame() == 0);
    assert(pacer.target_period() == kPal);
}

void a_match_that_costs_almost_nothing_is_taken_by_default() {
    /* 144 Hz: three refreshes is 20.833 ms, four percent slow. That is inside
     * the default limit, so it happens without anyone choosing it, and the
     * judder of 2.88 refreshes a frame goes away for free. */
    constexpr Nanoseconds refresh144 = NS_PER_SECOND / 144;
    FakeHost host;
    FramePacer pacer(host);
    pacer.set_display_refresh(refresh144);
    assert(pacer.refreshes_per_frame() == 3);
    assert(pacer.target_period() == 3 * refresh144);
}

void asking_for_it_takes_the_match_the_default_refused() {
    constexpr Nanoseconds refresh120 = NS_PER_SECOND / 120;
    FakeHost host;
    FramePacer pacer(host);
    pacer.set_speed_percent(150);
    pacer.set_display_refresh(refresh120);
    assert(pacer.refreshes_per_frame() == 0); /* 150% -> 120% costs 25% of the period */
    pacer.set_display_match(FramePacer::DisplayMatch::Always);
    assert(pacer.refreshes_per_frame() == 2);
    assert(pacer.target_period() == 2 * refresh120);
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
    every_frame_is_shown_at_ordinary_speed();
    fast_forward_shows_one_frame_per_pal_frame_and_skips_the_rest();
    the_display_cadence_does_not_drift_over_a_long_fast_forward();
    a_host_that_stalled_does_not_show_the_frames_it_missed_as_a_burst();
    the_audio_and_display_samples_do_not_consume_each_other();
    the_audio_cadence_does_not_drift_over_a_long_fast_forward();
    the_measurement_can_be_started_again_without_disturbing_the_pacing();
    a_matched_display_holds_every_frame_for_the_same_whole_number_of_refreshes();
    a_matched_frame_is_paced_to_the_matched_period_not_the_speed();
    a_matched_cadence_does_not_drift_over_a_long_run();
    a_display_that_already_divides_the_speed_changes_nothing();
    a_speed_faster_than_the_refresh_is_still_held_for_one_whole_refresh();
    letting_go_of_the_display_returns_to_the_exact_speed();
    matching_the_display_does_not_produce_a_short_frame_at_the_seam();
    the_report_says_what_the_display_is_doing();
    a_match_that_would_cost_real_speed_is_refused_by_default();
    a_match_that_costs_almost_nothing_is_taken_by_default();
    asking_for_it_takes_the_match_the_default_refused();
    return 0;
}
