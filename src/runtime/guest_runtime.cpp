#include "runtime/guest_runtime.h"
#include "engine/hw.h"

#ifdef __cplusplus
extern "C" {
#endif
#include "common/log.h"
#ifdef __cplusplus
}
#endif

#include <amigaport/executor.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint32_t kChipStart = 0x00DFF000u;
constexpr std::uint32_t kChipEnd = 0x00E00000u;
constexpr std::uint32_t kCiaBStart = 0x00BFD000u;
constexpr std::uint32_t kCiaBEnd = 0x00BFE000u;
constexpr std::uint32_t kCiaAStart = 0x00BFE000u;
constexpr std::uint32_t kCiaAEnd = 0x00BFF000u;

bool is_hardware_address(std::uint32_t address) {
    return (address >= kChipStart && address < kChipEnd) ||
           (address >= kCiaBStart && address < kCiaBEnd) ||
           (address >= kCiaAStart && address < kCiaAEnd);
}

template <typename T> amigaport::MemoryRead<T> unmapped() {
    return {.value = 0, .fault = amigaport::MemoryFault::Unmapped};
}

class GuestMemory final : public amigaport::Memory {
  public:
    explicit GuestMemory(std::vector<std::uint8_t> &bytes) : bytes_(bytes) {}

    amigaport::MemoryRead<std::uint8_t> read8(amigaport::GuestAddress address) override {
        if (is_hardware_address(address))
            return {.value = hw_read8(address), .fault = amigaport::MemoryFault::None};
        if (address >= bytes_.size())
            return unmapped<std::uint8_t>();
        return {.value = bytes_[address], .fault = amigaport::MemoryFault::None};
    }

    amigaport::MemoryRead<std::uint16_t> read16(amigaport::GuestAddress address) override {
        if ((address & 1u) != 0u)
            return unmapped<std::uint16_t>();
        if (is_hardware_address(address))
            return {.value = hw_read16(address), .fault = amigaport::MemoryFault::None};
        if (address > bytes_.size() - 2u)
            return unmapped<std::uint16_t>();
        return {.value = static_cast<std::uint16_t>(bytes_[address] << 8u | bytes_[address + 1u]),
                .fault = amigaport::MemoryFault::None};
    }

    amigaport::MemoryRead<std::uint32_t> read32(amigaport::GuestAddress address) override {
        if ((address & 1u) != 0u)
            return unmapped<std::uint32_t>();
        if (is_hardware_address(address))
            return {.value = hw_read32(address), .fault = amigaport::MemoryFault::None};
        if (address > bytes_.size() - 4u)
            return unmapped<std::uint32_t>();
        const std::uint32_t value = (static_cast<std::uint32_t>(bytes_[address]) << 24u) |
                                    (static_cast<std::uint32_t>(bytes_[address + 1u]) << 16u) |
                                    (static_cast<std::uint32_t>(bytes_[address + 2u]) << 8u) |
                                    static_cast<std::uint32_t>(bytes_[address + 3u]);
        return {.value = value, .fault = amigaport::MemoryFault::None};
    }

    amigaport::MemoryFault write8(amigaport::MemoryWrite<std::uint8_t> write) override {
        if (is_hardware_address(write.address)) {
            hw_write8(write.address, write.value);
            return amigaport::MemoryFault::None;
        }
        if (write.address >= bytes_.size())
            return amigaport::MemoryFault::Unmapped;
        bytes_[write.address] = write.value;
        return amigaport::MemoryFault::None;
    }

    amigaport::MemoryFault write16(amigaport::MemoryWrite<std::uint16_t> write) override {
        if ((write.address & 1u) != 0u)
            return amigaport::MemoryFault::Misaligned;
        if (is_hardware_address(write.address)) {
            hw_write16(write.address, write.value);
            return amigaport::MemoryFault::None;
        }
        if (write.address > bytes_.size() - 2u)
            return amigaport::MemoryFault::Unmapped;
        bytes_[write.address] = static_cast<std::uint8_t>(write.value >> 8u);
        bytes_[write.address + 1u] = static_cast<std::uint8_t>(write.value);
        return amigaport::MemoryFault::None;
    }

    amigaport::MemoryFault write32(amigaport::MemoryWrite<std::uint32_t> write) override {
        if ((write.address & 1u) != 0u)
            return amigaport::MemoryFault::Misaligned;
        if (is_hardware_address(write.address)) {
            hw_write32(write.address, write.value);
            return amigaport::MemoryFault::None;
        }
        if (write.address > bytes_.size() - 4u)
            return amigaport::MemoryFault::Unmapped;
        bytes_[write.address] = static_cast<std::uint8_t>(write.value >> 24u);
        bytes_[write.address + 1u] = static_cast<std::uint8_t>(write.value >> 16u);
        bytes_[write.address + 2u] = static_cast<std::uint8_t>(write.value >> 8u);
        bytes_[write.address + 3u] = static_cast<std::uint8_t>(write.value);
        return amigaport::MemoryFault::None;
    }

  private:
    std::vector<std::uint8_t> &bytes_;
};

class RuntimeLogger final : public amigaport::Logger {
  public:
    void write(amigaport::LogLevel level, std::string_view,
               std::string_view message) noexcept override {
        const auto mapped = [&]() {
            switch (level) {
            case amigaport::LogLevel::Trace:
                return BENEFACTOR_LOG_TRACE;
            case amigaport::LogLevel::Debug:
                return BENEFACTOR_LOG_DEBUG;
            case amigaport::LogLevel::Info:
                return BENEFACTOR_LOG_INFO;
            case amigaport::LogLevel::Warning:
                return BENEFACTOR_LOG_WARNING;
            case amigaport::LogLevel::Error:
                return BENEFACTOR_LOG_ERROR;
            }
            return BENEFACTOR_LOG_ERROR;
        }();
        benefactor_log_write(mapped, "amigaport", "%.*s", static_cast<int>(message.size()),
                             message.data());
    }
};

struct Registration final {
    std::uint32_t image_mask{};
    std::uint32_t address{};
    NativeFn function{};
    /* True when the native body wholly replaces a guest subroutine, so the
     * adapter owes its RTS: pop the guest return address once the body has
     * run. False when the body drives the boundary itself (an original call,
     * a continuation, or its own return). */
    bool replaces_subroutine{};
};

class Runtime final {
  public:
    Runtime()
        : bytes(BENEFACTOR_GUEST_MEMORY_SIZE), memory(bytes),
          executor({.max_instructions_per_slice = 1'000'000u}, memory, logger) {}

    amigaport::ImageIdentity activate(BenefactorImageKind kind) {
        const auto image = executor.replace_image({.value = static_cast<std::uint32_t>(kind)});
        for (const Registration &registration : registrations) {
            if ((registration.image_mask & image_mask(kind)) == 0u)
                continue;
            install(image, registration);
        }
        return image;
    }

    void bind(M68KCtx *ctx) {
        if (ctx == nullptr)
            throw std::invalid_argument("cannot bind a null M68K context");
        ctx->amigaport_runtime = this;
        ctx->D = executor.state().data.data();
        ctx->A = executor.state().address.data();
        ctx->sr = &executor.state().sr;
        ctx->memory = bytes.data();
        ctx->memory_size = bytes.size();
        const auto image = executor.image();
        ctx->image = {.kind = static_cast<BenefactorImageKind>(image.tag.value),
                      .generation = image.generation};
    }

    /* Guest time never runs backwards for the host, even though the CPU state
     * itself can be reset or rolled back (an interrupt that does not reach its
     * RTE restores the state it saved). Fold what has elapsed into the base
     * before discarding it, and latch the peak on the way out. */
    std::uint64_t guest_cycles() noexcept {
        const std::uint64_t now = cycle_base + executor.state().elapsed_cycles;
        const std::uint64_t seen = cycles_seen.load(std::memory_order_relaxed);
        if (now <= seen)
            return seen;
        cycles_seen.store(now, std::memory_order_relaxed);
        return now;
    }

    void add_guest_cycles(std::uint64_t cycles) noexcept { cycle_base += cycles; }

    [[nodiscard]] std::uint64_t cycle_base_value() const noexcept { return cycle_base; }
    [[nodiscard]] std::uint64_t cycles_elapsed_value() const noexcept {
        return executor.state().elapsed_cycles;
    }

    void reset(M68KCtx *ctx, BenefactorImageKind kind) {
        cycle_base += executor.state().elapsed_cycles;
        executor.state() = {};
        activate(kind);
        bind(ctx);
    }

    void register_native(std::uint32_t image_mask, std::uint32_t address, NativeFn function,
                         bool replaces_subroutine = false) {
        if (function == nullptr)
            throw std::invalid_argument("native override function is null");
        registrations.push_back({image_mask, address, function, replaces_subroutine});
        const auto image = executor.image();
        if ((image_mask & image_mask_for_tag(image.tag.value)) != 0u)
            install(image, registrations.back());
    }

    void record_call(std::uint32_t address) noexcept {
        const std::uint64_t index = calls_written.load(std::memory_order_relaxed);
        calls[index % kCallRingCapacity].store(address, std::memory_order_relaxed);
        calls_written.store(index + 1U, std::memory_order_relaxed);
    }

    int recent_calls(std::uint32_t *destination, int capacity) const noexcept {
        if (destination == nullptr || capacity <= 0)
            return 0;
        const std::uint64_t written = calls_written.load(std::memory_order_relaxed);
        const std::uint64_t available = std::min<std::uint64_t>(written, kCallRingCapacity);
        const std::uint64_t wanted =
            std::min<std::uint64_t>(available, static_cast<std::uint64_t>(capacity));
        int count = 0;
        for (std::uint64_t offset = wanted; offset > 0U; --offset)
            destination[count++] =
                calls[(written - offset) % kCallRingCapacity].load(std::memory_order_relaxed);
        return count;
    }

    amigaport::ExecutionExit execute(std::uint32_t address) {
        last_call_address.store(address, std::memory_order_relaxed);
        record_call(address);
        amigaport::ExecutionExit result = executor.call(address);
        while (result.reason == amigaport::ExitReason::InstructionBudget ||
               result.reason == amigaport::ExitReason::NativeOverride) {
            result = executor.execute();
        }
        if (result.reason == amigaport::ExitReason::MemoryFault) {
            benefactor_log_write(BENEFACTOR_LOG_ERROR, "runtime",
                                 "guest memory fault pc=$%06X address=$%08X opcode=$%04X "
                                 "a0=$%08X a1=$%08X a2=$%08X a3=$%08X a4=$%08X call=$%06X",
                                 result.identity.address, executor.state().exception.fault_address,
                                 executor.state().exception.instruction_word,
                                 executor.state().address[0], executor.state().address[1],
                                 executor.state().address[2], executor.state().address[3],
                                 executor.state().address[4],
                                 last_call_address.load(std::memory_order_relaxed));
        }
        last_pc.store(executor.state().pc, std::memory_order_relaxed);
        return result;
    }

    amigaport::ExecutionExit call_original(std::uint32_t address) {
        last_call_address.store(address, std::memory_order_relaxed);
        record_call(address);
        executor.state().pc = address;
        executor.state().prefetch_valid = false;
        return executor.call_original();
    }

    amigaport::ExecutionExit call_original_subroutine(std::uint32_t address) {
        last_call_address.store(address, std::memory_order_relaxed);
        record_call(address);
        executor.state().pc = address;
        executor.state().prefetch_valid = false;
        return executor.call_original_subroutine();
    }

    int return_from_native() {
        const auto return_pc = memory.read32(executor.state().address[7]);
        if (!return_pc) {
            benefactor_log_write(BENEFACTOR_LOG_ERROR, "runtime",
                                 "native replacement return address read failed at $%06X",
                                 executor.state().address[7]);
            return -1;
        }
        if ((return_pc.value & 1u) != 0u || return_pc.value >= bytes.size()) {
            benefactor_log_write(BENEFACTOR_LOG_ERROR, "runtime",
                                 "native replacement returned to invalid PC $%06X from $%06X",
                                 return_pc.value, executor.state().address[7]);
            return -1;
        }
        executor.state().address[7] += 4u;
        executor.state().pc = return_pc.value;
        executor.state().prefetch_valid = false;
        return 0;
    }

    amigaport::ExecutionExit call_interrupt(std::uint32_t address) {
        return executor.call_interrupt(address);
    }

    void exit_to_host() {
        if (native_host_exits.empty())
            throw std::logic_error("host exit requested outside an override");
        *native_host_exits.back() = true;
    }

    void continue_from_native(std::uint32_t address) {
        if ((address & 1u) != 0u || address >= bytes.size()) {
            throw std::invalid_argument("native continuation target is not a valid guest PC");
        }
        if (native_continuations.empty())
            throw std::logic_error("native continuation requested outside an override");
        executor.state().pc = address;
        executor.state().prefetch_valid = false;
        *native_continuations.back() = true;
    }

    amigaport::MemoryRead<std::uint8_t> read8(std::uint32_t address) {
        return memory.read8(address);
    }
    amigaport::MemoryRead<std::uint16_t> read16(std::uint32_t address) {
        return memory.read16(address);
    }
    amigaport::MemoryRead<std::uint32_t> read32(std::uint32_t address) {
        return memory.read32(address);
    }
    void write8(std::uint32_t address, std::uint8_t value) {
        (void)memory.write8({.address = address, .value = value});
    }
    void write16(std::uint32_t address, std::uint16_t value) {
        (void)memory.write16({.address = address, .value = value});
    }
    void write32(std::uint32_t address, std::uint32_t value) {
        (void)memory.write32({.address = address, .value = value});
    }

    std::vector<std::uint8_t> bytes;
    GuestMemory memory;
    RuntimeLogger logger;
    amigaport::Executor executor;
    std::vector<Registration> registrations;
    std::vector<bool *> native_continuations;
    std::vector<bool *> native_host_exits;
    std::atomic<std::uint32_t> last_call_address{};
    std::atomic<std::uint32_t> last_pc{};
    std::uint64_t cycle_base{};
    std::atomic<std::uint64_t> cycles_seen{};
    static constexpr std::size_t kCallRingCapacity = 64U;
    std::array<std::atomic<std::uint32_t>, kCallRingCapacity> calls{};
    std::atomic<std::uint64_t> calls_written{0};

  private:
    static std::uint32_t image_mask(BenefactorImageKind kind) {
        return 1u << (static_cast<std::uint32_t>(kind) - 1u);
    }

    static std::uint32_t image_mask_for_tag(std::uint32_t tag) {
        return tag >= BENEFACTOR_IMAGE_MAIN && tag <= BENEFACTOR_IMAGE_CREDITS ? 1u << (tag - 1u)
                                                                               : 0u;
    }

    void install(amigaport::ImageIdentity image, const Registration &registration) {
        const amigaport::ExecutionIdentity identity{.image = image,
                                                    .address = registration.address};
        executor.register_override(
            identity, [this, function = registration.function, address = registration.address,
                       replaces_subroutine = registration.replaces_subroutine](auto &) {
                M68KCtx context{};
                bind(&context);
                bool continue_execution = false;
                bool exit_to_host = false;
                native_continuations.push_back(&continue_execution);
                native_host_exits.push_back(&exit_to_host);
                try {
                    function(&context);
                } catch (...) {
                    native_continuations.pop_back();
                    native_host_exits.pop_back();
                    throw;
                }
                native_continuations.pop_back();
                native_host_exits.pop_back();
                /* Complete the replaced subroutine's RTS only when the body left the
                 * boundary untouched. A path that called the original, jumped, or
                 * exited to the host has already moved the PC and consumed whatever
                 * the guest stack owed. */
                if (replaces_subroutine && !continue_execution && !exit_to_host &&
                    executor.state().pc == address)
                    (void)return_from_native();
                amigaport::ExecutionExit result{};
                result.continue_execution = continue_execution;
                result.hand_off_to_host = exit_to_host;
                result.reason = amigaport::ExitReason::NativeOverride;
                result.identity.image = executor.image();
                result.identity.address = executor.state().pc;
                return result;
            });
    }
};

std::unique_ptr<Runtime> g_runtime;
Runtime &runtime() {
    if (!g_runtime)
        throw std::logic_error("Benefactor runtime is not initialized");
    return *g_runtime;
}

BenefactorImageKind image_kind(BenefactorImageIdentity image) { return image.kind; }

/* Name the instruction that transferred control into an override, so a missing
 * boundary says whether the guest arrived by JSR/BSR (the replacement owes an
 * rt_return_from_native) or by JMP/BRA (it owes an explicit next PC). */
void log_unterminated_override(const amigaport::ExecutionExit &exit) {
    std::array<amigaport::ExecutionTraceEntry, 2> entries{};
    const std::size_t count = runtime().executor.recent_execution(entries.data(), entries.size());
    const amigaport::ExecutionTraceEntry entered =
        count > 0 ? entries[count - 1] : amigaport::ExecutionTraceEntry{};
    benefactor_log_write(BENEFACTOR_LOG_ERROR, "runtime",
                         "native override $%06X returned without completing its guest boundary; "
                         "entered from $%06X opcode=$%04X image=%u a7=$%08X return=$%08X "
                         "continue=%d handoff=%d",
                         exit.identity.address, entered.pc, entered.opcode,
                         static_cast<unsigned>(exit.identity.image.tag.value),
                         runtime().executor.state().address[7],
                         runtime().read32(runtime().executor.state().address[7]).value,
                         exit.continue_execution ? 1 : 0, exit.hand_off_to_host ? 1 : 0);
}

const char *exit_reason_name(amigaport::ExitReason reason) {
    switch (reason) {
    case amigaport::ExitReason::NoImage:
        return "no-image";
    case amigaport::ExitReason::InstructionBudget:
        return "instruction-budget";
    case amigaport::ExitReason::NativeOverride:
        return "native-override";
    case amigaport::ExitReason::ReturnToHost:
        return "return-to-host";
    case amigaport::ExitReason::MemoryFault:
        return "memory-fault";
    case amigaport::ExitReason::UnsupportedInstruction:
        return "unsupported-instruction";
    case amigaport::ExitReason::Exception:
        return "exception";
    case amigaport::ExitReason::Halted:
        return "halted";
    case amigaport::ExitReason::ImageReplaced:
        return "image-replaced";
    case amigaport::ExitReason::UnterminatedNativeOverride:
        return "unterminated-native-override";
    }
    return "unknown";
}

/* Which host entry started a run, and where it asked the guest to begin. An
 * exit reason alone cannot say whose run it was: "1,000,000 instructions ending
 * at $3732" reads identically whether it was the game flow's own slice or an
 * interrupt delivery that never came back, and those want opposite fixes. */
void log_exit(const char *entry, std::uint32_t entry_address,
              const amigaport::ExecutionExit &exit) {
    /* Every guest call ends for exactly one reason. Naming it — even the benign
     * ones, at debug level — is what turns "the flow just returned" into an
     * answer, so no exit leaves the interpreter silently. */
    benefactor_log_write(BENEFACTOR_LOG_DEBUG, "runtime",
                         "guest call exit: entry=%s($%06X) reason=%s pc=$%06X "
                         "instructions=%u image=%u",
                         entry, entry_address, exit_reason_name(exit.reason), exit.identity.address,
                         exit.instructions, static_cast<unsigned>(exit.identity.image.tag.value));
    if (exit.reason == amigaport::ExitReason::UnterminatedNativeOverride) {
        log_unterminated_override(exit);
        return;
    }
    if (exit.reason == amigaport::ExitReason::MemoryFault ||
        exit.reason == amigaport::ExitReason::Exception ||
        exit.reason == amigaport::ExitReason::UnsupportedInstruction) {
        const auto &state = runtime().executor.state();
        benefactor_log_write(
            BENEFACTOR_LOG_ERROR, "runtime",
            "guest execution stopped: reason=%u pc=$%06X instructions=%u "
            "vector=%u opcode=$%04X d0=$%08X a5=$%08X a7=$%08X sr=$%04X "
            "image=%u generation=%llu",
            static_cast<unsigned>(exit.reason), exit.identity.address, exit.instructions,
            static_cast<unsigned>(state.exception.active_vector), exit.instruction_word,
            state.data[0], state.address[5], state.address[7], state.sr,
            static_cast<unsigned>(runtime().executor.image().tag.value),
            static_cast<unsigned long long>(runtime().executor.image().generation));
    }
}

template <typename Function> int boundary(Function &&function) {
    try {
        function();
        return 0;
    } catch (const std::exception &error) {
        benefactor_log_write(BENEFACTOR_LOG_ERROR, "runtime", "%s", error.what());
        return -1;
    }
}

} // namespace

extern "C" {

uint8_t *g_mem = nullptr;

uint8_t rt_read8(M68KCtx *, uint32_t address) {
    const auto result = runtime().read8(address);
    return result ? result.value : 0;
}

uint16_t rt_read16(M68KCtx *, uint32_t address) {
    const auto result = runtime().read16(address);
    return result ? result.value : 0;
}

uint32_t rt_read32(M68KCtx *, uint32_t address) {
    const auto result = runtime().read32(address);
    return result ? result.value : 0;
}

void rt_write8(M68KCtx *, uint32_t address, uint8_t value) { runtime().write8(address, value); }
void rt_write16(M68KCtx *, uint32_t address, uint16_t value) { runtime().write16(address, value); }
void rt_write32(M68KCtx *, uint32_t address, uint32_t value) { runtime().write32(address, value); }

void rt_register_native(uint32_t image_mask, uint32_t address, NativeFn function) {
    runtime().register_native(image_mask, address, function);
}

void rt_register_override(uint32_t address, NativeFn function) {
    rt_register_native(BENEFACTOR_IMAGE_MASK_MAIN | BENEFACTOR_IMAGE_MASK_TITLE |
                           BENEFACTOR_IMAGE_MASK_CREDITS,
                       address, function);
}

void rt_register_override_gp(uint32_t address, NativeFn function) {
    rt_register_native(BENEFACTOR_IMAGE_MASK_GAMEPLAY, address, function);
}

void rt_register_replacement(uint32_t address, NativeFn function) {
    runtime().register_native(BENEFACTOR_IMAGE_MASK_MAIN | BENEFACTOR_IMAGE_MASK_TITLE |
                                  BENEFACTOR_IMAGE_MASK_CREDITS,
                              address, function, true);
}

void rt_register_replacement_gp(uint32_t address, NativeFn function) {
    runtime().register_native(BENEFACTOR_IMAGE_MASK_GAMEPLAY, address, function, true);
}

void rt_context_bind(M68KCtx *ctx) { runtime().bind(ctx); }

void rt_context_reset(M68KCtx *ctx, BenefactorImageKind kind) { runtime().reset(ctx, kind); }

void rt_activate_image(M68KCtx *ctx, BenefactorImageKind kind) {
    runtime().activate(kind);
    if (ctx != nullptr)
        runtime().bind(ctx);
}

void rt_call(M68KCtx *ctx, BenefactorImageIdentity image, uint32_t address) {
    (void)image_kind(image);
    if (ctx != nullptr)
        rt_context_bind(ctx);
    log_exit("execute", address, runtime().execute(address));
}

void rt_call_interrupt(M68KCtx *ctx, BenefactorImageIdentity image, uint32_t address) {
    (void)image_kind(image);
    if (ctx != nullptr)
        rt_context_bind(ctx);
    log_exit("interrupt", address, runtime().call_interrupt(address));
}

void rt_jump(M68KCtx *ctx, BenefactorImageIdentity image, uint32_t address) {
    (void)image;
    if (ctx != nullptr)
        rt_context_bind(ctx);
    /* Native overrides return to the executor after this function returns.
     * Mutate that frame's PC instead of recursively starting a second PUAE
     * step; the embedded core owns one architectural context and is not
     * reentrant. */
    runtime().continue_from_native(address);
}

void rt_exit_to_host(M68KCtx *ctx) {
    if (ctx != nullptr)
        rt_context_bind(ctx);
    runtime().exit_to_host();
}

void rt_call_original(M68KCtx *ctx, BenefactorImageIdentity image, uint32_t address) {
    (void)image;
    (void)address;
    if (ctx != nullptr)
        rt_context_bind(ctx);
    log_exit("call-original", address, runtime().call_original(address));
}

void rt_call_original_subroutine(M68KCtx *ctx, BenefactorImageIdentity image, uint32_t address) {
    (void)image;
    if (ctx != nullptr)
        rt_context_bind(ctx);
    log_exit("call-original-sub", address, runtime().call_original_subroutine(address));
}

int rt_return_from_native(M68KCtx *ctx) {
    if (ctx != nullptr)
        rt_context_bind(ctx);
    return runtime().return_from_native();
}

void rt_reset_callstack(void) {}

int rt_init(const char *, uint32_t, uint32_t) {
    if (g_runtime)
        return -1;
    const int result = boundary([] {
        g_runtime = std::make_unique<Runtime>();
        g_runtime->activate(BENEFACTOR_IMAGE_MAIN);
        g_mem = g_runtime->bytes.data();
    });
    if (result != 0)
        g_runtime.reset();
    return result;
}

void rt_fini(void) {
    g_mem = nullptr;
    g_runtime.reset();
}

void rt_resume(M68KCtx *ctx, BenefactorImageIdentity image, uint32_t address) {
    rt_call(ctx, image, address);
}

int rt_has_guest_code(BenefactorImageIdentity, uint32_t address) {
    return address < BENEFACTOR_GUEST_MEMORY_SIZE;
}

int rt_is_resume_point(const M68KCtx *ctx, BenefactorImageIdentity image, uint32_t address) {
    if (ctx == nullptr || ctx->amigaport_runtime != g_runtime.get())
        return 0;
    const auto current = runtime().executor.image();
    return current.tag.value == image.kind && current.generation == image.generation &&
           runtime().executor.state().pc == address;
}

size_t rt_state_blob_size(void) { return sizeof(amigaport::CpuState); }

int rt_state_blob_save(void *destination, size_t capacity) {
    if (destination == nullptr || capacity < sizeof(amigaport::CpuState))
        return -1;
    std::memcpy(destination, &runtime().executor.state(), sizeof(amigaport::CpuState));
    return 0;
}

int rt_state_blob_load(const void *source, size_t size) {
    if (source == nullptr || size != sizeof(amigaport::CpuState))
        return -1;
    std::memcpy(&runtime().executor.state(), source, sizeof(amigaport::CpuState));
    return 0;
}

void rt_chip_rwatch_add(uint32_t, uint32_t) {}
void rt_chip_rwatch_clear(void) {}
void rt_chip_watch_add(uint32_t, uint32_t) {}
void rt_chip_watch_clear(void) {}
uint32_t rt_get_last_insn(void) { return runtime().last_pc.load(std::memory_order_relaxed); }
uint32_t rt_get_active_call_address(void) {
    return runtime().last_call_address.load(std::memory_order_relaxed);
}
uint32_t rt_get_pc(void) { return g_runtime ? g_runtime->executor.state().pc : 0u; }

uint64_t rt_get_guest_cycles(void) { return g_runtime ? g_runtime->guest_cycles() : 0u; }

/* Charge guest time for work the host performs instantly on the guest's behalf.
 * The blitter is the case that matters: on hardware a blit occupies the bus for
 * a computed number of cycles and the game's WaitBlit spins for exactly that
 * long. Completing it for free made every blitter-paced screen run as fast as
 * the host could interpret. */
void rt_add_guest_cycles(uint64_t cycles) {
    if (g_runtime)
        g_runtime->add_guest_cycles(cycles);
}

/* Raw halves of the guest clock, for diagnosing a clock that disagrees with the
 * work actually done: the folded base plus the live executor counter, which an
 * interrupt that never reaches its RTE rolls backwards. */
uint64_t rt_get_cycle_base(void) { return g_runtime ? g_runtime->cycle_base_value() : 0u; }
uint64_t rt_get_cycles_elapsed(void) { return g_runtime ? g_runtime->cycles_elapsed_value() : 0u; }

uint64_t rt_get_executed_instructions(void) {
    return g_runtime ? g_runtime->executor.state().executed_instructions : 0u;
}

int rt_insn_ring_snapshot(uint32_t *destination, int capacity) {
    if (g_runtime == nullptr || destination == nullptr || capacity <= 0)
        return 0;
    std::array<amigaport::ExecutionTraceEntry, 256> entries{};
    const std::size_t wanted =
        std::min<std::size_t>(static_cast<std::size_t>(capacity), entries.size());
    const std::size_t count = g_runtime->executor.recent_execution(entries.data(), wanted);
    for (std::size_t index = 0; index < count; ++index)
        destination[index] = entries[index].pc;
    return static_cast<int>(count);
}

int rt_insn_ring_entries(uint32_t *program_counters, uint16_t *opcodes, int capacity) {
    if (g_runtime == nullptr || program_counters == nullptr || capacity <= 0)
        return 0;
    std::array<amigaport::ExecutionTraceEntry, 256> entries{};
    const std::size_t wanted =
        std::min<std::size_t>(static_cast<std::size_t>(capacity), entries.size());
    const std::size_t count = g_runtime->executor.recent_execution(entries.data(), wanted);
    for (std::size_t index = 0; index < count; ++index) {
        program_counters[index] = entries[index].pc;
        if (opcodes != nullptr)
            opcodes[index] = entries[index].opcode;
    }
    return static_cast<int>(count);
}

int rt_recent_snapshot(uint32_t *destination, int capacity) {
    return g_runtime ? g_runtime->recent_calls(destination, capacity) : 0;
}

} // extern "C"
