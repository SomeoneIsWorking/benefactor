/* The guest debugger: breakpoints that stop the game where you can look at it.
 *
 * A breakpoint on its own only ends one execution slice — the game flow would
 * carry straight on into the next one, and by the time anything could be read
 * the moment is gone. So a breakpoint here does two things: it records where
 * the guest stopped, and it holds the game at the next frame boundary through
 * the same pause the control channel uses. The registers, memory and
 * framebuffer then all describe the same instant.
 *
 * Owned by the control channel (src/port/control/) and driven from it:
 *
 *   /break?at=3732      set one
 *   /break?clear=3732   clear one, or /break?clear=all
 *   /breaks             what is set, and where execution last stopped
 *   /step?frames=N      let N frames pass, then hold again
 *
 * See CLAUDE.md "Debugging the interpreter" for the other instruments; reach
 * for a breakpoint when you need the state AT an address, and for the
 * retired-instruction ring when you need to know how it got there.
 */
#ifndef BENEFACTOR_PORT_DEBUG_DEBUGGER_H
#define BENEFACTOR_PORT_DEBUG_DEBUGGER_H

#ifdef __cplusplus

#include <cstdint>
#include <string>
#include <vector>

namespace benefactor::debug {

/* Where the guest was when a breakpoint stopped it. */
struct Stop final {
    std::uint32_t address{};         /* the breakpoint that was reached */
    std::uint32_t program_counter{}; /* the PC, which is ON that address */
    int frame{};                     /* the displayed frame it stopped in */
    std::uint64_t guest_cycles{};
    bool valid{};
};

class Debugger final {
  public:
    static Debugger &instance();

    Debugger(const Debugger &) = delete;
    Debugger &operator=(const Debugger &) = delete;
    Debugger(Debugger &&) = delete;
    Debugger &operator=(Debugger &&) = delete;

    /* False if the address is already set, or the executor's set is full. */
    bool set(std::uint32_t address);
    bool clear(std::uint32_t address);
    void clear_all();
    [[nodiscard]] std::vector<std::uint32_t> addresses() const;
    [[nodiscard]] int capacity() const;

    /* Called from the runtime when a slice exited on a breakpoint. Records the
     * stop and asks the frame loop to hold, so the state can be read. */
    void reached(std::uint32_t address, std::uint32_t program_counter);

    [[nodiscard]] Stop last_stop() const;
    void forget_last_stop();

  private:
    Debugger() = default;

    Stop last_{};
};

} // namespace benefactor::debug

extern "C" {
#endif /* __cplusplus */

/* The C seam the runtime calls when a slice exits on a breakpoint. */
void pc_debug_breakpoint_reached(unsigned int address, unsigned int program_counter);

#ifdef __cplusplus
}
#endif

#endif /* BENEFACTOR_PORT_DEBUG_DEBUGGER_H */
