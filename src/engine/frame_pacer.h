/* src/engine/frame_pacer.h — how long a frame is allowed to take.
 *
 * A PAL frame is 20 ms and the game is written as though every frame is exactly
 * that. The pacer is the only thing that makes that true on a host whose clock,
 * scheduler and display have nothing to do with a 1994 Amiga.
 *
 * What it replaces, and why the replacement is not a tidy-up: the old pacer read
 * `SDL_GetTicks()` (whole milliseconds) into a microsecond accumulator and slept
 * with `SDL_Delay()` (whole milliseconds, truncated down). A 16.67 ms target
 * cannot be expressed in whole milliseconds at all, so every frame was rounded
 * twice and then overshot by however long the scheduler felt like. Measured over
 * 3,250 frames at 120% speed, with a 16.667 ms target: mean 17.15 ms, and only
 * 36% of frames within a millisecond of it — the rest spread from 11 ms to 25 ms.
 * That spread is the judder.
 *
 * Three things fix it, and all three have to be here rather than at the call
 * site:
 *
 *   - Nanoseconds throughout, from `SDL_GetTicksNS()`, so the clock is not
 *     quantised to two thirds of the thing being measured.
 *   - Deadlines derived from a frame index against an epoch, not accumulated by
 *     repeated addition, so a period that is not a whole number of nanoseconds
 *     (120% of 50 Hz is 16,666,666.67 ns) never drifts.
 *   - `SDL_DelayPrecise()`, which sleeps coarsely and then spins out the last
 *     fraction, instead of a truncating whole-millisecond sleep.
 *
 * The rule is separated from the host clock so it can be tested without waiting:
 * `FramePacer` asks a `PacerHost` for the time and for sleep, and the test gives
 * it a clock it controls. The shipping host is `SdlPacerHost`.
 */
#ifndef BENEFACTOR_ENGINE_FRAME_PACER_H
#define BENEFACTOR_ENGINE_FRAME_PACER_H

#include <stdint.h>

#ifdef __cplusplus

#include <cstdint>

namespace benefactor::engine {

using Nanoseconds = std::uint64_t;

inline constexpr Nanoseconds NS_PER_SECOND = 1000000000ULL;

/* PAL: 312 lines of 454 cycles at 50 Hz. The period is held as a rational so a
 * speed that does not divide evenly stays exact. */
inline constexpr Nanoseconds PAL_FRAME_NS = NS_PER_SECOND / 50;

/* The clock and the sleep the pacer needs, and nothing else. A test supplies a
 * clock it can move by hand; the product supplies SDL's. */
class PacerHost {
  public:
    PacerHost() = default;
    PacerHost(const PacerHost &) = delete;
    PacerHost &operator=(const PacerHost &) = delete;
    PacerHost(PacerHost &&) = delete;
    PacerHost &operator=(PacerHost &&) = delete;
    virtual ~PacerHost() = default;

    /* A monotonic clock in nanoseconds. Never goes backwards. */
    [[nodiscard]] virtual Nanoseconds now() const = 0;

    /* Give up the CPU for about this long, as precisely as the host can. */
    virtual void sleep_for(Nanoseconds duration) = 0;
};

/* What the pacing actually delivered, so the quality of it is a number rather
 * than an opinion. Reported by the control channel, and the reason the
 * measurement above can be repeated after any change to this file. */
struct PacingReport {
    std::uint64_t frames = 0;
    Nanoseconds target = 0; /* what a frame was supposed to take */
    Nanoseconds mean = 0;   /* what it did take */
    Nanoseconds shortest = 0;
    Nanoseconds longest = 0;
    std::uint64_t on_target = 0; /* frames within `kOnTarget` of the target */
    std::uint64_t resyncs = 0;   /* deadlines abandoned because the host stalled */
};

class FramePacer {
  public:
    /* A frame is on target when it lands within this of the target period.
     * Half a millisecond is well inside what a player can see at 50 Hz, and
     * comfortably outside the host scheduler's own noise floor. */
    static constexpr Nanoseconds kOnTarget = 500000;

    /* A host that has stalled longer than this has lost its place: sprinting
     * through the backlog would run the game fast to catch up, so the deadline
     * is abandoned and the next frame starts from now. Five frames is long
     * enough that ordinary scheduling noise never trips it and short enough
     * that a disk load does not leave the game sprinting afterwards. */
    static constexpr Nanoseconds kStallLimit = 5 * PAL_FRAME_NS;

    explicit FramePacer(PacerHost &host) noexcept;

    /* Pace to PAL 50 Hz scaled by `percent` of real time: 100 is PAL, 120 is the
     * turbo knob, 500 is hold-to-fast-forward. Changing it rebases the deadline
     * on the frame that is currently due, so the change does not produce one
     * short or long frame at the seam. */
    void set_speed_percent(unsigned percent) noexcept;
    [[nodiscard]] unsigned speed_percent() const noexcept { return percent_; }
    [[nodiscard]] Nanoseconds target_period() const noexcept;

    /* Wait for this frame's deadline and return how long the frame actually
     * took, measured deadline to deadline. The first call starts the clock and
     * returns 0. */
    Nanoseconds wait() noexcept;

    /* Start again from now: the game was held, or the host was asleep. Without
     * this a hold of any length is a backlog the pacer would try to run off. */
    void resync() noexcept;

    [[nodiscard]] PacingReport report() const noexcept;
    void forget_measurements() noexcept;

  private:
    [[nodiscard]] Nanoseconds deadline_for(std::uint64_t frame) const noexcept;
    void rebase(Nanoseconds at) noexcept;
    void record(Nanoseconds period) noexcept;

    PacerHost &host_;
    unsigned percent_ = 100;
    Nanoseconds epoch_ = 0;    /* the deadline frame 0 of this run was due at */
    std::uint64_t frames_ = 0; /* frames paced since the epoch */
    bool started_ = false;
    Nanoseconds last_woke_ = 0;

    std::uint64_t measured_ = 0;
    Nanoseconds total_ = 0;
    Nanoseconds shortest_ = 0;
    Nanoseconds longest_ = 0;
    std::uint64_t on_target_ = 0;
    std::uint64_t resyncs_ = 0;
};

/* The shipping clock: SDL's monotonic nanosecond tick and its precise sleep. */
class SdlPacerHost final : public PacerHost {
  public:
    [[nodiscard]] Nanoseconds now() const override;
    void sleep_for(Nanoseconds duration) override;
};

} // namespace benefactor::engine

extern "C" {
#endif /* __cplusplus */

/* The C engine's view. One pacer for the process, owned by this module, because
 * there is one game loop and its deadline is global to it. */

void pc_pace_set_speed_percent(unsigned percent);
unsigned pc_pace_speed_percent(void);

/* Wait out the rest of this frame. Returns the frame's measured length in
 * nanoseconds, or 0 for the first frame of a run. */
uint64_t pc_pace_frame_wait(void);

/* The game was held or the host stalled; do not try to make the time back. */
void pc_pace_resync(void);

/* What the pacing delivered, for /state and the frame watchdog. */
typedef struct {
    uint64_t frames;
    uint64_t target_ns;
    uint64_t mean_ns;
    uint64_t shortest_ns;
    uint64_t longest_ns;
    uint64_t on_target;
    uint64_t resyncs;
} PcPacingReport;

void pc_pace_report(PcPacingReport *out);
void pc_pace_forget_measurements(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* BENEFACTOR_ENGINE_FRAME_PACER_H */
