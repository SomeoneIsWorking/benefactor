/* harness/puae_snap_impl.c
 *
 * This file is #included at the very end of custom.c when HARNESS_BUILD is
 * defined.  Because it lives in the same translation unit it has direct access
 * to all static variables (bplpt, bpl1mod/bpl2mod, cop1lc, cop2lc, etc.).
 *
 * Do NOT compile this file on its own.
 */

/* Relative include path as seen from sources/src/custom.c */
#include "../../src/harness/puae_state.h"
#include "../../src/harness/trace.h"
#include "../../src/common_log.h"
#include "audio.h"

/* chipmem_bank is declared in memory.h, which custom.c already includes */

/* ── PUAE write tracing ───────────────────────────────────────────────────── */

/* We wrap chipmem_bank.wput (CPU writes) and chipmem_wput_indirect (blitter)
 * so every write to the copper-list area is recorded in the shared trace
 * buffer alongside the PC-port writes. */

static void (REGPARAM3 *orig_chipmem_wput)(uaecptr, uae_u32) REGPARAM;
static void (REGPARAM3 *orig_chipmem_wput_indirect)(uaecptr, uae_u32) REGPARAM;
static void (REGPARAM3 *orig_chipmem_bput)(uaecptr, uae_u32) REGPARAM;
static void (REGPARAM3 *orig_chipmem_lput)(uaecptr, uae_u32) REGPARAM;
static void (REGPARAM3 *orig_chipmem_lput_indirect)(uaecptr, uae_u32) REGPARAM;
static int s_puae_trace_inited = 0;

/* TEMP diagnostic: log who writes the $4d064 level table (CPU vs blitter). */
static void puae_log_4d(uaecptr addr, uae_u32 v, int sz, int indirect)
{
    static int live = 0;
    if (live < 5) { live++;
        fprintf(stderr, "[wlive] $%06X <- %08X (sz%d) %s\n", (unsigned)addr, (unsigned)v, sz, indirect?"BLIT":"CPU"); }
    if (addr >= 0x4c000u && addr < 0x4e000u)
        fprintf(stderr, "[w4d] $%06X <- %08X (sz%d) %s pc=$%06X\n",
                (unsigned)addr, (unsigned)v, sz, indirect ? "BLIT" : "CPU", (unsigned)m68k_getpc());
}

static int _puae_is_state_watch_addr(uaecptr addr)
{
    return (addr >= 0x0042FCu && addr < 0x0042FEu) ||
           (addr >= 0x0069F0u && addr < 0x006AEAu) ||
           (addr >= 0x07FFA2u && addr < 0x080000u);
}

static int _puae_is_timer_selector_addr(uaecptr addr)
{
    return (addr >= 0x0069F0u && addr < 0x006AEAu) ||
           (addr >= 0x07FFA2u && addr < 0x080000u);
}

static int _puae_is_focus_addr(uaecptr addr)
{
    return (addr >= 0x0042FCu && addr < 0x004308u) ||
           (addr >= 0x0069F1u && addr < 0x0069FEu) ||
           (addr >= 0x006A27u && addr < 0x006AEAu) ||
           (addr >= 0x07FFA2u && addr < 0x080000u);
}

static int _puae_is_bpl_addr(uaecptr addr)
{
    return (addr >= 0x04F6F4u && addr < 0x0546F4u) ||
           (addr >= 0x070930u && addr < 0x075930u) ||
           (addr >= 0x04F71Cu && addr < 0x05471Cu) ||
           (addr >= 0x025370u && addr < 0x02A370u);
}

/* Address ranges we care about: copper list ($86CC-$88CC) and $7Cxx tables */
static int _puae_addr_interesting(uaecptr addr)
{
        return (addr >= 0x86CCu && addr < 0x88CCu) ||
            (addr >= 0x7BC8u && addr < 0x7E00u) ||
            /* Focused bitplane windows around current first-diff bytes */
            (addr >= 0x04F74Bu && addr < 0x04F76Cu) ||
            (addr >= 0x070924u && addr < 0x070945u) ||
            (addr >= 0x025387u && addr < 0x0253A8u) ||
            /* Upstream source-chain windows feeding those writes */
            (addr >= 0x048240u && addr < 0x048260u) ||
            (addr >= 0x069300u && addr < 0x069500u) ||
            (addr >= 0x061E80u && addr < 0x061EC0u) ||
            _puae_is_state_watch_addr(addr);
}

static void REGPARAM2 puae_trace_bput(uaecptr addr, uae_u32 b)
{
    if (orig_chipmem_bput) {
        if (_puae_addr_interesting(addr)) {
            uint8_t old_v = (addr < chipmem_bank.allocated_size) ? chipmem_bank.baseaddr[addr] : 0;
            trace_write_puae((uint32_t)addr, old_v, (uint8_t)b, 1, (uint32_t)m68k_getpc(), 0);
        }
        puae_log_4d(addr, b, 1, 0);
        orig_chipmem_bput(addr, b);
    }
}

static void REGPARAM2 puae_trace_wput(uaecptr addr, uae_u32 w)
{
    if (orig_chipmem_wput) {
        if (_puae_addr_interesting(addr)) {
            uint16_t old_v = (addr + 1 < chipmem_bank.allocated_size)
                ? (uint16_t)((chipmem_bank.baseaddr[addr] << 8) | chipmem_bank.baseaddr[addr + 1])
                : 0;
            trace_write_puae((uint32_t)addr, old_v, (uint16_t)w, 2, (uint32_t)m68k_getpc(), 0);
        }
        puae_log_4d(addr, w, 2, 0);
        orig_chipmem_wput(addr, w);
    }
}

static void REGPARAM2 puae_trace_wput_indirect(uaecptr addr, uae_u32 w)
{
    if (orig_chipmem_wput_indirect) {
        if (_puae_addr_interesting(addr)) {
            uint16_t old_v = (addr + 1 < chipmem_bank.allocated_size)
                ? (uint16_t)((chipmem_bank.baseaddr[addr] << 8) | chipmem_bank.baseaddr[addr + 1])
                : 0;
            trace_write_puae((uint32_t)addr, old_v, (uint16_t)w, 2, (uint32_t)m68k_getpc(), 1);
        }
        puae_log_4d(addr, w, 2, 1);
        orig_chipmem_wput_indirect(addr, w);
    }
}

static void REGPARAM2 puae_trace_lput(uaecptr addr, uae_u32 l)
{
    if (orig_chipmem_lput) {
        if (_puae_addr_interesting(addr)) {
            uint32_t old_v = (addr + 3 < chipmem_bank.allocated_size)
                ? (((uint32_t)chipmem_bank.baseaddr[addr] << 24) |
                   ((uint32_t)chipmem_bank.baseaddr[addr + 1] << 16) |
                   ((uint32_t)chipmem_bank.baseaddr[addr + 2] << 8) |
                   ((uint32_t)chipmem_bank.baseaddr[addr + 3]))
                : 0;
            trace_write_puae((uint32_t)addr, old_v, (uint32_t)l, 4, (uint32_t)m68k_getpc(), 0);
        }
        puae_log_4d(addr, l, 4, 0);
        orig_chipmem_lput(addr, l);
    }
}

static void REGPARAM2 puae_trace_lput_indirect(uaecptr addr, uae_u32 l)
{
    if (orig_chipmem_lput_indirect) {
        if (_puae_addr_interesting(addr)) {
            uint32_t old_v = (addr + 3 < chipmem_bank.allocated_size)
                ? (((uint32_t)chipmem_bank.baseaddr[addr] << 24) |
                   ((uint32_t)chipmem_bank.baseaddr[addr + 1] << 16) |
                   ((uint32_t)chipmem_bank.baseaddr[addr + 2] << 8) |
                   ((uint32_t)chipmem_bank.baseaddr[addr + 3]))
                : 0;
            trace_write_puae((uint32_t)addr, old_v, (uint32_t)l, 4, (uint32_t)m68k_getpc(), 1);
        }
        puae_log_4d(addr, l, 4, 1);
        orig_chipmem_lput_indirect(addr, l);
    }
}

/* (Re)install the write wrappers. Must be called AGAIN after a state restore
 * (retro_unserialize), which resets the bank handlers back to the originals and
 * would otherwise leave our traces uninstalled. Idempotent: it only captures an
 * original handler when the current one isn't already our wrapper. */
void puae_trace_init(void)
{
    s_puae_trace_inited = 1;
    if (chipmem_bank.bput        != puae_trace_bput)          orig_chipmem_bput          = chipmem_bank.bput;
    if (chipmem_bank.wput        != puae_trace_wput)          orig_chipmem_wput          = chipmem_bank.wput;
    if (chipmem_bank.lput        != puae_trace_lput)          orig_chipmem_lput          = chipmem_bank.lput;
    if (chipmem_wput_indirect    != puae_trace_wput_indirect) orig_chipmem_wput_indirect = chipmem_wput_indirect;
    if (chipmem_lput_indirect    != puae_trace_lput_indirect) orig_chipmem_lput_indirect = chipmem_lput_indirect;
    chipmem_bank.bput = puae_trace_bput;
    chipmem_bank.wput = puae_trace_wput;
    chipmem_bank.lput = puae_trace_lput;
    chipmem_wput_indirect = puae_trace_wput_indirect;
    chipmem_lput_indirect = puae_trace_lput_indirect;
}

/* ── Snap / dump functions ────────────────────────────────────────────────── */

void puae_snap_state(FrameState *s)
{
    int i;

    s->cop1lc = cop1lc;
    s->cop2lc = cop2lc;
    s->bplcon0 = bplcon0;
    s->bplcon1 = bplcon1;
    s->bplcon2 = bplcon2;
    s->bpl1mod = (int16_t)bpl1mod;
    s->bpl2mod = (int16_t)bpl2mod;
    s->diwstrt = (uint16_t)diwstrt;
    s->diwstop = (uint16_t)diwstop;
    s->ddfstrt = (uint16_t)ddfstrt;
    s->ddfstop = (uint16_t)ddfstop;

    for (i = 0; i < 6; i++)
        s->bplpt[i] = (uint32_t)bplpt[i];

    for (i = 0; i < 8; i++)
        s->sprpt[i] = (uint32_t)spr[i].pt;

    /* Use color_regs_aga if AGA mode is active, else color_regs_ecs */
    for (i = 0; i < 32; i++) {
#ifdef AGA
        if (aga_mode) {
            uae_u32 aga = (uae_u32)current_colors.color_regs_aga[i];
            s->palette[i] = (uint16_t)(
                (((aga >> 16) & 0xF) << 8) |
                (((aga >> 8)  & 0xF) << 4) |
                ((aga >> 4)   & 0xF)
            );
        } else
#endif
            s->palette[i] = current_colors.color_regs_ecs[i];
    }

    /* Grab the first HARNESS_COPLIST_WORDS words of the copper list */
    s->coplist_valid = 0;
    if (chipmem_bank.baseaddr && cop1lc + HARNESS_COPLIST_WORDS * 2 <= (uaecptr)chipmem_bank.allocated_size) {
        uint8_t *p = chipmem_bank.baseaddr + cop1lc;
        for (i = 0; i < HARNESS_COPLIST_WORDS; i++)
            s->coplist[i] = (uint16_t)((p[i * 2] << 8) | p[i * 2 + 1]);
        s->coplist_valid = 1;
    }

    /* CRC32 of full chip RAM — catches bitplane/sprite data divergence */
    s->chipram_crc = 0;
    if (chipmem_bank.baseaddr && chipmem_bank.allocated_size >= 524288)
        s->chipram_crc = crc32_buf(chipmem_bank.baseaddr, 524288);

    /* CRC32 of active bitplane data regions only — used as DIFF trigger */
    s->bpl_data_crc = 0;
    if (chipmem_bank.baseaddr)
        s->bpl_data_crc = bpl_data_region_crc(chipmem_bank.baseaddr,
                                               (uint32_t)chipmem_bank.allocated_size);

    /* Paula audio channels */
    puae_audio_snap(s->audio);

    /* Animation phase tracking */
    s->flip_flop  = 0;
    s->sprite_ctr = 0;
    if (chipmem_bank.baseaddr && chipmem_bank.allocated_size >= 0x4300) {
        uint8_t *m = chipmem_bank.baseaddr;
        s->flip_flop  = (uint16_t)((m[0x41A2] << 8) | m[0x41A3]);
        s->sprite_ctr = (uint16_t)((m[0x42FC] << 8) | m[0x42FD]);
        s->anim_ptr   = ((uint32_t)m[0x42FE] << 24) | ((uint32_t)m[0x42FF] << 16) |
                        ((uint32_t)m[0x4300] << 8)  |  (uint32_t)m[0x4301];
    }
}

/* Lightweight accessor — just returns cop1lc; used during fast-forward boot
 * to avoid a full puae_snap_state() (which CRC32s 512KB of chip RAM). */
uint32_t puae_get_cop1lc(void)
{
    return (uint32_t)cop1lc;
}

/* Copy PUAE's chip RAM into a caller-supplied buffer.
 * Returns the number of bytes actually copied (≤ maxbytes). */
/* Host pointer to Amiga chip RAM base — for GDB to set a hardware watchpoint on
 * a specific chip address (GDB can't resolve chipmem_bank.baseaddr directly). */
void *puae_chip_baseaddr(void) { return chipmem_bank.baseaddr; }

int puae_dump_chipram(void *buf, int maxbytes)
{
    if (!chipmem_bank.baseaddr || chipmem_bank.allocated_size <= 0)
        return 0;
    int sz = (int)chipmem_bank.allocated_size;
    if (sz > maxbytes) sz = maxbytes;
    memcpy(buf, chipmem_bank.baseaddr, sz);
    return sz;
}

/* Copy `len` bytes of PUAE memory starting at Amiga address `addr` (any bank —
 * chip, fast, expansion) via the CPU memory map. RAM reads are side-effect-free.
 * Used to inspect gameplay state/code in fast RAM ($577000+) that the chip dump
 * doesn't cover. */
int puae_dump_mem(uint32_t addr, void *buf, int len)
{
    uint8_t *b = (uint8_t *)buf;
    for (int i = 0; i < len; i++)
        b[i] = (uint8_t)get_byte(addr + (uint32_t)i);
    return len;
}

/* Poke PUAE memory through the CPU map (covers chip + fast). Used to steer the
 * reference, e.g. write the level word $20.w at the gameplay engine entry so
 * PUAE loads an arbitrary level without a keyboard-driven password. */
void puae_poke_mem(uint32_t addr, const void *buf, int len)
{
    const uint8_t *b = (const uint8_t *)buf;
    for (int i = 0; i < len; i++)
        put_byte(addr + (uint32_t)i, b[i]);
}

/* Save PUAE audio channel state (raw register values) to a file.
 * Called once at the sync point so the PC port can pre-initialize its
 * audio register shadows and produce matching audio snapshots. */
void puae_dump_audio_regs(const char *path)
{
    AudioChanSnap snap[4];
    puae_audio_snap(snap);
    FILE *fp = fopen(path, "wb");
    if (fp) {
        fwrite(snap, sizeof(snap), 1, fp);
        fclose(fp);
    }
}
