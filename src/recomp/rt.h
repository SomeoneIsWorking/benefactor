/*
 * recomp/rt.h  –  68k static recompiler runtime
 * ================================================
 * All generated game functions take a single M68KCtx* argument.
 * Memory accesses go through MRxx / MWxx which route hardware I/O.
 */
#pragma once
#include "common_log.h"
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── CPU register context ─────────────────────────────────────────────────── */

typedef struct M68KCtx {
    uint32_t D[8];   /* Data registers   D0-D7  */
    uint32_t A[8];   /* Address registers A0-A7 (A[7] = SP) */
    /* Condition codes */
    uint8_t  N, Z, V, C, X;
    /* Supervisor state (rarely needed in game code) */
    uint8_t  S;
    uint8_t  I;   /* interrupt mask level */
} M68KCtx;

/* ── Memory access – implemented in rt.c, routes hardware I/O ─────────────── */

uint8_t  rt_read8 (M68KCtx *ctx, uint32_t addr);
uint16_t rt_read16(M68KCtx *ctx, uint32_t addr);
uint32_t rt_read32(M68KCtx *ctx, uint32_t addr);
void     rt_write8 (M68KCtx *ctx, uint32_t addr, uint8_t  v);
void     rt_write16(M68KCtx *ctx, uint32_t addr, uint16_t v);
void     rt_write32(M68KCtx *ctx, uint32_t addr, uint32_t v);

/* Convenience macros used by generated code (ctx must be in scope) */
#define MR8(a)      rt_read8 (ctx, (uint32_t)(int32_t)(a))
#define MR16(a)     rt_read16(ctx, (uint32_t)(int32_t)(a))
#define MR32(a)     rt_read32(ctx, (uint32_t)(int32_t)(a))
#define MW8(a,v)    rt_write8 (ctx, (uint32_t)(int32_t)(a), (uint8_t)(v))
#define MW16(a,v)   rt_write16(ctx, (uint32_t)(int32_t)(a), (uint16_t)(v))
#define MW32(a,v)   rt_write32(ctx, (uint32_t)(int32_t)(a), (uint32_t)(v))

/* ── Stack helpers ─────────────────────────────────────────────────────────── */

static inline void rt_push32(M68KCtx *ctx, uint32_t v)
    { ctx->A[7] -= 4; rt_write32(ctx, ctx->A[7], v); }
static inline void rt_push16(M68KCtx *ctx, uint16_t v)
    { ctx->A[7] -= 2; rt_write16(ctx, ctx->A[7], v); }
static inline uint32_t rt_pop32(M68KCtx *ctx)
    { uint32_t v = rt_read32(ctx, ctx->A[7]); ctx->A[7] += 4; return v; }
static inline uint16_t rt_pop16(M68KCtx *ctx)
    { uint16_t v = rt_read16(ctx, ctx->A[7]); ctx->A[7] += 2; return v; }

/* ── Condition-code flag macros ────────────────────────────────────────────── */
/*
 * SET_NZ: update N and Z from result; leave V, C unchanged.
 * ADD_FLAGS / SUB_FLAGS: full NZVC update after arithmetic.
 */

/* Sign-extension helpers — eliminate multi-cast chains from generated code.
 * RT_SX16(x) : word  → long  (uint32_t, bit-preserving signed extend)
 * RT_SX8(x)  : byte  → long  (uint32_t, bit-preserving signed extend)
 * RT_SX8W(x) : byte  → word  (int16_t,  for EXT.W and flag comparisons) */
#define RT_SX16(x) ((uint32_t)(int32_t)(int16_t)(x))
#define RT_SX8(x)  ((uint32_t)(int32_t)(int8_t)(x))
#define RT_SX8W(x) ((int16_t)(int8_t)(x))

/* NOTE: evaluate the argument EXACTLY ONCE into a temp. The argument is often a
 * side-effecting statement-expression (e.g. `({ MR16(A[0]); A[0]+=2; _v; })` for
 * an (an)+ operand). Evaluating it twice would run the post-increment twice and
 * compute the flag from the wrong (second) read. */
#define RT_SET_NZ_8(r)  do { uint8_t  _nz = (uint8_t)(r);  \
                              ctx->N = (_nz >> 7) & 1; ctx->Z = (_nz == 0); } while(0)
#define RT_SET_NZ_16(r) do { uint16_t _nz = (uint16_t)(r); \
                              ctx->N = (_nz >> 15) & 1; ctx->Z = (_nz == 0); } while(0)
#define RT_SET_NZ_32(r) do { uint32_t _nz = (uint32_t)(r); \
                              ctx->N = (_nz >> 31) & 1; ctx->Z = (_nz == 0); } while(0)

/* ADD: dst + src → result (uint64_t).  Sets NZVCX. */
#define RT_ADD_FLAGS_8(dst,src,res) do { \
    uint8_t __rtf_d8=(uint8_t)(dst),__rtf_s8=(uint8_t)(src),__rtf_r8=(uint8_t)(res); \
    ctx->N = (__rtf_r8 >> 7) & 1; ctx->Z = (__rtf_r8 == 0); \
    ctx->C = ctx->X = ((uint64_t)(res) >> 8) & 1; \
    ctx->V = ((~(__rtf_d8 ^ __rtf_s8) & (__rtf_d8 ^ __rtf_r8)) >> 7) & 1; } while(0)
#define RT_ADD_FLAGS_16(dst,src,res) do { \
    uint16_t __rtf_d16=(uint16_t)(dst),__rtf_s16=(uint16_t)(src),__rtf_r16=(uint16_t)(res); \
    ctx->N = (__rtf_r16 >> 15) & 1; ctx->Z = (__rtf_r16 == 0); \
    ctx->C = ctx->X = ((uint64_t)(res) >> 16) & 1; \
    ctx->V = ((~(__rtf_d16 ^ __rtf_s16) & (__rtf_d16 ^ __rtf_r16)) >> 15) & 1; } while(0)
#define RT_ADD_FLAGS_32(dst,src,res) do { \
    uint32_t __rtf_d32=(uint32_t)(dst),__rtf_s32=(uint32_t)(src),__rtf_r32=(uint32_t)(res); \
    ctx->N = (__rtf_r32 >> 31) & 1; ctx->Z = (__rtf_r32 == 0); \
    ctx->C = ctx->X = ((uint64_t)(res) >> 32) & 1; \
    ctx->V = ((~(__rtf_d32 ^ __rtf_s32) & (__rtf_d32 ^ __rtf_r32)) >> 31) & 1; } while(0)

/* SUB: dst - src → result.  Sets NZVCX. */
#define RT_SUB_FLAGS_8(dst,src,res) do { \
    uint8_t __rtf_d8=(uint8_t)(dst),__rtf_s8=(uint8_t)(src),__rtf_r8=(uint8_t)(res); \
    ctx->N = (__rtf_r8 >> 7) & 1; ctx->Z = (__rtf_r8 == 0); \
    ctx->C = ctx->X = ((uint64_t)(res) >> 8) & 1; \
    ctx->V = ((__rtf_d8 ^ __rtf_s8) & (__rtf_d8 ^ __rtf_r8) & 0x80) ? 1 : 0; } while(0)
#define RT_SUB_FLAGS_16(dst,src,res) do { \
    uint16_t __rtf_d16=(uint16_t)(dst),__rtf_s16=(uint16_t)(src),__rtf_r16=(uint16_t)(res); \
    ctx->N = (__rtf_r16 >> 15) & 1; ctx->Z = (__rtf_r16 == 0); \
    ctx->C = ctx->X = ((uint64_t)(res) >> 16) & 1; \
    ctx->V = ((__rtf_d16 ^ __rtf_s16) & (__rtf_d16 ^ __rtf_r16) & 0x8000) ? 1 : 0; } while(0)
#define RT_SUB_FLAGS_32(dst,src,res) do { \
    uint32_t __rtf_d32=(uint32_t)(dst),__rtf_s32=(uint32_t)(src),__rtf_r32=(uint32_t)(res); \
    ctx->N = (__rtf_r32 >> 31) & 1; ctx->Z = (__rtf_r32 == 0); \
    ctx->C = ctx->X = ((uint64_t)(res) >> 32) & 1; \
    ctx->V = ((__rtf_d32 ^ __rtf_s32) & (__rtf_d32 ^ __rtf_r32) & 0x80000000u) ? 1 : 0; } while(0)

/* ── Condition code boolean expressions ────────────────────────────────────── */

#define RT_CC_T   (1)
#define RT_CC_F   (0)
#define RT_CC_HI  (!ctx->C && !ctx->Z)
#define RT_CC_LS  (ctx->C || ctx->Z)
#define RT_CC_CC  (!ctx->C)
#define RT_CC_CS  (ctx->C)
#define RT_CC_NE  (!ctx->Z)
#define RT_CC_EQ  (ctx->Z)
#define RT_CC_VC  (!ctx->V)
#define RT_CC_VS  (ctx->V)
#define RT_CC_PL  (!ctx->N)
#define RT_CC_MI  (ctx->N)
#define RT_CC_GE  (!(ctx->N ^ ctx->V))
#define RT_CC_LT  (ctx->N ^ ctx->V)
#define RT_CC_GT  (!(ctx->Z || (ctx->N ^ ctx->V)))
#define RT_CC_LE  (ctx->Z || (ctx->N ^ ctx->V))

/* ── Indirect call / jump dispatch ─────────────────────────────────────────── */

typedef void (*GameFn)(M68KCtx *ctx);

typedef struct {
    uint32_t addr;
    GameFn   fn;
} GameFnEntry;

/* Native engine overrides – installed at runtime in rt.c */
typedef void (*NativeFn)(M68KCtx *ctx);

/* Register a native C function to replace a recompiled 68k function.
 * Pass addr == 0 to clear all overrides. */
void rt_register_override(uint32_t addr, NativeFn fn);
void rt_register_override_gp(uint32_t addr, NativeFn fn);  /* fires only during gameplay */

/* These are implemented in rt.c using the dispatch table from game.h */
void rt_call(M68KCtx *ctx, uint32_t addr);
void rt_jump(M68KCtx *ctx, uint32_t addr);
/* Call generated function at addr, bypassing any registered native override.
 * Use from within a native override to invoke the original recompiled logic
 * without re-entering the override (i.e. the "super" call). */
void rt_call_generated(M68KCtx *ctx, uint32_t addr);
void rt_trap(M68KCtx *ctx, uint32_t num);
void rt_illegal(M68KCtx *ctx, uint32_t at);

/* ── Error / stub ──────────────────────────────────────────────────────────── */

#define RT_UNIMPL(msg) do { \
    GLOBAL_LOG("[recomp] UNIMPL: %s\n", msg); \
    hw_running = 0; \
    return; \
} while(0)

/* ── Global memory buffer (8 MB chip+fast RAM) ─────────────────────────────── */

#define RT_MEM_SIZE  (8u * 1024u * 1024u)

extern uint8_t *g_mem;   /* allocated in rt.c */

/* ── Trace / debugging ─────────────────────────────────────────────────────── */

extern int rt_trace_calls;      /* env RT_CALLS – log every rt_call */
extern int rt_trace_insns;      /* env RT_INSNS – log every instruction */
extern uint64_t rt_insn_count;  /* global instruction counter */
extern uint64_t rt_watchdog;    /* env RT_WATCHDOG – abort after N insns */

#define RT_CALLSTACK_DEPTH 64
extern uint32_t rt_callstack[RT_CALLSTACK_DEPTH];
extern int rt_callstack_sp;

void rt_trace_insn(uint32_t addr, const char *mnemonic, M68KCtx *ctx);
void rt_dump_state(M68KCtx *ctx);

/* The generated code wraps every instruction in this macro so it expands to a
 * single line per instruction. When RT_TRACE_INSNS is defined the macro emits
 * the rt_trace_insn call (used by the per-instruction PC ring buffer that the
 * watchdog dumps on freeze); otherwise it's a no-op. */
#ifdef RT_TRACE_INSNS
# define RT_TRACE_INSN(addr, mnem) rt_trace_insn((addr), (mnem), ctx)
#else
# define RT_TRACE_INSN(addr, mnem) ((void)0)
#endif
void rt_push_call(uint32_t addr);
void rt_pop_call(void);

/* ── Initialisation ────────────────────────────────────────────────────────── */

/* Load raw binary into g_mem at given Amiga address. */
int rt_init(const char *binary_path, uint32_t load_addr, uint32_t stack_top);
void rt_fini(void);
