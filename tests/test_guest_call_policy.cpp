#include "runtime/guest_call_policy.h"

#include <cassert>

using benefactor::runtime::GuestCallBoundary;
using benefactor::runtime::GuestCallPolicy;
using benefactor::runtime::NativeEntry;

namespace {

constexpr std::uint32_t kNativeAddress = 0x0057901Eu;

constexpr GuestCallBoundary boundary_for(std::uint16_t opcode,
                                         std::uint32_t target = kNativeAddress) {
    return GuestCallPolicy::nested_boundary(
        NativeEntry{.address = kNativeAddress, .transfer_opcode = opcode}, target);
}

static_assert(boundary_for(0x6000U) == GuestCallBoundary::TailTransfer); // BRA
static_assert(boundary_for(0x66FEU) == GuestCallBoundary::TailTransfer); // BNE
static_assert(boundary_for(0x4ED0U) == GuestCallBoundary::TailTransfer); // JMP (A0)
static_assert(boundary_for(0x6100U) == GuestCallBoundary::Subroutine);   // BSR
static_assert(boundary_for(0x61FEU) == GuestCallBoundary::Subroutine);   // BSR.s
static_assert(boundary_for(0x4E90U) == GuestCallBoundary::Subroutine);   // JSR (A0)
static_assert(boundary_for(0x6000U, kNativeAddress + 2U) == GuestCallBoundary::Subroutine);

} // namespace

int main() {
    assert(boundary_for(0x6100U) == GuestCallBoundary::Subroutine);
    assert(boundary_for(0x6000U) == GuestCallBoundary::TailTransfer);
}
