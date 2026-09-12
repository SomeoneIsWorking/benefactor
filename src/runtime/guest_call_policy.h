#pragma once

#include <amigaport/executor.hpp>

#include <cstdint>

namespace benefactor::runtime {

/* Benefactor's native bodies are entered by two different guest contracts:
 * a JSR/BSR-style subroutine call, or a branch/jump trampoline. The shared
 * interpreter deliberately does not know this title policy. */
struct NativeEntry final {
    std::uint32_t address{};
    std::uint16_t transfer_opcode{};
};

class GuestCallPolicy final {
  public:
    [[nodiscard]] NativeEntry observe_entry(const amigaport::Executor &executor,
                                            std::uint32_t address) const noexcept;

    [[nodiscard]] amigaport::CallBoundary nested_boundary(const NativeEntry &caller,
                                                          std::uint32_t target) const noexcept;

  private:
    [[nodiscard]] static bool is_tail_transfer(std::uint16_t opcode) noexcept;
};

} // namespace benefactor::runtime
