#include "runtime/guest_call_policy.h"

namespace benefactor::runtime {

NativeEntry GuestCallPolicy::observe_entry(const amigaport::Executor &executor,
                                           std::uint32_t address) const noexcept {
    amigaport::ExecutionTraceEntry entry{};
    if (executor.recent_execution(&entry, 1U) == 0U ||
        entry.image_tag != executor.image().tag.value)
        return {.address = address};
    return {.address = address, .transfer_opcode = entry.opcode};
}

amigaport::CallBoundary GuestCallPolicy::nested_boundary(const NativeEntry &caller,
                                                         std::uint32_t target) const noexcept {
    /* A self-call from a branch/jump is a flat trampoline continuation. Any
     * other rt_call owns a guest subroutine return already present on A7. */
    if (caller.address == target && is_tail_transfer(caller.transfer_opcode))
        return amigaport::CallBoundary::TailTransfer;
    return amigaport::CallBoundary::GuestSubroutine;
}

bool GuestCallPolicy::is_tail_transfer(std::uint16_t opcode) noexcept {
    /* BRA/Bcc and JMP are the only transfers that do not push a guest return
     * address in the 68000 paths this title hands to native code. */
    return (opcode & 0xFF00U) == 0x6000U || (opcode & 0xFFC0U) == 0x4EC0U;
}

} // namespace benefactor::runtime
