#include "engine/frame_pacer.h"

#include <algorithm>

namespace benefactor::engine {

namespace {

/* The deadline of frame `n` at `percent` of PAL, as an exact rational rather
 * than `n` additions of a rounded period. 120% of 50 Hz is 16,666,666.666… ns;
 * adding a rounded 16,666,666 every frame loses two thirds of a nanosecond a
 * frame, which is nothing, but the same expression at 3% or 7% is not nothing,
 * and there is no reason to accept any of it when the exact form is this short.
 *
 * `n * NS_PER_SECOND * 100` overflows a 64-bit unsigned at about six million
 * frames — 33 hours at PAL — so the frame index is rebased well before then. */
constexpr Nanoseconds offset_of(std::uint64_t frame, unsigned percent) noexcept {
    return frame * NS_PER_SECOND * 100ULL / (50ULL * percent);
}

/* Rebasing keeps the frame index small enough that the multiply above cannot
 * overflow, and it is where a speed change lands. An hour of frames is far
 * inside the safe range and far outside anything a player would notice. */
constexpr std::uint64_t kRebaseEvery = 50ULL * 60ULL * 60ULL;

} // namespace

FramePacer::FramePacer(PacerHost &host) noexcept : host_(host) {
}

Nanoseconds FramePacer::target_period() const noexcept {
    return offset_of(1, percent_);
}

Nanoseconds FramePacer::deadline_for(std::uint64_t frame) const noexcept {
    return epoch_ + offset_of(frame, percent_);
}

void FramePacer::rebase(Nanoseconds at) noexcept {
    epoch_ = at;
    frames_ = 0;
}

void FramePacer::set_speed_percent(unsigned percent) noexcept {
    /* 0% is not a speed, and the caller asking for it means a configuration
     * value got here unvalidated. Refusing it here keeps the divide safe. */
    const unsigned wanted = std::max(1U, percent);
    if (wanted == percent_) {
        return;
    }
    /* Rebase onto the deadline that is currently due, so the frame spanning the
     * change is one frame of the old speed and not a short or long one. */
    if (started_) {
        rebase(deadline_for(frames_));
    }
    percent_ = wanted;
}

void FramePacer::resync() noexcept {
    started_ = false;
    resyncs_++;
}

void FramePacer::record(Nanoseconds period) noexcept {
    measured_++;
    total_ += period;
    shortest_ = (measured_ == 1) ? period : std::min(shortest_, period);
    longest_ = std::max(longest_, period);
    const Nanoseconds target = target_period();
    const Nanoseconds off = (period > target) ? period - target : target - period;
    if (off <= kOnTarget) {
        on_target_++;
    }
}

Nanoseconds FramePacer::wait() noexcept {
    const Nanoseconds now = host_.now();

    /* First frame of a run, or the first after a resync: there is no deadline to
     * hold yet, so this one starts the clock rather than being paced. */
    if (!started_) {
        started_ = true;
        rebase(now);
        last_woke_ = now;
        return 0;
    }

    frames_++;
    const Nanoseconds deadline = deadline_for(frames_);

    /* Behind by more than a stall's worth: the host was descheduled, or a load
     * took the thread away. Running the backlog off would show the player the
     * game sprinting, so the deadline is given up and the next frame is timed
     * from here. Frames genuinely too slow to make the target do NOT come
     * through here — they are late by less than the limit, and staying on the
     * old deadline is what lets the pacer give the time back. */
    if (now > deadline + kStallLimit) {
        resyncs_++;
        rebase(now);
        const Nanoseconds period = now - last_woke_;
        last_woke_ = now;
        record(period);
        return period;
    }

    if (now < deadline) {
        host_.sleep_for(deadline - now);
    }

    const Nanoseconds woke = host_.now();
    const Nanoseconds period = woke - last_woke_;
    last_woke_ = woke;
    record(period);

    if (frames_ >= kRebaseEvery) {
        rebase(deadline);
    }

    return period;
}

PacingReport FramePacer::report() const noexcept {
    PacingReport out;
    out.frames = measured_;
    out.target = target_period();
    out.mean = measured_ ? total_ / measured_ : 0;
    out.shortest = shortest_;
    out.longest = longest_;
    out.on_target = on_target_;
    out.resyncs = resyncs_;
    return out;
}

void FramePacer::forget_measurements() noexcept {
    measured_ = 0;
    total_ = 0;
    shortest_ = 0;
    longest_ = 0;
    on_target_ = 0;
    resyncs_ = 0;
}

} // namespace benefactor::engine
