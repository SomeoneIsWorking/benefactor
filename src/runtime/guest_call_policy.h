#pragma once

#include <cstdint>

namespace amigaport {
class Executor;
}

namespace benefactor::runtime {

/* Benefactor's native bodies are entered by two different guest contracts:
 * a JSR/BSR-style subroutine call, or a branch/jump trampoline. The shared
 * interpreter deliberately does not know this title policy. */
struct NativeEntry final {
    std::uint32_t address{};
    std::uint16_t transfer_opcode{};
};

enum class GuestCallBoundary : std::uint8_t { Subroutine, TailTransfer };

class GuestCallPolicy final {
  public:
    [[nodiscard]] NativeEntry observe_entry(const amigaport::Executor &executor,
                                            std::uint32_t address) const noexcept;

    [[nodiscard]] static constexpr GuestCallBoundary
    nested_boundary(const NativeEntry &caller, std::uint32_t target) noexcept {
        const std::uint16_t opcode = caller.transfer_opcode;
        /* BRA and conditional branches do not push a return address; BSR
         * (0x61xx) does. JMP is also a tail transfer, whereas JSR is not. */
        const bool branch_without_link =
            (opcode & 0xF000U) == 0x6000U && (opcode & 0x0F00U) != 0x0100U;
        const bool jump = (opcode & 0xFFC0U) == 0x4EC0U;
        return caller.address == target && (branch_without_link || jump)
                   ? GuestCallBoundary::TailTransfer
                   : GuestCallBoundary::Subroutine;
    }
};

} // namespace benefactor::runtime
