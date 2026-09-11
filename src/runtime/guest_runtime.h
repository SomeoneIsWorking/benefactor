/* Benefactor's title-side view of shared/amigaport.
 *
 * This file declares a borrowed adapter view. It does not own or duplicate
 * 68000 architectural state: D, A, and SR point into the canonical amigaport
 * CPU context and memory points at the active image mapping.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum BenefactorImageKind {
    BENEFACTOR_IMAGE_MAIN = 1,
    BENEFACTOR_IMAGE_TITLE = 2,
    BENEFACTOR_IMAGE_GAMEPLAY = 3,
    BENEFACTOR_IMAGE_CREDITS = 4,
} BenefactorImageKind;

typedef struct BenefactorImageIdentity {
    BenefactorImageKind kind;
    uint64_t generation;
} BenefactorImageIdentity;

typedef struct M68KCtx {
    void *amigaport_runtime;
    uint32_t *D;
    uint32_t *A;
    uint16_t *sr;
    uint8_t *memory;
    size_t memory_size;
    BenefactorImageIdentity image;
} M68KCtx;

typedef void (*NativeFn)(M68KCtx *ctx);

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BENEFACTOR_IMAGE_MASK_MAIN = 1u << 0,
    BENEFACTOR_IMAGE_MASK_TITLE = 1u << 1,
    BENEFACTOR_IMAGE_MASK_GAMEPLAY = 1u << 2,
    BENEFACTOR_IMAGE_MASK_CREDITS = 1u << 3,
    BENEFACTOR_IMAGE_MASK_ALL = 0x0fu,
};

uint8_t rt_read8(M68KCtx *ctx, uint32_t addr);
uint16_t rt_read16(M68KCtx *ctx, uint32_t addr);
uint32_t rt_read32(M68KCtx *ctx, uint32_t addr);
void rt_write8(M68KCtx *ctx, uint32_t addr, uint8_t value);
void rt_write16(M68KCtx *ctx, uint32_t addr, uint16_t value);
void rt_write32(M68KCtx *ctx, uint32_t addr, uint32_t value);

#define MR8(a) rt_read8(ctx, (uint32_t)(int32_t)(a))
#define MR16(a) rt_read16(ctx, (uint32_t)(int32_t)(a))
#define MR32(a) rt_read32(ctx, (uint32_t)(int32_t)(a))
#define MW8(a, v) rt_write8(ctx, (uint32_t)(int32_t)(a), (uint8_t)(v))
#define MW16(a, v) rt_write16(ctx, (uint32_t)(int32_t)(a), (uint16_t)(v))
#define MW32(a, v) rt_write32(ctx, (uint32_t)(int32_t)(a), (uint32_t)(v))
#define RT_SX16(x) ((uint32_t)(int32_t)(int16_t)(x))

/* The active image identity is mandatory for every registration and call.
 * The adapter must reject stale generations whenever an overlay load changes
 * the executable image behind a reused guest address. */
void rt_register_native(uint32_t image_mask, uint32_t address, NativeFn function);
/* These title-side helpers are retained until the registry is converted to an
 * injected object. Their implementations must call rt_register_native with an
 * explicit mask; they are not static-dispatch tables. */
void rt_register_override(uint32_t address, NativeFn function);
void rt_register_override_gp(uint32_t address, NativeFn function);
/* TITLE only. The title overlay loads its own code over addresses the intro
 * already uses, so a menu routine registered for the intro image too is
 * entered by whatever the intro happens to keep there. Measured: the menu's
 * option setup ($003872) fired during the boot animation with the intro's own
 * a5 ($531C instead of $511E) and wrote "CONTINUE"/"LEVEL SELECT"/"OPTIONS"
 * over the crawl text at $004C78 (tools/lockstep.py, frame 3). */
void rt_register_override_title(uint32_t address, NativeFn function);
/* Register a native body that WHOLLY REPLACES a guest subroutine: the adapter
 * completes the guest boundary (the RTS the replaced body would have run) once
 * the native body returns. Use rt_register_override instead whenever the body
 * drives the boundary itself — an original call, a continuation, or its own
 * rt_return_from_native. */
void rt_register_replacement(uint32_t address, NativeFn function);
void rt_register_replacement_gp(uint32_t address, NativeFn function);
void rt_register_replacement_title(uint32_t address, NativeFn function);
void rt_context_bind(M68KCtx *ctx);
void rt_context_reset(M68KCtx *ctx, BenefactorImageKind image_kind);
void rt_activate_image(M68KCtx *ctx, BenefactorImageKind image_kind);
void rt_call(M68KCtx *ctx, BenefactorImageIdentity image, uint32_t address);
void rt_call_interrupt(M68KCtx *ctx, BenefactorImageIdentity image, uint32_t address);
void rt_jump(M68KCtx *ctx, BenefactorImageIdentity image, uint32_t address);
/* Complete a native replacement entered through a guest JSR.  Native code
 * owns the replacement body, but the guest stack and continuation remain
 * architectural state owned by the interpreter. */
int rt_return_from_native(M68KCtx *ctx);
/* End the current guest run from inside a native override and hand control back
 * to the host, which owns what executes next (a screen hand-off restarting the
 * game thread on another image, for example). The guest flow that reached the
 * override is deliberately unwound, so the override owes no return or PC. */
void rt_exit_to_host(M68KCtx *ctx);

/* Transitional host seams awaiting the adapter implementation. They are
 * declarations only; no gameplay target is built until shared/amigaport owns
 * their implementation. */
void rt_reset_callstack(void);
int rt_init(const char *binary_path, uint32_t load_addr, uint32_t stack_top);
void rt_fini(void);
void rt_resume(M68KCtx *ctx, BenefactorImageIdentity image, uint32_t address);
int rt_has_guest_code(BenefactorImageIdentity image, uint32_t address);
int rt_is_resume_point(const M68KCtx *ctx, BenefactorImageIdentity image, uint32_t address);

/* The snapshot is an opaque serialization of amigaport::CpuState. It keeps
 * save files from introducing a second CPU model in this title. */
size_t rt_state_blob_size(void);
int rt_state_blob_save(void *destination, size_t capacity);
int rt_state_blob_load(const void *source, size_t size);

#define BENEFACTOR_GUEST_MEMORY_SIZE (8u * 1024u * 1024u)
#define RT_MEM_SIZE BENEFACTOR_GUEST_MEMORY_SIZE
extern uint8_t *g_mem;

void rt_chip_rwatch_add(uint32_t address, uint32_t length);
void rt_chip_rwatch_clear(void);
void rt_chip_watch_add(uint32_t address, uint32_t length);
void rt_chip_watch_clear(void);
uint32_t rt_get_last_insn(void);
/* Live architectural PC of the interpreter, valid while the guest is running.
 * Signal-handler safe: a relaxed read of the canonical CPU state. */
uint32_t rt_get_pc(void);

/* The guest's architectural registers, for a debugger to show. An address
 * register holding something unexpected is a whole class of fault — a wait
 * loop reading the wrong hardware address, a base pointer clobbered by an
 * interrupt — and it is invisible in a PC-and-cycles view. Fills `data` with
 * D0-D7 and `address` with A0-A7; either may be null. */
/* Stop the guest when execution reaches `address`, before the instruction
 * there runs. The exit is reported as reason=breakpoint. rt_set_breakpoint
 * returns 0 if the address is already set or the set is full; rt_breakpoints
 * copies out the current addresses and returns how many it wrote. */
int rt_set_breakpoint(uint32_t address);
int rt_clear_breakpoint(uint32_t address);
void rt_clear_breakpoints(void);
int rt_breakpoints(uint32_t *addresses, int capacity);
int rt_breakpoint_capacity(void);

void rt_cpu_registers(uint32_t *data, uint32_t *address, uint32_t *program_counter,
                      uint16_t *status);
uint64_t rt_get_executed_instructions(void);
/* Monotonic 68000 cycles the guest has consumed. This is the port's clock for
 * anything that was timed by the beam on hardware. */
uint64_t rt_get_guest_cycles(void);
void rt_add_guest_cycles(uint64_t cycles);
uint64_t rt_get_cycle_base(void);
uint64_t rt_get_cycles_elapsed(void);
/* Recently retired guest instructions, oldest first: PCs alone through
 * rt_insn_ring_snapshot, or PCs with their instruction words through
 * rt_insn_ring_entries (pass a null `opcodes` to skip them). */
int rt_insn_ring_entries(uint32_t *program_counters, uint16_t *opcodes, int capacity);
/* The watchdog reads this from a fatal signal handler; the adapter must expose
 * the active address without allocation, locks, or other non-signal-safe work. */
uint32_t rt_get_active_call_address(void);
int rt_insn_ring_snapshot(uint32_t *dest, int capacity);
int rt_recent_snapshot(uint32_t *dest, int capacity);

#ifdef __cplusplus
}
#endif
