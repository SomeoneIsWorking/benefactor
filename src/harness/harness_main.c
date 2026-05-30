/* harness_main.c – PC vs PUAE side-by-side compare tool (no lockstep)
 *
 * Boots PUAE (reference oracle) and the PC port (its single native disk-boot
 * path) and drops into a REPL. The two cores run INDEPENDENTLY — there is no
 * frame lockstep and no per-frame divergence checking. The REPL sends fire to
 * both cores and lets you advance each (or both, interleaved) and inspect where
 * each one ends up. Works headless (default) or headed (--headed: side-by-side
 * window).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

/* Prevent libretro VFS from redefining FILE/fprintf/fflush/etc. */
#define SKIP_STDIO_REDEFINES

#include "sysconfig.h"
#include "libretro-core.h"
#include "libretro.h"

#include "recomp/hw.h"
#include "recomp/rt.h"
#include "pc.h"
#include "harness/puae_state.h"
#include "harness/harness_internal.h"

/* ═══════════════════════════════════════════════════════════════════════════ */
/* Harness state logs and framebuffers (defined here, used by other modules) */

FrameState s_puae_log[MAX_FRAMES];
FrameState s_pc_log[MAX_FRAMES];

uint32_t s_puae_fb_log[MAX_FB_FRAMES][FB_W * FB_H];
uint32_t s_pc_fb_log[MAX_FB_FRAMES][FB_W * FB_H];
int s_puae_fb_count = 0;
int s_pc_fb_count = 0;

uint8_t s_puae_chipram_snap[CHIP_RAM_SIZE];
uint8_t s_pc_chipram_snap[CHIP_RAM_SIZE];
int s_puae_chipram_valid = 0;
int s_pc_chipram_valid = 0;

uint32_t s_puae_fb[FB_W * FB_H];

uint32_t s_pc_prerender_bpl_crc = 0;
int      s_pc_prerender_bpl_crc_valid = 0;

/* Read by the per-instruction trace (rt.c) to tag log lines with a frame index. */
int g_harness_compared_frame = 0;

/* ═══════════════════════════════════════════════════════════════════════════ */
/* External functions from modules */

extern int run_puae_phase(const char *kick_dir, const char *whdload_path,
                          int boot_frames, int n_frames,
                          char *chipram_out_path, int chipram_out_len,
                          int display_only, int interactive);

extern void retro_unload_game(void);
extern void retro_deinit(void);
extern void retro_run(void);
extern void puae_snap_state(FrameState *s);
extern void harness_combined_init(void);
extern void harness_combined_present(void);
extern void harness_combined_fini(void);
extern void harness_interactive_delay(int ms);

extern void input_force_fire(int on);
extern void input_force_dir(int up, int down, int left, int right);
extern void input_poll(void);
extern int  input_esc(void);
extern void retro_set_controller_port_device(unsigned port, unsigned device);

int main(int argc, char **argv)
{
    /* Line-buffer stdout so messages survive the watchdog's _exit (which would
     * otherwise drop the buffered loader/diagnostic output). */
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc < 4) {
        fprintf(stderr,
            "Usage: %s <kick_dir> <whdload_path> <disk1> [disk2] [disk3] [--headed]\n"
            "\n"
            "Boots PUAE (reference) and the PC port (native disk boot) and drops\n"
            "into a REPL. The two cores run INDEPENDENTLY (no lockstep).\n"
            "\n"
            "REPL commands:\n"
            "  fire 0|1   hold/release fire+start on BOTH cores\n"
            "  pc [n]     advance the PC port n frames (default 1)\n"
            "  pu [n]     advance PUAE n frames (default 1)\n"
            "  both [n]   advance both n frames, interleaved (no comparison)\n"
            "  state      print each core's cop1lc\n"
            "  cmp        framebuffer pixel-diff PC vs PUAE\n"
            "  fb         dump logs/fb_pc.bin + logs/fb_puae.bin\n"
            "  q          quit\n",
            argv[0]);
        return 1;
    }

    const char *kick_dir   = argv[1];
    const char *whdload_path = argv[2];
    const char *disks[4]   = { NULL };
    int n_disks = 0;
    int headed  = 0;
    int play_mode = 0;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--headed")) headed = 1;
        else if (!strcmp(argv[i], "--play")) { play_mode = 1; headed = 1; }
        else if (n_disks < 3)             disks[n_disks++] = argv[i];
    }
    if (n_disks < 1) { fprintf(stderr, "[harness] need at least disk1\n"); return 1; }

    printf("[harness] === BENEFACTOR PC vs PUAE compare (no lockstep) ===\n");
    fflush(stdout);

    /* Boot PUAE (reference oracle) and leave it ready to step via retro_run(). */
    char chip_ram_path[512] = "";
    if (run_puae_phase(kick_dir, whdload_path, /*boot_frames*/5000, /*n_frames*/1,
                       chip_ram_path, sizeof chip_ram_path,
                       /*display_only*/0, /*interactive*/1) < 0)
        return 1;

    /* Boot the PC port via its single path: native disk boot. The harness owns
     * the display (side-by-side), so the PC port must not open its own window. */
    hw_request_headless();
    if (pc_init_from_disk(disks, n_disks) < 0) {
        fprintf(stderr, "[harness] PC disk boot failed\n");
        return 1;
    }
    pc_set_harness_mode(1);   /* harness drives stepping; skip host-rate pacing */

    /* PUAE reads joystick fire ($BFE001 bit7); configure both ports as joypads so
     * the injected fire reaches the game. */
    retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    retro_set_controller_port_device(1, RETRO_DEVICE_JOYPAD);

    int win_inited = 0;
    if (headed) { harness_combined_init(); win_inited = 1; harness_combined_present(); }

    int fire = 0;
    int ju = 0, jd = 0, jl = 0, jr = 0;   /* held joystick directions */
    #define STEP_PC() do { g_harness_compared_frame++; hw_set_joystick(ju, jd, jl, jr, fire); \
                           hw_set_mouse_lmb(fire); pc_step(); \
                           if (headed) harness_combined_present(); } while (0)
    #define STEP_PU() do { g_harness_compared_frame++; input_force_fire(fire); \
                           input_force_dir(ju, jd, jl, jr); \
                           hw_watchdog_arm("PUAE", 2); retro_run(); hw_watchdog_disarm(); \
                           if (headed) harness_combined_present(); } while (0)

    /* --play: skip the (stdin-blocking) REPL and go straight into the live loop,
     * so the window is event-pumped/responsive immediately with no typing. */
    if (play_mode) {
        printf("[harness] LIVE — focus the window. arrows=move, Z/Ctrl/Space=fire, ESC/close=quit\n");
        fflush(stdout);
        for (;;) {
            input_poll();                 /* keyboard -> PC + PUAE; exit() on close/ESC */
            pc_step();
            hw_watchdog_arm("PUAE", 2);
            retro_run();
            hw_watchdog_disarm();
            harness_combined_present();
            harness_interactive_delay(20);
        }
    }

    printf("[crepl] ready (headed=%d). cmds: play|headed|fire|joy|pc|pu|both|state|cmp|fb|m|mp|q\n", headed);
    fflush(stdout);

    char line[256];
    while (fgets(line, sizeof line, stdin)) {
        char cmd[16] = {0}; unsigned n = 0;
        if (sscanf(line, "%15s", cmd) < 1) continue;

        if (!strcmp(cmd, "q")) break;
        else if (!strcmp(cmd, "headed")) {
            int on = 1; sscanf(line, "%*s %d", &on);
            if (on && !win_inited) { harness_combined_init(); win_inited = 1; }
            headed = on;
            if (headed) harness_combined_present();   /* show current frame now */
            printf("[crepl] headed=%d\n", headed);
        }
        else if (!strcmp(cmd, "play")) {
            /* Live real-time drive: read the keyboard each frame and advance BOTH
             * cores, so you play it yourself (arrows = move, Z/Ctrl/Space = fire).
             * Close the window or press ESC to stop. Optional arg = ms/frame
             * (default 20 ≈ 50fps). */
            int ms = 20; sscanf(line, "%*s %d", &ms); if (ms < 1) ms = 20;
            if (!win_inited) { harness_combined_init(); win_inited = 1; }
            headed = 1;
            printf("[crepl] LIVE — focus the window. arrows=move, Z/Ctrl/Space=fire, ESC=quit\n");
            fflush(stdout);
            for (;;) {
                input_poll();                 /* keyboard -> PC (hw_set_joystick) + PUAE (s_in); exit() on close/ESC */
                pc_step();                    /* one PC frame with live input */
                hw_watchdog_arm("PUAE", 2);
                retro_run();                  /* one PUAE frame (reads the same live input) */
                hw_watchdog_disarm();
                harness_combined_present();
                harness_interactive_delay(ms);
            }
        }
        else if (!strcmp(cmd, "setlevel")) {  /* setlevel N — pre-apply $20.w for next $150 hand-off */
            int n = 1; sscanf(line, "%*s %d", &n);
            extern void pc_set_start_level(int);
            pc_set_start_level(n);
        }
        else if (!strcmp(cmd, "levelinfo")) {
            extern uint8_t *g_mem;
            extern void pc_level_split(int, int*, int*);
            extern const char *pc_world_name(int);
            extern const char *pc_current_level_name(void);
            extern int pc_is_level_card_displayed(void);
            int level = g_mem ? ((int)g_mem[0x20] << 8) | g_mem[0x21] : 0;
            int world = 0, liw = 0;
            pc_level_split(level, &world, &liw);
            char name[64] = {0}; const char *p = pc_current_level_name();
            int j = 0;
            for (int k = 0; k < 63 && p[k] && p[k] != '"'; k++) name[j++] = p[k];
            printf("[crepl] $20.w = %d  -> world %d (%s) / level_in_world %d  name=\"%s\"  card_displayed=%d\n",
                   level, world, pc_world_name(world), liw, name,
                   pc_is_level_card_displayed());
        }
        else if (!strcmp(cmd, "dumpall")) {  /* dumpall <file> — dump first 6MB of g_mem */
            char path[256] = "logs/gmem_runtime.bin";
            sscanf(line, "%*s %255s", path);
            extern uint8_t *g_mem;
            FILE *f = fopen(path, "wb"); if (f) { fwrite(g_mem, 1, 0x600000, f); fclose(f); }
            printf("[crepl] dumped 0..$600000 of g_mem -> %s\n", path);
        }
        else if (!strcmp(cmd, "fire")) {
            sscanf(line, "%*s %d", &fire); printf("[crepl] fire=%d\n", fire);
        }
        else if (!strcmp(cmd, "done")) {  /* debug: force PC level-complete (win) */
            extern void pc_debug_complete_level(void);
            pc_debug_complete_level();
            printf("[crepl] forced PC level-complete (win) flag\n");
        }
        else if (!strcmp(cmd, "gameover")) {  /* debug: force PC game-over (death) */
            extern void pc_debug_game_over(void);
            pc_debug_game_over();
            printf("[crepl] forced PC game-over flag\n");
        }
        else if (!strcmp(cmd, "joy")) {   /* joy <up> <down> <left> <right> (held until changed) */
            ju = jd = jl = jr = 0;
            sscanf(line, "%*s %d %d %d %d", &ju, &jd, &jl, &jr);
            printf("[crepl] joy u=%d d=%d l=%d r=%d\n", ju, jd, jl, jr);
        }
        else if (!strcmp(cmd, "pc")) {
            n = 1; sscanf(line, "%*s %u", &n); if (!n) n = 1;
            for (unsigned i = 0; i < n; i++) STEP_PC();
            FrameState c; hw_get_snap(&c);
            printf("[crepl] PC +%u -> cop1lc=$%06X\n", n, c.cop1lc);
        }
        else if (!strcmp(cmd, "runto")) {  /* runto <cop1lc-hex> [maxframes] — step PC (current fire/joy held) until cop1lc matches */
            unsigned target = 0, maxf = 5000;
            sscanf(line, "%*s %x %u", &target, &maxf); if (!maxf) maxf = 5000;
            FrameState c; unsigned i = 0;
            for (; i < maxf; i++) {
                STEP_PC();
                hw_get_snap(&c);
                if (c.cop1lc == target) break;
            }
            printf("[crepl] runto $%06X: %s after %u frames (cop1lc=$%06X)\n",
                   target, (c.cop1lc == target) ? "REACHED" : "gave up", i + (i < maxf), c.cop1lc);
        }
        else if (!strcmp(cmd, "pu")) {
            n = 1; sscanf(line, "%*s %u", &n); if (!n) n = 1;
            for (unsigned i = 0; i < n; i++) STEP_PU();
            FrameState p; puae_snap_state(&p);
            printf("[crepl] PU +%u -> cop1lc=$%06X\n", n, p.cop1lc);
        }
        else if (!strcmp(cmd, "both")) {
            n = 1; sscanf(line, "%*s %u", &n); if (!n) n = 1;
            for (unsigned i = 0; i < n; i++) { STEP_PC(); STEP_PU(); }
            FrameState p, c; puae_snap_state(&p); hw_get_snap(&c);
            printf("[crepl] both +%u -> PC=$%06X PU=$%06X\n", n, c.cop1lc, p.cop1lc);
        }
        else if (!strcmp(cmd, "mpw")) {   /* mpw <hexaddr> <hexbyte> — poke a PC g_mem byte */
            unsigned a = 0, v = 0; sscanf(line, "%*s %x %x", &a, &v);
            extern uint8_t *g_mem;
            g_mem[a & 0x7FFFFF] = (uint8_t)v;
            printf("[crepl] PC poke $%06X = %02X\n", a, (uint8_t)v);
        }
        else if (!strcmp(cmd, "m") || !strcmp(cmd, "mp")) {
            unsigned a = 0, k = 16; sscanf(line, "%*s %x %u", &a, &k); if (!k) k = 16;
            if (cmd[1] == 'p') {   /* PC g_mem */
                extern uint8_t *g_mem;
                printf("[crepl] PC  $%06X:", a);
                for (unsigned i = 0; i < k; i++) printf(" %02X", g_mem[(a + i) & 0x7FFFFF]);
                printf("\n");
            } else {               /* PUAE any bank (chip/fast/expansion) via CPU map */
                extern int puae_dump_mem(uint32_t addr, void *buf, int len);
                uint8_t pb[256]; if (k > 256) k = 256;
                puae_dump_mem(a, pb, (int)k);
                printf("[crepl] PU  $%06X:", a);
                for (unsigned i = 0; i < k; i++) printf(" %02X", pb[i]);
                printf("\n");
            }
        }
        else if (!strcmp(cmd, "state")) {
            FrameState p, c; puae_snap_state(&p); hw_get_snap(&c);
            extern volatile uint32_t g_rt_last_call, g_hw_last_read;
            printf("[crepl] PC cop1lc=$%06X  PU cop1lc=$%06X  fire=%d  "
                   "PC last_fn=$%06X last_read=$%06X\n",
                   c.cop1lc, p.cop1lc, fire,
                   (unsigned)g_rt_last_call, (unsigned)g_hw_last_read);
        }
        else if (!strcmp(cmd, "cmp")) {
            const uint32_t *pcfb = hw_get_framebuffer();
            int diff = 0;
            if (pcfb)
                for (int i = 0; i < FB_W * FB_H; i++)
                    if ((pcfb[i] & 0xFFFFFF) != (s_puae_fb[i] & 0xFFFFFF)) diff++;
            printf("[crepl] fb diff=%d/%d\n", diff, FB_W * FB_H);
        }
        else if (!strcmp(cmd, "fb")) {
            char tag[64] = {0};
            sscanf(line, "%*s %63s", tag);   /* optional: fb <tag> -> logs/fb_pc_<tag>.bin */
            char pcpath[128], pupath[128];
            if (tag[0]) {
                snprintf(pcpath, sizeof pcpath, "logs/fb_pc_%s.bin", tag);
                snprintf(pupath, sizeof pupath, "logs/fb_puae_%s.bin", tag);
            } else {
                snprintf(pcpath, sizeof pcpath, "logs/fb_pc.bin");
                snprintf(pupath, sizeof pupath, "logs/fb_puae.bin");
            }
            const uint32_t *pcfb = hw_get_framebuffer();
            FILE *q = fopen(pcpath, "wb");
            if (q && pcfb) { fwrite(pcfb, 4, FB_W * FB_H, q); fclose(q); }
            q = fopen(pupath, "wb");
            if (q) { fwrite(s_puae_fb, 4, FB_W * FB_H, q); fclose(q); }
            printf("[crepl] wrote %s + %s\n", pcpath, pupath);
        }
        else printf("[crepl] ? %s", line);
        fflush(stdout);
    }
    #undef STEP_PC
    #undef STEP_PU

    if (headed) harness_combined_fini();
    pc_fini();
    retro_unload_game();
    retro_deinit();
    return 0;
}
