/* src/port/frame_accounting.h — where a frame's guest time and frames go.
 *
 * A screen that runs at the wrong speed is one of a few very different faults:
 * the game flow doing too much, an interrupt handler doing too much, or frames
 * simply not being presented. Guessing between them costs hours, so the frame
 * loop records the split and /state (plus the frame watchdog) reports it.
 *
 * Every peak is kept alongside the last value, because the per-frame values are
 * only readable between iterations: a single iteration that runs away leaves the
 * sampler looking at whatever the previous, short iteration wrote.
 *
 * The interrupt owners used to be six loose globals selected by three parallel
 * ternaries at the one call site — `owner == 3 ? &g_pc_cycles_irq3 : ...` for
 * the total, again for the peak, again for the delivery count. An owner is one
 * thing with three numbers, so it is one object here, and the caller names the
 * owner instead of picking its storage.
 *
 * Read from the frame watchdog's signal handler and from the control channel's
 * own thread, so every counter is a lock-free atomic read/written with relaxed
 * ordering: no locks, no allocation, and no torn value. Relaxed is right
 * because nothing here orders anything else — each number is only ever read to
 * be reported. (These were `volatile` integers; C++20 deprecates incrementing
 * those, and volatile never actually promised an untorn read.)
 */
#ifndef BENEFACTOR_PORT_FRAME_ACCOUNTING_H
#define BENEFACTOR_PORT_FRAME_ACCOUNTING_H

#include <stdint.h>

/* Who is running guest code. Guest code entered through an interrupt vector
 * runs on the MAIN thread, where the per-frame wait cannot park anything. */
typedef enum {
    PC_OWNER_FLOW = 0,          /* the parkable game flow */
    PC_OWNER_LEVEL3_VBLANK = 3, /* level-3 vector: the frame's copper list */
    PC_OWNER_LEVEL6_TIMER = 6,  /* level-6 vector: the music player chain */
} PcOwner;

#ifdef __cplusplus

#include <atomic>
#include <cstdint>

namespace benefactor::diag {

/* A number written by whoever is running guest code and read from anywhere,
 * including a signal handler. Lock-free on every platform this builds for. */
using Counter = std::atomic<std::uint32_t>;
static_assert(Counter::is_always_lock_free, "a signal handler reads these");

inline void bump(Counter &counter) noexcept { counter.fetch_add(1, std::memory_order_relaxed); }

inline std::uint32_t load(const Counter &counter) noexcept {
    return counter.load(std::memory_order_relaxed);
}

/* One measured span: the last value and the largest ever seen. */
class Span final {
  public:
    void record(std::uint64_t cycles) noexcept {
        last_ = cycles;
        if (cycles > peak_) {
            peak_ = cycles;
        }
    }
    [[nodiscard]] std::uint64_t last() const noexcept { return last_; }
    [[nodiscard]] std::uint64_t peak() const noexcept { return peak_; }

  private:
    std::uint64_t last_{};
    std::uint64_t peak_{};
};

/* An interrupt owner is one thing with three numbers: how long its last
 * delivery ran, the longest that ever ran, and how many there were. The cycle
 * totals say how long a delivery took; the count says how often it happened,
 * which is what a screen driven from inside an interrupt (the intro crawl)
 * actually advances on. */
class OwnerAccount final {
  public:
    void delivered(std::uint64_t cycles) noexcept {
        bump(deliveries_);
        cycles_.record(cycles);
    }
    [[nodiscard]] const Span &cycles() const noexcept { return cycles_; }
    [[nodiscard]] std::uint32_t deliveries() const noexcept { return load(deliveries_); }

  private:
    Span cycles_{};
    Counter deliveries_{};
};

/* How many per-frame waits were reached, refused (not the game flow, so
 * unparkable) and actually parked. */
class WaitAccount final {
  public:
    void reached() noexcept { bump(reached_); }
    void refused() noexcept { bump(refused_); }
    void parked() noexcept { bump(parked_); }
    [[nodiscard]] std::uint32_t reached_count() const noexcept { return load(reached_); }
    [[nodiscard]] std::uint32_t refused_count() const noexcept { return load(refused_); }
    [[nodiscard]] std::uint32_t parked_count() const noexcept { return load(parked_); }

  private:
    Counter reached_{};
    Counter refused_{};
    Counter parked_{};
};

class FrameAccounting final {
  public:
    static FrameAccounting &instance();

    FrameAccounting(const FrameAccounting &) = delete;
    FrameAccounting &operator=(const FrameAccounting &) = delete;
    FrameAccounting(FrameAccounting &&) = delete;
    FrameAccounting &operator=(FrameAccounting &&) = delete;

    /* One PAL frame is 141,648 cycles — an owner far above that is the fault. */
    [[nodiscard]] OwnerAccount &owner(PcOwner which) noexcept;
    [[nodiscard]] const OwnerAccount &owner(PcOwner which) const noexcept;

    [[nodiscard]] Span &flow() noexcept { return flow_; }
    [[nodiscard]] const Span &flow() const noexcept { return flow_; }
    [[nodiscard]] Span &iteration() noexcept { return iteration_; }
    [[nodiscard]] const Span &iteration() const noexcept { return iteration_; }
    [[nodiscard]] WaitAccount &waits() noexcept { return waits_; }
    [[nodiscard]] const WaitAccount &waits() const noexcept { return waits_; }

    [[nodiscard]] std::uint64_t frame_cycles() const noexcept { return frame_cycles_; }
    void set_frame_cycles(std::uint64_t cycles) noexcept { frame_cycles_ = cycles; }
    [[nodiscard]] std::uint64_t present_cycles() const noexcept { return present_cycles_; }
    void set_present_cycles(std::uint64_t cycles) noexcept { present_cycles_ = cycles; }

    /* Which owner is running guest code right now. */
    [[nodiscard]] PcOwner running() const noexcept { return static_cast<PcOwner>(load(running_)); }
    void set_running(PcOwner which) noexcept {
        running_.store(static_cast<std::uint32_t>(which), std::memory_order_relaxed);
    }

    /* How many times the title/intro screen redrew itself ($0041A4). Compare
     * it to the presented frame count: more than one draw per frame means the
     * screen is being stepped faster than it is shown, which is what "the
     * crawl is too fast" looks like from here. */
    [[nodiscard]] std::uint32_t title_draws() const noexcept { return load(title_draws_); }
    void note_title_draw() noexcept { bump(title_draws_); }

  private:
    FrameAccounting() = default;

    Span flow_{};
    Span iteration_{};
    OwnerAccount level3_{};
    OwnerAccount level6_{};
    WaitAccount waits_{};
    std::uint64_t frame_cycles_{};
    std::uint64_t present_cycles_{};
    Counter running_{PC_OWNER_FLOW};
    Counter title_draws_{};
};

} // namespace benefactor::diag

extern "C" {
#endif /* __cplusplus */

/* The C seam. The frame loop, the hardware layer and the watchdog are C; each
 * of these forwards to exactly one thing on FrameAccounting, so there is one
 * place a number is kept. */

/* Record a delivery of `owner` that ran for `cycles`. Replaces the old
 * pc_account(&total, &peak, cycles) — the caller no longer picks storage. */
void pc_account_owner(PcOwner owner, uint64_t cycles);
void pc_account_flow(uint64_t cycles);
void pc_account_iteration(uint64_t cycles);
void pc_set_frame_cycles(uint64_t cycles);
void pc_set_present_cycles(uint64_t cycles);

/* Which owner is running guest code (see PcOwner). */
PcOwner pc_running_owner(void);
void pc_set_running_owner(PcOwner owner);

void pc_note_wait_reached(void);
void pc_note_wait_refused(void);
void pc_note_wait_parked(void);
void pc_note_title_draw(void);

/* Everything the watchdog and /state report, in one read. Signal-safe. */
typedef struct {
    uint64_t flow, flow_peak;
    uint64_t irq3, irq3_peak;
    uint64_t irq6, irq6_peak;
    uint64_t frame, present;
    uint64_t iteration, iteration_peak;
    uint32_t irq3_deliveries, irq6_deliveries;
    uint32_t waits_reached, waits_refused, waits_parked;
    uint32_t title_draws;
    uint32_t running_owner;
} PcFrameAccounting;

void pc_frame_accounting(PcFrameAccounting *out);

/* Name every screen change with the frame it happened on. A phase timeline is
 * what makes two runs comparable: `tools/oracle_diff.py` builds the retired
 * reference product out of tree, has it emit the same lines, and diffs the two
 * timelines phase by phase — a screen that runs at the wrong speed shows up as
 * a frame count that does not match. Logged once per presented frame, only when
 * cop1lc actually changes. */
void pc_note_frame_phase(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* BENEFACTOR_PORT_FRAME_ACCOUNTING_H */
