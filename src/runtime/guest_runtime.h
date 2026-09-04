/* Benefactor's title-side view of shared/amigaport.
 *
 * This file declares a borrowed adapter view. It does not own or duplicate
 * 68000 architectural state: D and A point into the canonical amigaport CPU
 * context and memory points at the active image mapping. The implementation
 * belongs to the missing Benefactor/amigaport adapter.
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
    uint8_t *memory;
    size_t memory_size;
    BenefactorImageIdentity image;
} M68KCtx;

typedef void (*NativeFn)(M68KCtx *ctx);

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
void rt_call(M68KCtx *ctx, BenefactorImageIdentity image, uint32_t address);
void rt_jump(M68KCtx *ctx, BenefactorImageIdentity image, uint32_t address);
void rt_call_original(M68KCtx *ctx, BenefactorImageIdentity image, uint32_t address);

/* Transitional host seams awaiting the adapter implementation. They are
 * declarations only; no gameplay target is built until shared/amigaport owns
 * their implementation. */
void rt_reset_callstack(void);
int rt_init(const char *binary_path, uint32_t load_addr, uint32_t stack_top);
void rt_fini(void);
void rt_resume(M68KCtx *ctx, BenefactorImageIdentity image, uint32_t address);
int rt_has_guest_code(BenefactorImageIdentity image, uint32_t address);
int rt_is_resume_point(const M68KCtx *ctx, BenefactorImageIdentity image, uint32_t address);

#define BENEFACTOR_GUEST_MEMORY_SIZE (8u * 1024u * 1024u)
#define RT_MEM_SIZE BENEFACTOR_GUEST_MEMORY_SIZE
extern uint8_t *g_mem;

void rt_chip_rwatch_add(uint32_t address, uint32_t length);
void rt_chip_rwatch_clear(void);
void rt_chip_watch_add(uint32_t address, uint32_t length);
void rt_chip_watch_clear(void);
uint32_t rt_get_last_insn(void);
/* The watchdog reads this from a fatal signal handler; the adapter must expose
 * the active address without allocation, locks, or other non-signal-safe work. */
uint32_t rt_get_active_call_address(void);
int rt_insn_ring_snapshot(uint32_t *dest, int capacity);
int rt_recent_snapshot(uint32_t *dest, int capacity);
