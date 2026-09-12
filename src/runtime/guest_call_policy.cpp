#include "runtime/guest_call_policy.h"

#include <amigaport/executor.hpp>

namespace benefactor::runtime {

NativeEntry GuestCallPolicy::observe_entry(const amigaport::Executor &executor,
                                           std::uint32_t address) const noexcept {
    amigaport::ExecutionTraceEntry entry{};
    if (executor.recent_execution(&entry, 1U) == 0U ||
        entry.image_tag != executor.image().tag.value)
        return {.address = address};
    return {.address = address, .transfer_opcode = entry.opcode};
}

} // namespace benefactor::runtime
