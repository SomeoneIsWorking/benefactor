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

#include "engine/rt.h"
#include "engine/hw.h"
#include "common/game_state.h"   /* g_overlay_active, g_gameplay_active */

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

/* REPL-driven chip-RAM read/write watch. The harness's `pcwatch <hex>[-<hex>]`
 * adds a WRITE-watch range; `pcread <hex>[-<hex>]` adds a READ-watch range.
 * Matching accesses log M68K PC + addr + value. Use these to answer "does our
 * recompiled code ever touch $X, and what code does?" — much more reliable
 * than the address-via-static-scan approach since it catches register-
 * indirect addressing too. Read-tracing is the only reliable way to map data
 * accesses in a block of chip RAM whose entries don't show up as literal-abs
 * in the binary (e.g. the $150..$2A57 block-copy region). */
#define RT_CHIP_WATCH_MAX 16
static struct { uint32_t lo, hi; } s_chip_watch[RT_CHIP_WATCH_MAX];
static int s_chip_watch_n = 0;
static struct { uint32_t lo, hi; } s_chip_rwatch[RT_CHIP_WATCH_MAX];
static int s_chip_rwatch_n = 0;
void rt_chip_watch_add(uint32_t lo, uint32_t hi)
{
    if (s_chip_watch_n >= RT_CHIP_WATCH_MAX) {
        fprintf(stderr, "[pcwatch] list full (%d entries) — clear first\n",
                RT_CHIP_WATCH_MAX);
        return;
    }
    if (hi < lo) hi = lo;
    s_chip_watch[s_chip_watch_n].lo = lo;
    s_chip_watch[s_chip_watch_n].hi = hi;
    s_chip_watch_n++;
    fprintf(stderr, "[pcwatch] watching $%06X..$%06X (%d total)\n",
            lo, hi, s_chip_watch_n);
}
void rt_chip_watch_clear(void)
{
    s_chip_watch_n = 0;
    fprintf(stderr, "[pcwatch] cleared\n");
}
static inline int rt_chip_watch_hit(uint32_t a)
{
    for (int i = 0; i < s_chip_watch_n; i++)
        if (a >= s_chip_watch[i].lo && a <= s_chip_watch[i].hi) return 1;
    return 0;
}
void rt_chip_rwatch_add(uint32_t lo, uint32_t hi)
{
    if (s_chip_rwatch_n >= RT_CHIP_WATCH_MAX) {
        fprintf(stderr, "[pcread] list full (%d entries) — clear first\n",
                RT_CHIP_WATCH_MAX);
        return;
    }
    if (hi < lo) hi = lo;
    s_chip_rwatch[s_chip_rwatch_n].lo = lo;
    s_chip_rwatch[s_chip_rwatch_n].hi = hi;
    s_chip_rwatch_n++;
    fprintf(stderr, "[pcread] reading $%06X..$%06X (%d total)\n",
            lo, hi, s_chip_rwatch_n);
}
void rt_chip_rwatch_clear(void)
{
    s_chip_rwatch_n = 0;
    fprintf(stderr, "[pcread] cleared\n");
}
static inline int rt_chip_rwatch_hit(uint32_t a)
{
    for (int i = 0; i < s_chip_rwatch_n; i++)
        if (a >= s_chip_rwatch[i].lo && a <= s_chip_rwatch[i].hi) return 1;
    return 0;
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
    { static int w = -1; if (w < 0) w = getenv("MENUTEXT_WATCH") ? 1 : 0;
      if (w && addr >= 0x004A78u && addr < 0x004AC0u) {
          static int n = 0; if (n++ < 16)
              printf("[menutext-read8] $%06X pc=$%06X last_call=$%06X\n",
                     addr, _rt_current_pc(), (unsigned)g_rt_last_call); } }
    if (s_chip_rwatch_n && rt_chip_rwatch_hit(addr)) {
        uint8_t v = (addr < RT_MEM_SIZE && !is_hw(addr)) ? g_mem[addr] : 0;
        fprintf(stderr, "[pcread] $%06X.b = $%02X  M68K_pc=$%06X fn=$%06X\n",
                addr, v, _rt_current_pc(), (unsigned)g_rt_last_call);
    }
    if (addr < RT_MEM_SIZE && !is_hw(addr))
        return g_mem[addr];
    return hw_read8(addr);
}

uint16_t rt_read16(M68KCtx *ctx, uint32_t addr)
{
    (void)ctx;
    addr &= 0xFFFFFF;
    _rt_spin_check(ctx, addr);
    if (s_chip_rwatch_n && (rt_chip_rwatch_hit(addr) || rt_chip_rwatch_hit(addr+1))) {
        uint16_t v = (addr + 1 < RT_MEM_SIZE && !is_hw(addr))
                     ? (uint16_t)((g_mem[addr] << 8) | g_mem[addr+1]) : 0;
        fprintf(stderr, "[pcread] $%06X.w = $%04X  M68K_pc=$%06X fn=$%06X\n",
                addr, v, _rt_current_pc(), (unsigned)g_rt_last_call);
    }
    if (addr + 1 < RT_MEM_SIZE && !is_hw(addr))
        return (uint16_t)((g_mem[addr] << 8) | g_mem[addr+1]);
    return hw_read16(addr);
}

uint32_t rt_read32(M68KCtx *ctx, uint32_t addr)
{
    (void)ctx;
    addr &= 0xFFFFFF;
    if (s_chip_rwatch_n) {
        int hit = 0;
        for (int i = 0; i < 4; i++) if (rt_chip_rwatch_hit(addr + i)) { hit = 1; break; }
        if (hit) {
            uint32_t v = (addr + 3 < RT_MEM_SIZE && !is_hw(addr))
                ? (((uint32_t)g_mem[addr] << 24) | ((uint32_t)g_mem[addr+1] << 16) |
                   ((uint32_t)g_mem[addr+2] << 8)  | (uint32_t)g_mem[addr+3])
                : 0;
            fprintf(stderr, "[pcread] $%06X.l = $%08X  M68K_pc=$%06X fn=$%06X\n",
                    addr, v, _rt_current_pc(), (unsigned)g_rt_last_call);
        }
    }
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
    if (s_chip_watch_n && rt_chip_watch_hit(addr))
        fprintf(stderr, "[pcwatch] $%06X.b = $%02X  M68K_pc=$%06X fn=$%06X\n",
                addr, v, _rt_current_pc(), (unsigned)g_rt_last_call);
    _gp4d(addr, v, 1);
    { static int w = -1; if (w < 0) w = getenv("GP_RDY_WATCH") ? 1 : 0;
      if (w && (addr == 0x57FEBEu || addr == 0x57FEBFu)) {
          static int n = 0; if (n++ < 80)
              printf("[rdy-write8] $%06X (hi=%s) = %02X (pc=$%06X)%s\n", addr,
                     addr==0x57FEBEu?"yes":"no", v, _rt_current_pc(),
                     (addr==0x57FEBEu && (v&0x80))?"  <-- BIT15 READY":""); } }
    { static int w = -1; if (w < 0) w = getenv("LNAME_WATCH") ? 1 : 0;
      if (w && (addr >= 0x5786ACu && addr < 0x5786ACu + 440u)) {
          static int n = 0; if (n++ < 32)
              printf("[lname-write8] $%06X = %02X  pc=$%06X  last_call=$%06X\n",
                     addr, v, _rt_current_pc(), (unsigned)g_rt_last_call); } }
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
    if (s_chip_watch_n && (rt_chip_watch_hit(addr) || rt_chip_watch_hit(addr+1)))
        fprintf(stderr, "[pcwatch] $%06X.w = $%04X  M68K_pc=$%06X fn=$%06X\n",
                addr, v, _rt_current_pc(), (unsigned)g_rt_last_call);
    _gp4d(addr, v, 2);
    { static int watch = -1; if (watch < 0) watch = getenv("GP_PAL_WATCH") ? 1 : 0;
      if (watch && ((addr >= 0x34F4u && addr < 0x3700u) || (addr >= 0x57F100u && addr < 0x57F300u))) {
          static int n = 0; if (n++ < 60)
              printf("[pal-write] $%06X = %04X (pc=$%06X)\n", addr, v, _rt_current_pc()); } }
    { static int w = -1; if (w < 0) w = getenv("GP_RDY_WATCH") ? 1 : 0;
      if (w && addr == 0x57FEBEu) {   /* $10AC(a5) gameplay status word; bit15=level-ready */
          static int n = 0; if (n++ < 80)
              printf("[rdy-write] $10AC(a5)=%04X (pc=$%06X)%s\n", v, _rt_current_pc(), (v&0x8000)?"  <-- BIT15 READY":""); } }
    { static int w = -1; if (w < 0) w = getenv("LNAME_WATCH") ? 1 : 0;
      if (w && (addr >= 0x5786ACu && addr < 0x5786ACu + 440u)) {
          static int n = 0; if (n++ < 32)
              printf("[lname-write16] $%06X = %04X  pc=$%06X  last_call=$%06X\n",
                     addr, v, _rt_current_pc(), (unsigned)g_rt_last_call); } }
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
    if (s_chip_watch_n) {
        int hit = 0;
        for (int i = 0; i < 4; i++) if (rt_chip_watch_hit(addr + i)) { hit = 1; break; }
        if (hit)
            fprintf(stderr, "[pcwatch] $%06X.l = $%08X  M68K_pc=$%06X fn=$%06X\n",
                    addr, v, _rt_current_pc(), (unsigned)g_rt_last_call);
    }
    _gp4d(addr, v, 4);
    { static int w = -1; if (w < 0) w = getenv("LNAME_WATCH") ? 1 : 0;
      if (w && ((addr >= 0x5786ACu && addr < 0x5786ACu + 440u) || (addr >= 0x114u && addr < 0x118u))) {
          static int n = 0; if (n++ < 32)
              printf("[lname-write32] $%06X = %08X  pc=$%06X  last_call=$%06X\n",
                     addr, v, _rt_current_pc(), (unsigned)g_rt_last_call); } }
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

/* Dynamic table — a fixed cap silently dropped registrations once (64 cap,
 * ~100 registrations: late-registered wrappers never fired, looked like the
 * feature "didn't work"). Registration happens at startup only, so growth is
 * cheap; lookup is unchanged. */
static struct OverrideEnt {
    uint32_t addr;
    NativeFn fn;
    int      gp;     /* 1 = gameplay-bank override (fires only when g_gameplay_active) */
} *g_overrides = NULL;
static int g_override_count = 0;
static int g_override_cap   = 0;

static void rt_register_override_x(uint32_t addr, NativeFn fn, int gp)
{
    if (addr == 0) {
        g_override_count = 0;
        return;
    }
    if (g_override_count >= g_override_cap) {
        int ncap = g_override_cap ? g_override_cap * 2 : 128;
        void *p = realloc(g_overrides, (size_t)ncap * sizeof *g_overrides);
        if (!p) {
            fprintf(stderr, "FATAL: out of memory growing override table (%d)\n", ncap);
            abort();
        }
        g_overrides = p;
        g_override_cap = ncap;
    }
    g_overrides[g_override_count].addr = addr;
    g_overrides[g_override_count].fn   = fn;
    g_overrides[g_override_count].gp   = gp;
    g_override_count++;
}

void rt_register_override(uint32_t addr, NativeFn fn)    { rt_register_override_x(addr, fn, 0); }
void rt_register_override_gp(uint32_t addr, NativeFn fn) { rt_register_override_x(addr, fn, 1); }



static NativeFn override_lookup(uint32_t addr)
{
    /* In gameplay, only gameplay overrides (gp=1) fire — the title overrides at
     * the same addresses hold different loaded code with their own gpl-bank fns.
     * Outside gameplay, only the title/intro overrides (gp=0); and intro
     * overrides are skipped once the title-state overlay is live above $3294
     * (those addresses hold different code), except the shared resident region.
     *
     * EXCEPTION: a few title-menu-state addresses (e.g. $003C5A, the alternate
     * entry to the menu-input handler) DO need overrides to fire even when
     * g_overlay_active=1, because we're in the MENU state (g_overlay_active=1,
     * g_gameplay_active=0 — fire hasn't entered gameplay yet) and the engine
     * still calls the title-bank code there. Allow-list those. */
    int want_gp = g_gameplay_active ? 1 : 0;
    int allow_in_menu = (addr == 0x003C5Au || addr == 0x003C6Eu ||
                         addr == 0x003C88u || addr == 0x003C9Au ||
                         addr == 0x000039D0u || addr == 0x000049B6u ||
                         addr == 0x00003872u || addr == 0x00003DAAu);
    if (!want_gp && g_overlay_active && addr >= 0x3294u && !allow_in_menu)
        return NULL;
    /* First pass: prefer an exact bank match (gp=1 in gameplay, gp=0 outside).
     * Second pass: for RESIDENT-REGION addresses (<$3294), the same bytes are
     * present in both banks — the original code at e.g. $150 is identical
     * whether called from title or from gameplay. Fall back to the other
     * bank's override so we don't crash when gameplay code calls the loader
     * (which we override under gp=0). Without this fallback the win-sequence
     * `$5773A2: jmp $150` rt-misses because $150 isn't recompiled as a gpl
     * function. */
    for (int i = 0; i < g_override_count; i++) {
        if (g_overrides[i].addr == addr && g_overrides[i].gp == want_gp)
            return g_overrides[i].fn;
    }
    if (addr < 0x3294u) {
        for (int i = 0; i < g_override_count; i++) {
            if (g_overrides[i].addr == addr) return g_overrides[i].fn;
        }
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
/* Credits / end-game bank: the engine the $150 d0=3 path brings in (Disk.3 →
 * $3330, length $1888C). Different bytes than the gameplay bank at the same
 * addresses, so we need a separate dispatch table. Selected by
 * g_credits_active. */
extern const GameFnEntry g_fn_table_credits[] __attribute__((weak));
extern const int         g_fn_credits_count   __attribute__((weak));
/* g_overlay_active, g_gameplay_active, g_credits_active live on g_state. */

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
    /* Credits/end-game bank takes top priority when active. Its bytes overlay
     * the same $3330+ addresses as the gameplay bank but with completely
     * different code, so we must NOT fall through to gameplay/gp/intro within
     * the overlaid range. */
    if (g_credits_active && &g_fn_credits_count) {
        GameFn fn = table_search(g_fn_table_credits, g_fn_credits_count, addr);
        if (fn) return fn;
        if (addr >= OVERLAY_RESIDENT_END) return NULL;
    }
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
/* Trampoline source (defined below near rt_jump); forward-declared for
 * rt_miss_caller. */
static uint32_t s_rt_jump_source;

/* Best-effort "where did this call come from?" for diagnostics. Prefer the
 * trampoline-recorded source (set by rt_jump when an override or recompiled fn
 * hands off via tail rt_jump) — native overrides don't update
 * rt_last_insn_addr, so without this, trampoline hops reported "from $000000".
 * Fall back to the most recent recompiled-instruction PC, then to the prior
 * call-stack entry, then 0. */
static uint32_t rt_miss_caller(void)
{
    if (s_rt_jump_source) return s_rt_jump_source;
    if (rt_last_insn_addr) return rt_last_insn_addr;
    if (rt_callstack_sp >= 2) return rt_callstack[rt_callstack_sp - 2];
    return 0;
}

static void rt_miss(uint32_t addr, M68KCtx *ctx)
{
    uint32_t from = rt_miss_caller();
    /* Targeted miss trace: log full call stack for specific addrs. */
    if (addr == 0x003C5Au) {
        fprintf(stderr, "[miss-trace] $%06X miss; call stack (sp=%d):\n", addr, rt_callstack_sp);
        for (int i = rt_callstack_sp - 1; i >= 0 && i >= rt_callstack_sp - 16; i--) {
            fprintf(stderr, "  [%d] $%06X\n", i, rt_callstack[i]);
        }
        fprintf(stderr, "  D0=%08X D1=%08X D2=%08X D6=%08X D7=%08X\n",
                ctx->D[0], ctx->D[1], ctx->D[2], ctx->D[6], ctx->D[7]);
        fflush(stderr);
    }
    /* Collect every DISTINCT missing call target to stderr (one run → full list
     * of functions to seed). A skipped call drops the function's work entirely,
     * corrupting state — so for the gameplay bank these must all be resolved.
     * The "from" includes BOTH the trampoline source (the fn that did
     * rt_jump → us) and a chain of recent fns, since the immediate caller is
     * often itself a tail-jump trampoline and only the fuller chain pinpoints
     * the originating dispatch site. */
    {
        static uint32_t seen[512]; static int nseen = 0;
        int known = 0;
        for (int i = 0; i < nseen; i++) if (seen[i] == addr) { known = 1; break; }
        if (!known && nseen < 512) {
            seen[nseen++] = addr;
            fprintf(stderr, "[rt-miss] $%06X (from $%06X)  chain:",
                    addr, from);
            /* Last few entries of the call stack — newest-first. Skip the
             * topmost which is `addr` itself (already printed). */
            int hi = rt_callstack_sp - 2;
            int lo = hi - 7; if (lo < 0) lo = 0;
            for (int i = hi; i >= lo; i--)
                fprintf(stderr, " $%06X", rt_callstack[i]);
            fprintf(stderr, "\n");
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

    static int allow = -1;
    if (allow < 0) allow = getenv("RT_ALLOW_MISS") ? 1 : 0;
    static int fail = -1;
    if (fail < 0) fail = getenv("RT_FAIL_ON_MISS") ? 1 : 0;
    if ((g_gameplay_active && !allow) || fail) {
        fprintf(stderr, "\n*** rt_call: NO FUNCTION at $%06X (from $%06X)"
                " — gameplay/RT_FAIL_ON_MISS, aborting ***\n", addr, from);
        fprintf(stderr, "    D0=%08X D1=%08X D2=%08X A0=%08X A1=%08X A2=%08X\n",
                ctx->D[0], ctx->D[1], ctx->D[2], ctx->A[0], ctx->A[1], ctx->A[2]);
        /* Dump g_mem so post-mortem tools can walk the live object/dispatch
         * structures (which contain the runtime-computed code pointers static
         * descent can't reach). Same file the watchdog uses. */
        { FILE *f = fopen("logs/pc_freeze.bin", "wb");
          if (f) { fwrite(g_mem, 1, RT_MEM_SIZE, f); fclose(f);
                   fprintf(stderr, "    dumped g_mem -> logs/pc_freeze.bin\n"); } }
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
/* rt_jump trampoline state (single-threaded by design). The generated code
 * emits `rt_jump(ctx, X); return;` — without this, each rt_jump invokes
 * rt_call_impl → fn → rt_jump → rt_call_impl ... and a state-machine cycle
 * like $59E198 ↔ $59E28C ↔ $59E1FA ↔ $59E1C6 grows the C stack until SIGSEGV.
 * Instead, rt_jump records the target and returns; the enclosing rt_call_impl
 * loops, picking up the new target. The Amiga RA slot is reserved once for
 * the outermost call and released once on exit.
 *
 * s_rt_jump_source captures the address of the fn that issued the rt_jump —
 * native overrides don't update rt_last_insn_addr, so without this an rt-miss
 * triggered through a trampoline hop reports "from $000000". With it the
 * caller chain is preserved across trampoline hops. */
static int      s_rt_jump_pending = 0;
static uint32_t s_rt_jump_target  = 0;
/* s_rt_jump_source declared near rt_miss_caller above. */

static void rt_call_impl(M68KCtx *ctx, uint32_t addr, int is_call)
{
    /* Save the parent invocation's trampoline source so a recursive rt_call
     * (e.g. from a native override) doesn't smear our source onto the parent's
     * downstream rt_miss. Restored before return. */
    uint32_t saved_source = s_rt_jump_source;
    s_rt_jump_source = 0;
    s_rt_jump_pending = 0;

    int ra_set = 0;

    for (;;) {
        g_rt_last_call = addr;    /* watchdog diagnostic: function most recently entered */
        rt_recent[rt_recent_pos] = addr;
        rt_recent_pos = (rt_recent_pos + 1) % RT_RECENT;
        rt_push_call(addr);
        if (is_call && g_gameplay_active && !ra_set) {
            ctx->A[7] -= 4u; rt_write32(ctx, ctx->A[7], rt_last_insn_addr);
            ra_set = 1;
        }
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
            s_rt_jump_pending = 0;
            native(ctx);
            rt_pop_call();
            /* Some overrides (e.g. native_overlay_loader at $6D714) end with
             * rt_jump(X) to hand off to recompiled code — same trampoline
             * semantics as generated fns. */
            if (!s_rt_jump_pending) break;
            addr = s_rt_jump_target;
            is_call = 0;
            s_rt_jump_pending = 0;
            continue;
        }
        GameFn fn = dispatch_lookup(addr);
        if (fn) {
            s_rt_jump_pending = 0;
            fn(ctx);
            rt_pop_call();
            if (!s_rt_jump_pending) break;
            addr = s_rt_jump_target;
            is_call = 0;
            s_rt_jump_pending = 0;
            continue;
        }
        if (getenv("RT_SKIPLOG"))
            GLOBAL_LOG("[rt-skip] no function at $%06X (from $%06X) D0=%08X D1=%08X D2=%08X A0=%08X A1=%08X\n",
                       addr, rt_last_insn_addr, ctx->D[0], ctx->D[1], ctx->D[2], ctx->A[0], ctx->A[1]);
        RT_LOG("rt_call: no function at $%06X – skipping\n", addr);
        rt_miss(addr, ctx);
        rt_pop_call();
        break;
    }
    if (ra_set) ctx->A[7] += 4u;
    s_rt_jump_source = saved_source;
}

void rt_call(M68KCtx *ctx, uint32_t addr) { rt_call_impl(ctx, addr, 1); }

/* Re-enter the game flow at `addr` as a tail transfer (is_call=0), NOT a call.
 * Used by the savestate LOAD path to resume a resumable function mid-cycle
 * ($577114 with ctx->resume set): a normal rt_call would push a return address
 * onto the M68K stack (ctx->A[7]) and corrupt the saved mid-cycle a7, since
 * $577114 — unlike the gameplay entry $577000 — does not reset a7. is_call=0
 * runs the same dispatch trampoline without that push. */
void rt_resume(M68KCtx *ctx, uint32_t addr) { rt_call_impl(ctx, addr, 0); }

/* Reset the dispatch call-stack. Used when the old game thread is discarded on a
 * savestate load (its stack is abandoned mid-block) so stale entries don't linger
 * into the freshly spawned resume thread. */
void rt_reset_callstack(void)
{
    rt_callstack_sp = 0;
    s_rt_jump_pending = 0;
}

void rt_jump(M68KCtx *ctx, uint32_t addr)
{
    /* Generated callers do `rt_jump(ctx, X); return;` — defer dispatch to the
     * enclosing rt_call_impl loop so a state-machine cycle doesn't recurse.
     * Record the SOURCE so a downstream rt-miss reports a useful caller (the
     * fn that handed control to the missing target), not "from $000000". The
     * source is the most recent valid entry on the call-stack — that's the
     * fn currently executing when rt_jump fires. */
    s_rt_jump_pending = 1;
    s_rt_jump_target  = addr;
    s_rt_jump_source  = (rt_callstack_sp > 0)
                            ? rt_callstack[rt_callstack_sp - 1]
                            : 0;
    (void)ctx;
}

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
