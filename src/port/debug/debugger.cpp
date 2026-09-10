#include "port/debug/debugger.h"

#include "port/control/input_script.h"

extern "C" {
#include "common/log.h"
#include "engine/hw.h"
#include <cstdio>

#include "port/frame_accounting.h"
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
    /* Freeze how it got here BEFORE anything else runs. See Stop::trace. */
    last_.trace_length = rt_insn_ring_entries(last_.trace, last_.trace_opcodes, kStopTraceDepth);
    /* Hold at the next frame boundary. Ending the slice alone would let the
     * game flow run straight on, and the state at the breakpoint would be gone
     * before anything could read it. */
    control::InputScript::instance().pause();
    benefactor_log_write(BENEFACTOR_LOG_INFO, "debug",
                         "breakpoint $%06X reached at pc $%06X, frame %d, owner %u; holding",
                         address, program_counter, last_.frame, (unsigned)pc_running_owner());
}

Stop Debugger::last_stop() const { return last_; }

std::size_t Debugger::format_stop_trace(char *buffer, std::size_t capacity) const {
    if (buffer == nullptr || capacity == 0) {
        return 0;
    }
    buffer[0] = '\0';
    if (!last_.valid || last_.trace_length <= 0) {
        return 0;
    }
    std::size_t used = 0;
    for (int index = 0; index < last_.trace_length; index++) {
        const int written =
            snprintf(buffer + used, capacity - used, "%s$%06X %04X", used ? " " : "",
                     last_.trace[index], last_.trace_opcodes[index]);
        if (written < 0 || (std::size_t)written >= capacity - used) {
            break;
        }
        used += (std::size_t)written;
    }
    return used;
}

void Debugger::forget_last_stop() { last_ = Stop{}; }

} // namespace benefactor::debug

extern "C" void pc_debug_breakpoint_reached(unsigned int address, unsigned int program_counter) {
    benefactor::debug::Debugger::instance().reached(address, program_counter);
}
