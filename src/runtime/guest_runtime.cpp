#include "runtime/guest_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif
#include "common/log.h"
#include "engine/hw.h"
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

    void reset(M68KCtx *ctx, BenefactorImageKind kind) {
        executor.state() = {};
        activate(kind);
        bind(ctx);
    }

    void register_native(std::uint32_t image_mask, std::uint32_t address, NativeFn function) {
        if (function == nullptr)
            throw std::invalid_argument("native override function is null");
        registrations.push_back({image_mask, address, function});
        const auto image = executor.image();
        if ((image_mask & image_mask_for_tag(image.tag.value)) != 0u)
            install(image, registrations.back());
    }

    amigaport::ExecutionExit execute(std::uint32_t address) {
        last_call_address.store(address, std::memory_order_relaxed);
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
        executor.state().pc = address;
        executor.state().prefetch_valid = false;
        return executor.call_original();
    }

    amigaport::ExecutionExit call_original_subroutine(std::uint32_t address) {
        last_call_address.store(address, std::memory_order_relaxed);
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
    std::atomic<std::uint32_t> last_call_address{};
    std::atomic<std::uint32_t> last_pc{};

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
        executor.register_override(identity, [this, function = registration.function](auto &) {
            M68KCtx context{};
            bind(&context);
            bool continue_execution = false;
            native_continuations.push_back(&continue_execution);
            try {
                function(&context);
            } catch (...) {
                native_continuations.pop_back();
                throw;
            }
            native_continuations.pop_back();
            amigaport::ExecutionExit result{};
            result.continue_execution = continue_execution;
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

void log_exit(const amigaport::ExecutionExit &exit) {
    if (exit.reason == amigaport::ExitReason::MemoryFault ||
        exit.reason == amigaport::ExitReason::Exception ||
        exit.reason == amigaport::ExitReason::UnsupportedInstruction) {
        benefactor_log_write(BENEFACTOR_LOG_ERROR, "runtime",
                             "guest execution stopped: reason=%u pc=$%06X instructions=%u",
                             static_cast<unsigned>(exit.reason), exit.identity.address,
                             exit.instructions);
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
    log_exit(runtime().execute(address));
}

void rt_call_interrupt(M68KCtx *ctx, BenefactorImageIdentity image, uint32_t address) {
    (void)image_kind(image);
    if (ctx != nullptr)
        rt_context_bind(ctx);
    log_exit(runtime().call_interrupt(address));
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

void rt_call_original(M68KCtx *ctx, BenefactorImageIdentity image, uint32_t address) {
    (void)image;
    (void)address;
    if (ctx != nullptr)
        rt_context_bind(ctx);
    log_exit(runtime().call_original(address));
}

void rt_call_original_subroutine(M68KCtx *ctx, BenefactorImageIdentity image, uint32_t address) {
    (void)image;
    if (ctx != nullptr)
        rt_context_bind(ctx);
    log_exit(runtime().call_original_subroutine(address));
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
int rt_insn_ring_snapshot(uint32_t *, int) { return 0; }
int rt_recent_snapshot(uint32_t *, int) { return 0; }

} // extern "C"
