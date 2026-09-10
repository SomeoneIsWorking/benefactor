#include "port/debug/debugger.h"

#include "port/control/input_script.h"

extern "C" {
#include "common/log.h"
#include "engine/hw.h"
#include "runtime/guest_runtime.h"
}

namespace benefactor::debug {

Debugger &Debugger::instance() {
    static Debugger only;
    return only;
}

bool Debugger::set(std::uint32_t address) { return rt_set_breakpoint(address) != 0; }

bool Debugger::clear(std::uint32_t address) { return rt_clear_breakpoint(address) != 0; }

void Debugger::clear_all() { rt_clear_breakpoints(); }

std::vector<std::uint32_t> Debugger::addresses() const {
    std::vector<std::uint32_t> found(static_cast<std::size_t>(rt_breakpoint_capacity()));
    const int written = rt_breakpoints(found.data(), static_cast<int>(found.size()));
    found.resize(written < 0 ? 0U : static_cast<std::size_t>(written));
    return found;
}

int Debugger::capacity() const { return rt_breakpoint_capacity(); }

void Debugger::reached(std::uint32_t address, std::uint32_t program_counter) {
    last_ = Stop{.address = address,
                 .program_counter = program_counter,
                 .frame = hw_get_frame_num(),
                 .guest_cycles = rt_get_guest_cycles(),
                 .valid = true};
    /* Hold at the next frame boundary. Ending the slice alone would let the
     * game flow run straight on, and the state at the breakpoint would be gone
     * before anything could read it. */
    control::InputScript::instance().pause();
    benefactor_log_write(BENEFACTOR_LOG_INFO, "debug",
                         "breakpoint $%06X reached at pc $%06X, frame %d; holding", address,
                         program_counter, last_.frame);
}

Stop Debugger::last_stop() const { return last_; }

void Debugger::forget_last_stop() { last_ = Stop{}; }

} // namespace benefactor::debug

extern "C" void pc_debug_breakpoint_reached(unsigned int address, unsigned int program_counter) {
    benefactor::debug::Debugger::instance().reached(address, program_counter);
}
