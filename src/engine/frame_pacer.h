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
    std::uint64_t on_target = 0;      /* frames within `kOnTarget` of the target */
    std::uint64_t resyncs = 0;        /* deadlines abandoned because the host stalled */
    Nanoseconds display_refresh = 0;  /* 0 when the display is not being matched */
    unsigned refreshes_per_frame = 0; /* how long each frame is held, in refreshes */
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
    [[nodiscard]] unsigned speed_percent() const noexcept {
        return percent_;
    }

    /* How often the display actually puts a picture up, so a frame can be held
     * for a whole number of those instead of a fraction of one.
     *
     * This is the judder the pacer cannot otherwise reach, and it is not a
     * pacing error: a frame period of 13.333 ms on a 120 Hz display is 1.6
     * refreshes, so frames are shown for 2, 2, 1, 2, 2, 1 refreshes however
     * exactly they are produced, and slow steady motion wobbles. The period is
     * rounded to the nearest whole number of refreshes — 2 here, 16.667 ms, 60
     * frames a second — and every frame is then shown for the same length of
     * time. It costs the difference between the speed asked for and the nearest
     * one the display can show evenly, which is why the caller decides whether
     * to ask: `set_display_refresh(0)` means "do not", and is also what an
     * unknown refresh rate, a fast-forward and every headless run pass.
     *
     * `speed_percent()` keeps reporting what was asked for; `target_period()`
     * reports what is being held to. */
    void set_display_refresh(Nanoseconds refresh) noexcept;
    [[nodiscard]] Nanoseconds display_refresh() const noexcept {
        return refresh_;
    }

    /* Whether a match may change the speed of the game to get it.
     *
     * This is what makes matching safe to leave on. On a 144 Hz display the
     * nearest whole-refresh period to PAL is 20.833 ms — four percent slow,
     * nobody can tell, and the judder is gone for nothing. On a 120 Hz display
     * it is 16.667 ms, which is PAL run twenty percent FAST, and a port that
     * did that to a player who picked "normal" would be lying about the speed
     * of the game. So the free match happens by itself and the expensive one is
     * the player's to ask for. */
    enum class DisplayMatch : std::uint8_t {
        WhenFree, /* only when the speed barely changes (kFreeMatchPct) */
        Always,   /* smoothness is worth whatever the speed change costs */
    };
    /* How far a `WhenFree` match may move the frame period, in percent. Four
     * percent is the 144 Hz case and has to be inside it; twenty is the 120 Hz
     * one and has to be outside. */
    static constexpr unsigned kFreeMatchPct = 10;

    void set_display_match(DisplayMatch match) noexcept;
    [[nodiscard]] DisplayMatch display_match() const noexcept {
        return match_;
    }
    /* How many refreshes each frame is held for, or 0 when not matching. */
    [[nodiscard]] unsigned refreshes_per_frame() const noexcept;

    [[nodiscard]] Nanoseconds target_period() const noexcept;

    /* Wait for this frame's deadline and return how long the frame actually
     * took, measured deadline to deadline. The first call starts the clock and
     * returns 0. */
    Nanoseconds wait() noexcept;

    /* Whether the frame now being produced is one to SHOW. At 100% it always
     * is. Fast-forward runs the guest at up to five times PAL, and the display
     * has nothing to gain from that — no monitor shows 250 frames a second, and
     * uploading them is what capped the old turbo — so the picture is sampled
     * at the rate the game is displayed at when it is NOT being fast-forwarded,
     * which is PAL. Every frame is still produced and still paced; only the
     * present is skipped.
     *
     * This is the same kind of deadline as `wait()`'s and for the same reason:
     * what stood here was `SDL_GetTicks() - last < 16`, whole milliseconds
     * against a constant that cannot express 16.667 ms, carrying its own hidden
     * static — the exact defect the pacing rewrite removed from the main loop.
     * The rate is `PAL_FRAME_NS`, derived like every other period here, rather
     * than a number chosen to look like 60 Hz. */
    [[nodiscard]] bool display_frame_due() noexcept;

    /* Whether the frame now being produced is one to feed the audio device.
     * The same question as `display_frame_due()` and the same answer at 100%;
     * at fast-forward the mixer wants one frame of samples per real PAL frame,
     * not five, or the device runs out of buffer and the song plays fast.
     *
     * It is a separate cadence rather than the display's because the two are
     * asked at different points in the loop and either may be skipped alone —
     * sharing one counter would mean whichever asked first consumed the other's
     * turn. */
    [[nodiscard]] bool audio_frame_due() noexcept;

    /* Start again from now: the game was held, or the host was asleep. Without
     * this a hold of any length is a backlog the pacer would try to run off. */
    void resync() noexcept;

    [[nodiscard]] PacingReport report() const noexcept;
    void forget_measurements() noexcept;

  private:
    /* A real-time PAL cadence: how the pacer answers "is one of these due yet"
     * for something that is sampled at the rate the game is displayed at,
     * whatever speed the guest is running. Held as an epoch and a count, like
     * the pacing deadline and for the same reason: a cadence accumulated
     * period by period drifts, and these have to hold for as long as
     * fast-forward is held. */
    struct Cadence {
        Nanoseconds epoch = 0;
        std::uint64_t frames = 0;
        bool started = false;
    };

    [[nodiscard]] Nanoseconds deadline_for(std::uint64_t frame) const noexcept;
    void rebase(Nanoseconds at) noexcept;
    void record(Nanoseconds period) noexcept;
    [[nodiscard]] bool cadence_due(Cadence &cadence) noexcept;

    PacerHost &host_;
    unsigned percent_ = 100;
    Nanoseconds refresh_ = 0; /* the display's, or 0 for "do not match it" */
    DisplayMatch match_ = DisplayMatch::WhenFree;
    Nanoseconds epoch_ = 0;    /* the deadline frame 0 of this run was due at */
    std::uint64_t frames_ = 0; /* frames paced since the epoch */
    bool started_ = false;
    Nanoseconds last_woke_ = 0;

    /* Kept apart from the pacing deadline: the guest's frame rate is the speed
     * knob's to scale, the picture's and the song's are not. */
    Cadence display_{};
    Cadence audio_{};

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

/* The display's refresh interval in nanoseconds, or 0 to pace to the speed
 * alone. See FramePacer::set_display_refresh. */
void pc_pace_set_display_refresh(uint64_t refresh_ns);

/* Non-zero to match the display even when doing so changes the speed of the
 * game; zero to match only when it is free. See FramePacer::DisplayMatch. */
void pc_pace_set_display_match_at_any_speed(int always);

/* Wait out the rest of this frame. Returns the frame's measured length in
 * nanoseconds, or 0 for the first frame of a run. */
uint64_t pc_pace_frame_wait(void);

/* The game was held or the host stalled; do not try to make the time back. */
void pc_pace_resync(void);

/* Non-zero when the frame now being produced is one to show. See
 * FramePacer::display_frame_due — the fast-forward display sample. */
int pc_pace_display_frame_due(void);

/* FramePacer::audio_frame_due — the fast-forward audio sample. */
int pc_pace_audio_frame_due(void);

/* What the pacing delivered, for /state and the frame watchdog. */
typedef struct {
    uint64_t frames;
    uint64_t target_ns;
    uint64_t mean_ns;
    uint64_t shortest_ns;
    uint64_t longest_ns;
    uint64_t on_target;
    uint64_t resyncs;
    uint64_t display_refresh_ns;
    unsigned refreshes_per_frame;
} PcPacingReport;

void pc_pace_report(PcPacingReport *out);
void pc_pace_forget_measurements(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* BENEFACTOR_ENGINE_FRAME_PACER_H */
