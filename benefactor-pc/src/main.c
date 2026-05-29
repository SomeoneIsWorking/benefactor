/*
 * main.c  –  Benefactor PC Port entry point.
 *
 * Architecture overview
 * ─────────────────────
 * This is a thin Amiga compatibility layer, not a general emulator.
 * We load the user-supplied WHDLoad disk images (Disk.1 / Disk.2 / Disk.3),
 * run the game's own 68k code inside a Musashi CPU core, and redirect
 * custom-chip accesses to SDL2.
 *
 * No copyrighted data is distributed.  Users must supply their own
 * disk images from their original WHDLoad installation.
 *
 * Boot sequence (derived from WHDLoad slave source Benefactor.asm):
 *   1.  Load first $1600 bytes of Disk.1 to chip RAM $76000
 *   2.  Install ILLEGAL ($4AFC) at the game's disk-Load routine ($7644E)
 *       so that we can intercept it and serve data from the image files.
 *   3.  Start the 68k CPU at $76412 with SP = $80000
 *   4.  The game's own boot code calls Load → our ILLEGAL handler
 *       reads the right chunk from Disk.1 into $3000 (145 KB)
 *   5.  The game's own Decrunch routine ($765E4) decompresses in-place
 *   6.  CPU jumps to $3000; second-stage loader runs, loading the main
 *       executable to $80000; further Load/Decrunch calls are intercepted
 *       at the addresses the WHDLoad slave would have patched.
 *   7.  Once the game reaches its main loop, vblank interrupts drive the
 *       SDL2 display and audio.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "SDL2/SDL.h"

/* Amiga hardware layer */
#include "amiga/memory.h"
#include "amiga/cpu.h"
#include "amiga/custom.h"
#include "amiga/cia.h"
#include "amiga/disk.h"

/* Platform layer */
#include "platform/display.h"
#include "platform/audio.h"
#include "platform/input.h"

/* Musashi (needed here only for register access from the ILLEGAL callback) */
#include "m68k.h"

/* ── Constants ───────────────────────────────────────────────────────────── */

/* Chip RAM addresses for the loader and game.
 * These come from reading the WHDLoad slave source Benefactor.asm. */
#define LOADER_BASE     0x76000u   /* where the bootloader is loaded */
#define LOADER_SIZE     0x1600u    /* bytes loaded from Disk.1 track 0 */
#define LOADER_ENTRY    0x76412u   /* game entry point offset into loader */
#define LOADER_STACK    0x80000u   /* initial SP (top of 512 KB chip RAM) */

/* Addresses of the game's disk Load / Decrunch routines in the loader.
 * We write ILLEGAL ($4AFC) here to intercept and handle in C. */
#define LOAD_ADDR_1     0x7644Eu   /* initial loader's Load routine */
#define LOAD_ADDR_2     0x6D902u   /* Load after Patch installs secondary hooks */
#define LOAD_ADDR_3     0x80B8Cu   /* Load in the main executable (after expand) */

/* Magic word: ILLEGAL instruction (Motorola opcode $4AFC) */
#define ILLEGAL_OPCODE  0x4AFCu

/* ── Load-routine interception ───────────────────────────────────────────── */

/* Addresses that we intercept with ILLEGAL, plus a sentinel (0). */
static const uint32_t load_intercept_addrs[] = {
    LOAD_ADDR_1,
    LOAD_ADDR_2,
    LOAD_ADDR_3,
    0
};

static void install_load_traps(void)
{
    for (int i = 0; load_intercept_addrs[i]; i++) {
        /* ILLEGAL (2 bytes) + RTS (2 bytes) = minimal function replacement */
        mem_write16(load_intercept_addrs[i],     ILLEGAL_OPCODE);
        mem_write16(load_intercept_addrs[i] + 2, 0x4E75u);   /* RTS */
    }
}

/*
 * Decode the calling convention used by Benefactor's custom disk loader:
 *
 *   D0[7:0]   = disk number (0-based; add 1 for disk_load's 1-based API)
 *   D0[31:8]  = byte offset into the disk image
 *   D1        = number of bytes to load
 *   D2        = destination address in chip RAM
 */
static int handle_load_call(void)
{
    uint32_t d0 = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t d1 = m68k_get_reg(NULL, M68K_REG_D1);
    uint32_t d2 = m68k_get_reg(NULL, M68K_REG_D2);

    int      disk_no    = (int)(d0 & 0xFF) + 1;   /* convert to 1-based */
    uint32_t byte_off   = d0 >> 8;
    uint32_t length     = d1;
    uint32_t dst_addr   = d2;

    int loaded = disk_load(disk_no, byte_off, dst_addr, length);
    if (loaded < 0) {
        fprintf(stderr, "handle_load_call: disk %d, off %08x, len %u: failed\n",
                disk_no, byte_off, length);
        m68k_set_reg(M68K_REG_D0, (uint32_t)-1);
        return 1;
    }

    fprintf(stderr, "[Load] disk=%d off=0x%06x len=0x%x dst=0x%06x → %d bytes\n",
            disk_no, byte_off, length, dst_addr, loaded);

    m68k_set_reg(M68K_REG_D0, 0);   /* success */
    return 1;
}

/* Musashi ILLEGAL instruction callback.
 * Returns 1 if we handled it (suppresses exception), 0 to trigger exception. */
static int on_illegal_instruction(int opcode)
{
    /* Musashi has already advanced PC past the 2-byte ILLEGAL opcode */
    uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC) - 2;

    for (int i = 0; load_intercept_addrs[i]; i++) {
        if (pc == load_intercept_addrs[i])
            return handle_load_call();
    }

    /* Unknown ILLEGAL: let the CPU raise exception 4 */
    (void)opcode;
    fprintf(stderr, "ILLEGAL at PC=0x%06x\n", pc);
    return 0;
}

/* ── Interrupt handling ───────────────────────────────────────────────────── */

/*
 * Fire a 68k autovector interrupt.
 * Level 3 = VBLANK (Amiga INTREQ bit VERTB, autovector #3).
 * Level 2 = CIA-A PORTS (keyboard / timer) (autovector #2).
 */
static void deliver_interrupts(void)
{
    int cia_flags = cia_tick();

    if (custom_vblank_pending()) {
        custom_vblank_ack();
        if (custom_intena() & INTEN) {
            if (custom_intena() & VERTB) {
                cpu_irq(3);   /* VBL autovector */
            }
        }
    }

    if (cia_flags & (1 | 2 | 4)) {
        if (custom_intena() & INTEN) {
            if (custom_intena() & PORTS) {
                cpu_irq(2);   /* CIA-A PORTS autovector */
            }
        }
    }
}

/* ── Usage ───────────────────────────────────────────────────────────────── */

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s <Disk.1> <Disk.2> <Disk.3>\n\n"
        "Provide the three disk images from your WHDLoad Benefactor installation.\n"
        "Typically found in the Benefactor/ directory of your WHDLoad setup.\n\n"
        "Controls:\n"
        "  Arrow keys   – joystick movement\n"
        "  Ctrl / Space – fire\n"
        "  Alt+Enter    – toggle fullscreen\n"
        "  F10          – exit\n",
        prog);
}

/* ── Main ─────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (argc < 4) {
        print_usage(argv[0]);
        return 1;
    }

    /* ── Initialise subsystems ─────────────────────────────────────────── */

    mem_init();
    ciaa_init();
    ciab_init();
    custom_init();

    const char *disk_paths[3] = { argv[1], argv[2], argv[3] };
    if (disk_open(disk_paths, 3) < 0) {
        fprintf(stderr, "Failed to open disk images.\n");
        return 1;
    }

    if (display_init("Benefactor (Amiga) – PC Port") < 0) return 1;
    if (audio_init() < 0) {
        fprintf(stderr, "Warning: audio initialisation failed; continuing without audio.\n");
    }
    input_init();

    /* ── Load initial bootloader code ─────────────────────────────────── */

    int loaded = disk_load(1, 0, LOADER_BASE, LOADER_SIZE);
    if (loaded < (int)LOADER_SIZE) {
        fprintf(stderr, "Failed to load initial loader from Disk.1 (%d bytes)\n", loaded);
        return 1;
    }
    fprintf(stderr, "[Boot] Loaded %d bytes from Disk.1 to $%06x\n", loaded, LOADER_BASE);

    /* ── Install Load-routine intercepts ──────────────────────────────── */

    install_load_traps();

    /* ── Set up exception vectors in chip RAM ─────────────────────────── */

    /* Vector table: SSP at $0, initial PC at $4, etc.
     * We set these to something sensible; the game's boot code sets up
     * its own interrupt vectors as it initialises. */
    mem_write32(0x00, LOADER_STACK);    /* SSP */
    mem_write32(0x04, LOADER_ENTRY);    /* initial PC */

    /* TRAP #0 vector ($80) – not used by us directly, but set to safe value */
    mem_write32(0x80, 0x7FFFFEu);

    /* ── Initialise CPU ───────────────────────────────────────────────── */

    /* cpu_init calls m68k_pulse_reset which clears all callbacks.
     * We must register our ILLEGAL-instruction hook AFTER cpu_init. */
    cpu_init(LOADER_ENTRY, LOADER_STACK);
    m68k_set_illg_instr_callback(on_illegal_instruction);
    fprintf(stderr, "[Boot] CPU initialised, entry=0x%06x SP=0x%06x\n",
            LOADER_ENTRY, LOADER_STACK);

    /* ── Main loop ────────────────────────────────────────────────────── */

    int running = 1;
    int scanline = 0;
    int frame_count = 0;

    /* ~7.09 MHz / 228 cycles per scanline ≈ 31,084 scanlines/sec
     * 312 lines per PAL frame → ~99.7 fps before vsync.
     * We execute roughly one scanline's worth of 68k cycles per iteration. */
    const int CYCLES_PER_SCANLINE = 228;

    while (running) {
        /* Handle SDL events */
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            int result = input_handle_event(&ev);
            if (result == 1) { running = 0; break; }
            if (result == 2) display_toggle_fullscreen();
        }
        if (!running) break;

        /* Update joystick state into CIA / custom registers */
        input_update();

        /* Run one scanline of CPU */
        cpu_run(CYCLES_PER_SCANLINE);

        /* Custom chip scanline tick (copper, beam position) */
        custom_tick_scanline(scanline);

        /* One-shot: dump VPOSR + scanline when stuck at $373a first time */
        static int stuck_dbg = 0;
        if (!stuck_dbg && frame_count > 530) {
            uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC);
            if (pc == 0x373a || pc == 0x3732) {
                uint8_t vposr_lo = mem_read8(0xDFF005);
                fprintf(stderr, "[DBG] scanline=%d vposr_lo=%02x PC=%06x\n",
                        scanline, vposr_lo, pc);
                if (scanline == 257) stuck_dbg = 1;  /* only dump one cycle around 256 */
            }
        }

        /* Render this scanline now that copper has run */
        display_render_scanline(scanline);

        /* CIA timers */
        deliver_interrupts();

        scanline++;
        if (scanline >= 312) {
            scanline = 0;
            frame_count++;

            /* Periodic PC + display state dump so we can see what the game is doing */
            if (frame_count == 1 || (frame_count % 60) == 0) {
                uint32_t pc  = m68k_get_reg(NULL, M68K_REG_PC);
                uint32_t sp  = m68k_get_reg(NULL, M68K_REG_SP);
                uint32_t d0  = m68k_get_reg(NULL, M68K_REG_D0);
                uint32_t a6  = m68k_get_reg(NULL, M68K_REG_A6);
                uint16_t bc0 = custom_bplcon0();
                int npl = (bc0 >> 12) & 7;
                /* Read what the game reads at $3(a6) and (a6) */
                uint16_t at_a6    = mem_read16(a6);
                uint16_t at_a6_2  = mem_read16(a6 + 2);
                fprintf(stderr,
                    "[Frame %4d] PC=%06x SP=%06x D0=%08x A6=%06x "
                    "(a6)=%04x +2=%04x  BPLCON0=%04x npl=%d bpl0=%06x col0=%04x dmacon=%04x\n",
                    frame_count, pc, sp, d0, a6, at_a6, at_a6_2,
                    bc0, npl, custom_bplptr(0) & 0xFFFFFF, custom_color(0),
                    custom_dmacon());
            }

            /* One-shot copper list dump at frame 960 */
            if (frame_count == 960) {
                /* Copper list starts at $86CC (set by the game) */
                uint32_t cop_addr = custom_cop1lc();
                fprintf(stderr, "[CopDump] cop1lc=%06x\n", cop_addr);
                if (cop_addr < 0x80000) {
                    for (int ci = 0; ci < 32; ci++) {
                        uint16_t w1 = mem_read16(cop_addr + ci*4);
                        uint16_t w2 = mem_read16(cop_addr + ci*4 + 2);
                        fprintf(stderr, "  +%02d: %04x %04x\n", ci, w1, w2);
                    }
                }
            }

            /* Render frame – scanlines already drawn, just present */
            display_present();
        }
    }

    /* ── Shutdown ─────────────────────────────────────────────────────── */

    audio_fini();
    display_fini();
    disk_close();
    mem_fini();
    SDL_Quit();

    return 0;
}
