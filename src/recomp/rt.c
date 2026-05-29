/*
 * recomp/rt.c  –  68k recompiler runtime implementation
 *
 * Memory layout:
 *   $000000 – $07FFFF   chip RAM     (512 KB)
 *   $080000 – $7FFFFF   fast RAM     (7.5 MB, for data buffers)
 *   $BFD000 – $BFDFFF   CIA-B        (routed to hw.c)
 *   $BFE000 – $BFEFFF   CIA-A        (routed to hw.c)
 *   $DFF000 – $DFFFFF   custom chips (routed to hw.c)
 */

#include "rt.h"
#include "hw.h"

#ifdef HARNESS_BUILD
#include "harness/trace.h"
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef HARNESS_BUILD
# define RT_VERBOSE 0
#else
# define RT_VERBOSE 1
#endif
#define RT_LOG(...) GLOBAL_LOG_IF(RT_VERBOSE, __VA_ARGS__)

static uint32_t _rt_current_pc(void);
static uint32_t rt_last_insn_addr = 0;
uint32_t rt_get_last_insn(void) { return rt_last_insn_addr; }
volatile uint32_t g_rt_last_call = 0;   /* watchdog: most recently entered recompiled fn */
static void _gp4d(uint32_t addr, uint32_t v, int sz);

/* Ring of recent rt_call targets, for post-mortem when the game flow unwinds. */
#define RT_RECENT 48
static uint32_t rt_recent[RT_RECENT];
static int rt_recent_pos = 0;
void rt_dump_recent(FILE *f)
{
    fprintf(f, "recent rt_call targets (oldest..newest):\n");
    for (int i = 0; i < RT_RECENT; i++) {
        uint32_t a = rt_recent[(rt_recent_pos + i) % RT_RECENT];
        if (a) fprintf(f, " $%06X", a);
    }
    fprintf(f, "\n");
}

/* Signal-handler-safe snapshot of the recent rt_call ring (oldest..newest).
 * Returns the number of entries written. */
int rt_recent_snapshot(uint32_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < RT_RECENT && n < max; i++) {
        uint32_t a = rt_recent[(rt_recent_pos + i) % RT_RECENT];
        if (a) out[n++] = a;
    }
    return n;
}

static int _rt_is_state_watch_addr(uint32_t addr)
{
    return (addr >= 0x0042FCu && addr < 0x0042FEu) ||
           (addr >= 0x0069DCu && addr < 0x0069E0u) ||
           (addr >= 0x0069F0u && addr < 0x006AEAu) ||
           (addr >= 0x07FFA2u && addr < 0x080000u);
}

/* ── Global memory ─────────────────────────────────────────────────────────── */

uint8_t *g_mem = NULL;

/* ── Init / fini ───────────────────────────────────────────────────────────── */

int rt_init(const char *binary_path, uint32_t load_addr, uint32_t stack_top)
{
    g_mem = (uint8_t *)calloc(1, RT_MEM_SIZE);
    if (!g_mem) {
        RT_LOG("Out of memory\n");
        return -1;
    }

    if (binary_path) {
        FILE *fp = fopen(binary_path, "rb");
        if (!fp) {
            RT_LOG("Cannot open %s\n", binary_path);
            return -1;
        }
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        rewind(fp);
        if (load_addr + (uint32_t)sz > RT_MEM_SIZE) {
            RT_LOG("Binary too large (0x%lx bytes at 0x%06x)\n",
                    sz, load_addr);
            fclose(fp);
            return -1;
        }
        size_t n = fread(g_mem + load_addr, 1, (size_t)sz, fp);
        fclose(fp);
        RT_LOG("Loaded %zu bytes at $%06X\n", n, load_addr);
    }

    (void)stack_top;
    return 0;
}

void rt_fini(void)
{
    free(g_mem);
    g_mem = NULL;
}

/* ── Memory routing helpers ────────────────────────────────────────────────── */

static inline int is_hw(uint32_t a)
{
    a &= 0xFFFFFF;
    return (a >= 0xBFD000 && a <= 0xBFDFFF) ||   /* CIA-B */
           (a >= 0xBFE000 && a <= 0xBFEFFF) ||   /* CIA-A */
           (a >= 0xDFF000 && a <= 0xDFFFFF);      /* Custom */
}

/* ── Read ──────────────────────────────────────────────────────────────────── */

/* Spin detector (env GP_SPIN_DETECT): if the SAME address is read a huge number
 * of times in a row (a poll loop that never makes progress), dump the polled
 * address + the spinning function (top of the recompiler call stack) and abort.
 * Catches both hardware-register and in-memory wait loops without a trace build. */
static inline void _rt_spin_check(M68KCtx *ctx, uint32_t addr)
{
    static int en = -1;
    if (en < 0) en = getenv("GP_SPIN_DETECT") ? 1 : 0;
    if (!en) return;
    static uint32_t last = 0xFFFFFFFFu;
    static uint64_t cnt = 0;
    if (addr == last) {
        if (++cnt == 4000000ull) {
            fprintf(stderr, "[spin] address $%06X polled %llu× with no progress; fn=$%06X\n"
                    "  D0=%08X D1=%08X D5=%08X D6=%08X D7=%08X A0=%08X A1=%08X A2=%08X A5=%08X\n",
                    addr, (unsigned long long)cnt, _rt_current_pc(),
                    ctx->D[0], ctx->D[1], ctx->D[5], ctx->D[6], ctx->D[7],
                    ctx->A[0], ctx->A[1], ctx->A[2], ctx->A[5], ctx->A[7]);
            fflush(stderr);
            abort();
        }
    } else { last = addr; cnt = 0; }
}

uint8_t rt_read8(M68KCtx *ctx, uint32_t addr)
{
    (void)ctx;
    addr &= 0xFFFFFF;
    _rt_spin_check(ctx, addr);
    if (addr < RT_MEM_SIZE && !is_hw(addr))
        return g_mem[addr];
    return hw_read8(addr);
}

uint16_t rt_read16(M68KCtx *ctx, uint32_t addr)
{
    (void)ctx;
    addr &= 0xFFFFFF;
    _rt_spin_check(ctx, addr);
    if (addr + 1 < RT_MEM_SIZE && !is_hw(addr))
        return (uint16_t)((g_mem[addr] << 8) | g_mem[addr+1]);
    return hw_read16(addr);
}

uint32_t rt_read32(M68KCtx *ctx, uint32_t addr)
{
    (void)ctx;
    addr &= 0xFFFFFF;
    if (addr + 3 < RT_MEM_SIZE && !is_hw(addr))
        return ((uint32_t)g_mem[addr  ] << 24) | ((uint32_t)g_mem[addr+1] << 16) |
               ((uint32_t)g_mem[addr+2] <<  8) |  (uint32_t)g_mem[addr+3];
    return hw_read32(addr);
}

/* ── Write ─────────────────────────────────────────────────────────────────── */

void rt_write8(M68KCtx *ctx, uint32_t addr, uint8_t v)
{
    (void)ctx;
    addr &= 0xFFFFFF;
    _gp4d(addr, v, 1);
    { static int w = -1; if (w < 0) w = getenv("GP_RDY_WATCH") ? 1 : 0;
      if (w && (addr == 0x57FEBEu || addr == 0x57FEBFu)) {
          static int n = 0; if (n++ < 80)
              printf("[rdy-write8] $%06X (hi=%s) = %02X (pc=$%06X)%s\n", addr,
                     addr==0x57FEBEu?"yes":"no", v, _rt_current_pc(),
                     (addr==0x57FEBEu && (v&0x80))?"  <-- BIT15 READY":""); } }
    if (addr < RT_MEM_SIZE && !is_hw(addr)) {
#ifdef HARNESS_BUILD
        if (_rt_is_state_watch_addr(addr))
            trace_write_pc(addr, g_mem[addr], (uint8_t)v, 1, _rt_current_pc(), 0);
#endif
        g_mem[addr] = v;
        return;
    }
    hw_write8(addr, v);
}

/* Helper: current approximate M68K PC from call stack */
static uint32_t _rt_current_pc(void)
{
    if (rt_callstack_sp > 0)
        return rt_callstack[rt_callstack_sp - 1];
    return 0;
}

/* Trace chip-RAM writes that land in "interesting" ranges.
 * Defined as: copper list ($86CC-$88CC), bitplane tables ($7C80-$7CA0),
 * and sprite staging area / buf_A ($070930-$0718D0). */
static int _rt_addr_interesting(uint32_t addr)
{
    return (addr >= 0x86CCu && addr < 0x88CCu) ||
           (addr >= 0x7BC8u && addr < 0x7E00u) ||
           (addr >= 0x070930u && addr < 0x0718D1u) ||
           (addr >= 0x0006A0u && addr < 0x0006D0u) ||
           _rt_is_state_watch_addr(addr);
}

/* GP_4D=1: log every write to the level-table region $4d040-$4d080 with the
 * writing instruction + enclosing recompiled fn — to find what should decrunch
 * the $4d064 table (and what instead leaves it $FFFF). */
static void _gp4d(uint32_t addr, uint32_t v, int sz)
{
    static int w = -1; if (w < 0) w = getenv("GP_4D") ? 1 : 0;
    extern volatile uint32_t g_rt_last_call;
    if (w && addr >= 0x2Eu && addr < 0x44u) {
        static int n = 0; if (n++ < 8000)
            printf("[4d] $%06X = %0*X (sz%d) insn=$%06X fn=$%06X\n",
                   addr, sz*2, v, sz, rt_last_insn_addr, _rt_current_pc());
    }
}

void rt_write16(M68KCtx *ctx, uint32_t addr, uint16_t v)
{
    addr &= 0xFFFFFF;
    _gp4d(addr, v, 2);
    { static int watch = -1; if (watch < 0) watch = getenv("GP_PAL_WATCH") ? 1 : 0;
      if (watch && ((addr >= 0x34F4u && addr < 0x3700u) || (addr >= 0x57F100u && addr < 0x57F300u))) {
          static int n = 0; if (n++ < 60)
              printf("[pal-write] $%06X = %04X (pc=$%06X)\n", addr, v, _rt_current_pc()); } }
    { static int w = -1; if (w < 0) w = getenv("GP_RDY_WATCH") ? 1 : 0;
      if (w && addr == 0x57FEBEu) {   /* $10AC(a5) gameplay status word; bit15=level-ready */
          static int n = 0; if (n++ < 80)
              printf("[rdy-write] $10AC(a5)=%04X (pc=$%06X)%s\n", v, _rt_current_pc(), (v&0x8000)?"  <-- BIT15 READY":""); } }
    if (addr + 1 < RT_MEM_SIZE && !is_hw(addr)) {
#ifdef HARNESS_BUILD
        if (_rt_addr_interesting(addr)) {
            uint16_t old_v = (g_mem[addr] << 8) | g_mem[addr + 1];
            trace_write_pc(addr, old_v, v, 2, _rt_current_pc(), 0);
        }
#endif
        g_mem[addr]   = (uint8_t)(v >> 8);
        g_mem[addr+1] = (uint8_t)(v);
        return;
    }
    hw_write16(addr, v);
}

void rt_write32(M68KCtx *ctx, uint32_t addr, uint32_t v)
{
    addr &= 0xFFFFFF;
    _gp4d(addr, v, 4);
    if (addr + 3 < RT_MEM_SIZE && !is_hw(addr)) {
#ifdef HARNESS_BUILD
        if (_rt_addr_interesting(addr) || _rt_addr_interesting(addr + 2)) {
            uint32_t old_v = ((uint32_t)g_mem[addr] << 24) |
                             ((uint32_t)g_mem[addr + 1] << 16) |
                             ((uint32_t)g_mem[addr + 2] << 8) |
                             g_mem[addr + 3];
            trace_write_pc(addr, old_v, v, 4, _rt_current_pc(), 0);
        }
#endif
        g_mem[addr]   = (uint8_t)(v >> 24);
        g_mem[addr+1] = (uint8_t)(v >> 16);
        g_mem[addr+2] = (uint8_t)(v >>  8);
        g_mem[addr+3] = (uint8_t)(v);
        return;
    }
    hw_write32(addr, v);
}

/* ── Native engine overrides ───────────────────────────────────────────────── */

#define MAX_OVERRIDES 64
static struct {
    uint32_t addr;
    NativeFn fn;
    int      gp;     /* 1 = gameplay-bank override (fires only when g_gameplay_active) */
} g_overrides[MAX_OVERRIDES];
static int g_override_count = 0;

static void rt_register_override_x(uint32_t addr, NativeFn fn, int gp)
{
    if (addr == 0) {
        g_override_count = 0;
        return;
    }
    if (g_override_count >= MAX_OVERRIDES) {
        RT_LOG("override table full\n");
        return;
    }
    g_overrides[g_override_count].addr = addr;
    g_overrides[g_override_count].fn   = fn;
    g_overrides[g_override_count].gp   = gp;
    g_override_count++;
}

void rt_register_override(uint32_t addr, NativeFn fn)    { rt_register_override_x(addr, fn, 0); }
void rt_register_override_gp(uint32_t addr, NativeFn fn) { rt_register_override_x(addr, fn, 1); }

extern int g_overlay_active;
extern int g_gameplay_active;

static NativeFn override_lookup(uint32_t addr)
{
    /* In gameplay, only gameplay overrides (gp=1) fire — the title overrides at
     * the same addresses hold different loaded code with their own gpl-bank fns.
     * Outside gameplay, only the title/intro overrides (gp=0); and intro
     * overrides are skipped once the title-state overlay is live above $3294
     * (those addresses hold different code), except the shared resident region. */
    int want_gp = g_gameplay_active ? 1 : 0;
    if (!want_gp && g_overlay_active && addr >= 0x3294u)
        return NULL;
    for (int i = 0; i < g_override_count; i++) {
        if (g_overrides[i].addr == addr && g_overrides[i].gp == want_gp)
            return g_overrides[i].fn;
    }
    return NULL;
}

/* ── Dispatch table (populated from game.h) ────────────────────────────────── */

/* Defined in generated/game.c */
extern const GameFnEntry g_fn_table[];
extern const int         g_fn_count;

/* Gameplay overlay bank (loaded from disk over the intro at the same
 * addresses).  When the runtime loader brings it in it sets g_overlay_active;
 * dispatch then prefers the gameplay table.  Weak so binaries that don't link
 * the gameplay bank still build/link. */
extern const GameFnEntry g_fn_table_gp[] __attribute__((weak));
extern const int         g_fn_gp_count   __attribute__((weak));
/* Gameplay bank: the disk overlay the $150 loader brings in (code at $3330 +
 * $577000). Selected when g_gameplay_active; takes priority over the title/gp
 * bank since it overlays the same low addresses with different code. */
extern const GameFnEntry g_fn_table_gpl[] __attribute__((weak));
extern const int         g_fn_gpl_count   __attribute__((weak));
int g_overlay_active = 0;
int g_gameplay_active = 0;

/* Resident region $3000-$3293 is byte-identical in both banks; everything at
 * or above this is overlaid with different gameplay code. */
#define OVERLAY_RESIDENT_END 0x3294u

static GameFn table_search(const GameFnEntry *tbl, int n, uint32_t addr)
{
    int lo = 0, hi = n - 1;            /* table is sorted by address */
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (tbl[mid].addr == addr) return tbl[mid].fn;
        if (tbl[mid].addr < addr)  lo = mid + 1;
        else                       hi = mid - 1;
    }
    return NULL;
}

/* Which bank owns a given address? Used by the timer-IRQ delivery to route an
 * installed interrupt vector to the correct bank (the cover-art title installs
 * level-3/6 handlers that live only in the title-state "gp" bank). */
int rt_intro_has_fn(uint32_t addr)
{
    return table_search(g_fn_table, g_fn_count, addr) ? 1 : 0;
}
int rt_gp_has_fn(uint32_t addr)
{
    return (&g_fn_gp_count && table_search(g_fn_table_gp, g_fn_gp_count, addr)) ? 1 : 0;
}
int rt_gpl_has_fn(uint32_t addr)
{
    return (&g_fn_gpl_count && table_search(g_fn_table_gpl, g_fn_gpl_count, addr)) ? 1 : 0;
}

static GameFn dispatch_lookup(uint32_t addr)
{
    if (g_gameplay_active && &g_fn_gpl_count) {
        GameFn fn = table_search(g_fn_table_gpl, g_fn_gpl_count, addr);
        if (fn) return fn;
        if (addr >= OVERLAY_RESIDENT_END) return NULL;  /* undiscovered gameplay fn */
    }
    if (g_overlay_active && &g_fn_gp_count) {
        GameFn fn = table_search(g_fn_table_gp, g_fn_gp_count, addr);
        if (fn) return fn;
        /* In the overlaid range the intro bank holds DIFFERENT code, so never
         * fall back to it — return NULL so an undiscovered handler is logged
         * (drives runtime discovery) instead of running stale intro code. */
        if (addr >= OVERLAY_RESIDENT_END) return NULL;
    }
    return table_search(g_fn_table, g_fn_count, addr);
}

/* Fail-fast on an unregistered call target. Opt-in (RT_FAIL_ON_MISS=1) because
 * the gameplay-overlay bank intentionally returns NULL for not-yet-discovered
 * handlers to drive runtime discovery; enable it to hard-stop at the first miss
 * (with the caller address) when hunting missing functions. */
static void rt_miss(uint32_t addr, M68KCtx *ctx)
{
    /* Collect every DISTINCT missing call target to stderr (one run → full list
     * of functions to seed). A skipped call drops the function's work entirely,
     * corrupting state — so for the gameplay bank these must all be resolved. */
    {
        static uint32_t seen[512]; static int nseen = 0;
        int known = 0;
        for (int i = 0; i < nseen; i++) if (seen[i] == addr) { known = 1; break; }
        if (!known && nseen < 512) {
            seen[nseen++] = addr;
            fprintf(stderr, "[rt-miss] $%06X (from $%06X)\n", addr, rt_last_insn_addr);
            fflush(stderr);
        }
    }
    /* A skipped call drops the function's work → corrupt/uncertain state, which
     * makes debugging impossible (can't tell a real bug from a skipped handler).
     * So in the GAMEPLAY bank a miss is FATAL by default — it tells us exactly
     * which indirect-dispatch handler still needs seeding. RT_ALLOW_MISS=1
     * downgrades to log+skip ONLY for discovery runs (tools_seed_converge.sh,
     * which collects every miss in one pass). RT_FAIL_ON_MISS=1 forces abort
     * even outside gameplay. */
    extern int g_gameplay_active;
    static int allow = -1;
    if (allow < 0) allow = getenv("RT_ALLOW_MISS") ? 1 : 0;
    static int fail = -1;
    if (fail < 0) fail = getenv("RT_FAIL_ON_MISS") ? 1 : 0;
    if ((g_gameplay_active && !allow) || fail) {
        fprintf(stderr, "\n*** rt_call: NO FUNCTION at $%06X (from $%06X)"
                " — gameplay/RT_FAIL_ON_MISS, aborting ***\n", addr, rt_last_insn_addr);
        fprintf(stderr, "    D0=%08X D1=%08X D2=%08X A0=%08X A1=%08X A2=%08X\n",
                ctx->D[0], ctx->D[1], ctx->D[2], ctx->A[0], ctx->A[1], ctx->A[2]);
        fflush(stderr);
        abort();
    }
}

/* is_call=1 for bsr/jsr (push a return-address slot, like real M68K), 0 for
 * bra/jmp tail transfers (no push). The recompiler models calls as C calls and
 * normally never touches the Amiga SP for the return address — but gameplay
 * code reads its stack ARGUMENTS at a7-relative offsets that ASSUME the 4-byte
 * return address is present (e.g. $59DCBE: movem d2-d3/a5,-(a7); movea $10(a7),a5
 * reads the caller's arg above the RA). Without the RA slot, a7 sits 4 bytes
 * too high per call level → arg reads land on garbage → corruption. So in the
 * gameplay bank we reserve the RA slot around the call. Gated to gameplay to
 * preserve the already-correct title/intro behaviour. */
static void rt_call_impl(M68KCtx *ctx, uint32_t addr, int is_call)
{
    g_rt_last_call = addr;        /* watchdog diagnostic: function most recently entered */
    rt_recent[rt_recent_pos] = addr;
    rt_recent_pos = (rt_recent_pos + 1) % RT_RECENT;
    rt_push_call(addr);
    extern int g_gameplay_active;
    int ra = (is_call && g_gameplay_active);
    if (ra) { ctx->A[7] -= 4u; rt_write32(ctx, ctx->A[7], rt_last_insn_addr); }
    if (rt_trace_calls && (addr == 0x00593Au || addr == 0x0055A0u || addr == 0x00377Au || addr == 0x003818u)) {
        extern int hw_get_frame_num(void);
        GLOBAL_LOG("CALL $%06X from $%06X frame=%d A0=%08X A1=%08X A2=%08X A3=%08X A4=%08X A5=%08X D0=%08X D1=%08X D2=%08X D7=%08X\n",
                   addr, rt_last_insn_addr, hw_get_frame_num(),
                   ctx->A[0], ctx->A[1], ctx->A[2], ctx->A[3],
                   ctx->A[4], ctx->A[5], ctx->D[0], ctx->D[1], ctx->D[2], ctx->D[7]);
        if (addr == 0x003818u || addr == 0x00377Au) {
            GLOBAL_LOG("  STACK: ");
            for (int _si = 0; _si < rt_callstack_sp && _si < 8; _si++)
                GLOBAL_LOG("$%06X ", rt_callstack[_si]);
            GLOBAL_LOG("\n");
        }
    }
    /* override_lookup is bank-aware: in gameplay only gp-overrides fire, so the
     * title overrides ($405C/$41A4/…) don't hijack the gameplay's own code. */
    NativeFn native = override_lookup(addr);
    if (native) {
        native(ctx);
        if (ra) ctx->A[7] += 4u;
        rt_pop_call();
        return;
    }
    GameFn fn = dispatch_lookup(addr);
    if (fn) {
        fn(ctx);
    } else {
        if (getenv("RT_SKIPLOG"))
            GLOBAL_LOG("[rt-skip] no function at $%06X (from $%06X) D0=%08X D1=%08X D2=%08X A0=%08X A1=%08X\n",
                       addr, rt_last_insn_addr, ctx->D[0], ctx->D[1], ctx->D[2], ctx->A[0], ctx->A[1]);
        RT_LOG("rt_call: no function at $%06X – skipping\n", addr);
        rt_miss(addr, ctx);
    }
    if (ra) ctx->A[7] += 4u;
    rt_pop_call();
}

void rt_call(M68KCtx *ctx, uint32_t addr) { rt_call_impl(ctx, addr, 1); }

void rt_jump(M68KCtx *ctx, uint32_t addr) { rt_call_impl(ctx, addr, 0); }

void rt_call_generated(M68KCtx *ctx, uint32_t addr)
{
    rt_push_call(addr);
    GameFn fn = dispatch_lookup(addr);
    if (fn) {
        fn(ctx);
    } else {
        RT_LOG("rt_call_generated: no fn at $%06X – skipping\n", addr);
        rt_miss(addr, ctx);
    }
    rt_pop_call();
}

/* ── TRAP / ILLEGAL ────────────────────────────────────────────────────────── */

void rt_trap(M68KCtx *ctx, uint32_t num)
{
    (void)ctx;
    RT_LOG("TRAP #%u\n", num);
}

void rt_illegal(M68KCtx *ctx, uint32_t at)
{
    (void)ctx;
    RT_LOG("ILLEGAL at $%06X – halting\n", at);
    hw_running = 0;
}

/* ── Trace / debugging implementation ──────────────────────────────────────── */

int rt_trace_calls = 0;
int rt_trace_insns = 0;
uint64_t rt_insn_count = 0;
uint64_t rt_watchdog = 0;
uint32_t rt_callstack[RT_CALLSTACK_DEPTH];
int rt_callstack_sp = 0;
static int rt_trace_insn_filter = 0;
static uint32_t rt_trace_insn_lo = 0;
static uint32_t rt_trace_insn_hi = 0xFFFFFFu;
static int rt_watch_a6 = 0;
static uint32_t rt_watch_a6_lo = 0;
static uint32_t rt_watch_a6_hi = 0;
static uint32_t rt_watch_a6_prev = 0;
static int rt_watch_a6_prev_valid = 0;

static void trace_init_env(void)
{
    static int done = 0;
    if (done) return;
    done = 1;
    rt_trace_calls = getenv("RT_CALLS") != NULL;
    rt_trace_insns = getenv("RT_INSNS") != NULL;
    {
        const char *insn_lo = getenv("RT_INSN_LO");
        const char *insn_hi = getenv("RT_INSN_HI");
        if (insn_lo || insn_hi) {
            rt_trace_insn_filter = 1;
            if (insn_lo) rt_trace_insn_lo = (uint32_t)strtoul(insn_lo, NULL, 0) & 0xFFFFFFu;
            if (insn_hi) rt_trace_insn_hi = (uint32_t)strtoul(insn_hi, NULL, 0) & 0xFFFFFFu;
        }
    }
    rt_watch_a6 = getenv("RT_WATCH_A6") != NULL;
    if (rt_watch_a6) {
        const char *lo = getenv("RT_WATCH_A6_LO");
        const char *hi = getenv("RT_WATCH_A6_HI");
        rt_watch_a6_lo = lo ? (uint32_t)strtoul(lo, NULL, 0) : 0x0055A0u;
        rt_watch_a6_hi = hi ? (uint32_t)strtoul(hi, NULL, 0) : 0x0058A0u;
    }
    const char *w = getenv("RT_WATCHDOG");
    if (w) rt_watchdog = strtoull(w, NULL, 10);
    if (rt_trace_calls || rt_trace_insns || rt_watchdog || rt_watch_a6)
        RT_LOG("trace: calls=%d insns=%d watch_a6=%d [%06X..%06X] watchdog=%llu\n",
                rt_trace_calls, rt_trace_insns, rt_watch_a6,
                rt_watch_a6_lo, rt_watch_a6_hi,
                (unsigned long long)rt_watchdog);
}

/* Ring of the most recent instruction PCs (cheap: one store per insn), so the
 * watchdog can show the exact spin loop + how it got there on a freeze. Only
 * populated in the trace build (RT_TRACE_INSNS), where rt_trace_insn fires. */
#define RT_INSN_RING 1024
static uint32_t rt_insn_ring[RT_INSN_RING];
static int rt_insn_ring_pos = 0;
int rt_insn_ring_snapshot(uint32_t *out, int max)
{
    int n = (max < RT_INSN_RING) ? max : RT_INSN_RING;
    for (int i = 0; i < n; i++)
        out[i] = rt_insn_ring[(rt_insn_ring_pos - n + i + RT_INSN_RING) % RT_INSN_RING];
    return n;
}

void rt_trace_insn(uint32_t addr, const char *mnemonic, M68KCtx *ctx)
{
    trace_init_env();
    rt_last_insn_addr = addr;
    rt_insn_ring[rt_insn_ring_pos] = addr;
    rt_insn_ring_pos = (rt_insn_ring_pos + 1) % RT_INSN_RING;
    rt_insn_count++;

    /* Benefactor harness: env-gated per-instruction trace to a file, in the
     * SAME format as the PUAE-side benefactor_insn_trace (newcpu.c). Diffing
     * logs/pc_insn_trace.txt vs logs/puae_insn_trace.txt finds the first
     * instruction where recompiled vs emulated execution diverges.
     * Requires building with -DRT_TRACE_INSNS so these calls are emitted. */
    {
        static int bt_en = -1;
        static FILE *bt_f = NULL;
        static uint32_t bt_lo = 0x5500, bt_hi = 0x6100;
        static int bt_flo = 0, bt_fhi = 1 << 30;
        static long bt_lines = 0;
        if (bt_en < 0) {
            const char *e = getenv("BENEFACTOR_M68K_TRACE");
            bt_en = (e && e[0] == '1') ? 1 : 0;
            if (bt_en) {
                const char *rg = getenv("BENEFACTOR_M68K_RANGE");
                if (rg) { unsigned a, b; if (sscanf(rg, "%x-%x", &a, &b) == 2) { bt_lo = a; bt_hi = b; } }
                const char *fr = getenv("BENEFACTOR_M68K_FRAMES");
                if (fr) { unsigned a, b; if (sscanf(fr, "%u-%u", &a, &b) == 2) { bt_flo = (int)a; bt_fhi = (int)b; } }
                bt_f = fopen("logs/pc_insn_trace.txt", "w");
            }
        }
        int _itf = 0;
#ifdef HARNESS_BUILD
        extern int g_harness_compared_frame;
        _itf = g_harness_compared_frame;
#endif
        if (bt_en && bt_f && bt_lines < 200000 && addr >= bt_lo && addr < bt_hi
            && _itf >= bt_flo && _itf <= bt_fhi) {
            fprintf(bt_f, "f=%d %06X d0=%08X d1=%08X d5=%08X d6=%08X d7=%08X a0=%08X a1=%08X a2=%08X a5=%08X a7=%08X\n",
                    _itf, addr, ctx->D[0], ctx->D[1], ctx->D[5], ctx->D[6], ctx->D[7],
                    ctx->A[0], ctx->A[1], ctx->A[2], ctx->A[5], ctx->A[7]);
            bt_lines++;
            if ((bt_lines & 0x3FF) == 0) fflush(bt_f);
        }
    }

    if (rt_watchdog && rt_insn_count >= rt_watchdog) {
        RT_LOG("\nWATCHDOG triggered at insn #%llu addr=$%06X\n",
                (unsigned long long)rt_insn_count, addr);
        rt_dump_state(ctx);
        hw_running = 0;
        return;
    }
    if (rt_trace_insns) {
        if (!rt_trace_insn_filter || (addr >= rt_trace_insn_lo && addr <= rt_trace_insn_hi)) {
            GLOBAL_LOG("[%10llu] $%06X %-12s D0=%08X D1=%08X D2=%08X D3=%08X A0=%08X A1=%08X A2=%08X A3=%08X N=%d Z=%d V=%d C=%d\n",
                    (unsigned long long)rt_insn_count, addr, mnemonic,
                    ctx->D[0], ctx->D[1], ctx->D[2], ctx->D[3],
                    ctx->A[0], ctx->A[1], ctx->A[2], ctx->A[3],
                    ctx->N, ctx->Z, ctx->V, ctx->C);
        }
    }
    if (rt_watch_a6 && addr >= rt_watch_a6_lo && addr <= rt_watch_a6_hi) {
        if (!rt_watch_a6_prev_valid) {
            rt_watch_a6_prev = ctx->A[6];
            rt_watch_a6_prev_valid = 1;
            GLOBAL_LOG("[A6_WATCH] enter @$%06X a6=$%08X a2=$%08X a4=$%08X d7=$%08X\n",
                addr, ctx->A[6], ctx->A[2], ctx->A[4], ctx->D[7]);
        } else if (ctx->A[6] != rt_watch_a6_prev) {
            GLOBAL_LOG("[A6_WATCH] @$%06X a6 %08X -> %08X (a2=%08X a4=%08X d7=%08X)\n",
                addr, rt_watch_a6_prev, ctx->A[6], ctx->A[2], ctx->A[4], ctx->D[7]);
            rt_watch_a6_prev = ctx->A[6];
        }
    } else if (rt_watch_a6_prev_valid) {
        rt_watch_a6_prev_valid = 0;
    }
}

void rt_dump_state(M68KCtx *ctx)
{
    printf("=== CPU STATE ===\n");
    for (int i = 0; i < 8; i++)
        printf("  D%d = %08X   A%d = %08X\n", i, ctx->D[i], i, ctx->A[i]);
    printf("  N=%d Z=%d V=%d C=%d X=%d S=%d I=%d\n",
            ctx->N, ctx->Z, ctx->V, ctx->C, ctx->X, ctx->S, ctx->I);
    printf("=== CALL STACK (%d frames) ===\n", rt_callstack_sp);
    for (int i = 0; i < rt_callstack_sp && i < RT_CALLSTACK_DEPTH; i++)
        printf("  #%d $%06X\n", i, rt_callstack[i]);
}

void rt_push_call(uint32_t addr)
{
    trace_init_env();
    if (rt_callstack_sp < RT_CALLSTACK_DEPTH)
        rt_callstack[rt_callstack_sp++] = addr;
    if (rt_trace_calls) {
        for (int i = 0; i < rt_callstack_sp - 1; i++)
            GLOBAL_LOG("  ");
        GLOBAL_LOG("-> $%06X\n", addr);
    }
}

void rt_pop_call(void)
{
    if (rt_callstack_sp > 0)
        rt_callstack_sp--;
}
