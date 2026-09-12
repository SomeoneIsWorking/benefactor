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

struct GuestImageToken final {
    std::uint32_t kind{};
    std::uint64_t generation{};
};

enum class GuestCallBoundary : std::uint8_t { Subroutine, TailTransfer, HostSubroutine };

class GuestCallPolicy final {
  public:
    [[nodiscard]] static constexpr bool image_matches(GuestImageToken requested,
                                                      GuestImageToken active) noexcept {
        return requested.kind != 0U && requested.kind == active.kind &&
               requested.generation != 0U && requested.generation == active.generation;
    }

    [[nodiscard]] static constexpr bool completes_replacement(bool replaces_subroutine,
                                                              bool continued, bool host_exit,
                                                              std::uint32_t entry_pc,
                                                              std::uint32_t current_pc) noexcept {
        return replaces_subroutine && !continued && !host_exit && current_pc == entry_pc;
    }

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
        if (caller.address != target)
            return GuestCallBoundary::HostSubroutine;
        return branch_without_link || jump ? GuestCallBoundary::TailTransfer
                                           : GuestCallBoundary::Subroutine;
    }
};

} // namespace benefactor::runtime
