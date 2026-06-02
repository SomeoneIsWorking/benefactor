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
#include "game_state.h"   /* g_state + legacy-name macros */
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
    extern void pc_pin_address_space(int, char **);
    pc_pin_address_space(argc, argv);

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
    int direct_level = 0;   /* 0 = full title boot; 1..60 = jump straight to that level */
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--headed")) headed = 1;
        else if (!strcmp(argv[i], "--play")) { play_mode = 1; headed = 1; }
        else if (!strcmp(argv[i], "--level") && i + 1 < argc) {
            direct_level = atoi(argv[++i]);
        }
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

    /* Boot the PC port. The harness owns the display (side-by-side), so the PC
     * port must not open its own window. Use the direct-to-gameplay path when
     * --level was supplied — skips intro/title/menu and enters at $577000. */
    hw_request_headless();
    int pc_init_rc = direct_level > 0
        ? pc_init_to_gameplay(disks, n_disks, direct_level)
        : pc_init_from_disk(disks, n_disks);
    if (pc_init_rc < 0) {
        fprintf(stderr, "[harness] PC %s failed\n",
                direct_level > 0 ? "direct-to-gameplay" : "disk boot");
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

    printf("[crepl] ready (headed=%d). cmds: play|headed|fire|joy|pc|pu|both|state|cmp|fb|m|mp|save|load|goto|pcread|pcreadclear|pcwatch|pcwatchclear|puwatch|puwatchclear|pufind|q\n", headed);
    printf("[crepl] play mode keys: S=save, D=load (logs/savestate.bin)\n");
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
        else if (!strcmp(cmd, "lsui")) {  /* toggle level-select overlay */
            extern int g_level_select_visible;
            g_level_select_visible = !g_level_select_visible;
            printf("[crepl] level-select overlay: %s\n", g_level_select_visible ? "ON" : "OFF");
        }
        else if (!strcmp(cmd, "setlevel")) {  /* setlevel N — pre-apply $20.w for next $150 hand-off */
            int n = 1; sscanf(line, "%*s %d", &n);
            extern void pc_set_start_level(int);
            pc_set_start_level(n);
        }
        else if (!strcmp(cmd, "runtomenu")) {  /* runtomenu [maxframes] — drive to the main menu */
            /* The main menu is the screen at cop1lc=$008302. The cover-art /
             * attract at $0086CC is a hard wait-for-fire gate, so HOLD fire to
             * advance intro -> menu. Because fire is held the whole way, the
             * first frame we see $008302 is the menu: the idle poster (which
             * may share this cop1lc) is only rested-on when NOT advancing, and
             * held fire never rests there. Stop per-frame the instant the menu
             * appears, then RELEASE fire (held fire would auto-select PLAY GAME)
             * and let the fade-in settle so the menu is fully drawn. */
            #define MENU_COP1LC 0x008302u
            unsigned maxf = 3000; sscanf(line, "%*s %u", &maxf);
            int saved_fire = fire;
            fire = 1;
            unsigned i = 0; int reached = 0;
            for (; i < maxf; i++) { STEP_PC(); if (hw_get_cop1lc() == MENU_COP1LC) { reached = 1; break; } }
            fire = 0;
            if (reached) for (int s = 0; s < 12 && hw_get_cop1lc() == MENU_COP1LC; s++) STEP_PC();
            fire = saved_fire;
            printf("[crepl] runtomenu: %s after %u frames (cop1lc=$%06X)\n",
                   reached ? "REACHED" : "gave up", i + reached, hw_get_cop1lc());
            #undef MENU_COP1LC
        }
        else if (!strcmp(cmd, "runtocard")) {  /* runtocard [maxframes] — step PC until TITLE CARD shows */
            extern int pc_is_title_card_displayed(void);
            unsigned maxf = 5000; sscanf(line, "%*s %u", &maxf);
            unsigned i = 0;
            for (; i < maxf; i++) { STEP_PC(); if (pc_is_title_card_displayed()) break; }
            FrameState c; hw_get_snap(&c);
            printf("[crepl] runtocard: %s after %u frames (cop1lc=$%06X)\n",
                   pc_is_title_card_displayed() ? "REACHED" : "gave up", i + (i < maxf), c.cop1lc);
        }
        else if (!strcmp(cmd, "lnames")) {
            /* Print the current world's level names in PLAY order, via the
             * single source of truth (pc_static_level_name — already applies
             * the storage->play-order name permutation). */
            extern uint8_t *g_mem;
            int level = g_mem ? (((int)g_mem[0x20] << 8) | g_mem[0x21]) : 1;
            if (level < 1 || level > PC_NUM_LEVELS) level = 1;
            int world = 0, liw = 0;
            pc_level_split(level, &world, &liw);
            printf("  world %d: \"%s\"\n", world, pc_world_name(world));
            for (int i = 0; i < pc_levels_in_world(world); i++) {
                int gl = pc_world_first_level(world) + i;
                printf("  L%-2d (liw %d): \"%s\"\n", gl, i, pc_static_level_name(gl));
            }
        }
        else if (!strcmp(cmd, "levelinfo")) {
            extern uint8_t *g_mem;
            extern int pc_is_level_card_displayed(void);
            int level = g_mem ? ((int)g_mem[0x20] << 8) | g_mem[0x21] : 0;
            int world = 0, liw = 0;
            pc_level_split(level, &world, &liw);
            printf("[crepl] $20.w = %d  -> world %d (%s) / level_in_world %d  name=\"%s\"  card_displayed=%d\n",
                   level, world, pc_world_name(world), liw, pc_static_level_name(level),
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
        else if (!strcmp(cmd, "goto")) {  /* goto <N> — restart PC coroutine at level N (1..60), bypassing title */
            int n = 0; sscanf(line, "%*s %d", &n);
            if (n < 1 || n > 60) {
                printf("[crepl] usage: goto <1..60>\n");
            } else {
                extern uint8_t *g_mem;
                /* Re-pin level + ensure overlay is loaded; trigger coroutine
                 * restart via the existing g_enter_gameplay path. */
                if (!g_overlay_active) {
                    extern void native_overlay_load_d0(void);
                    native_overlay_load_d0();
                    extern void pc_preload_all_level_names(void);
                    pc_preload_all_level_names();
                    for (uint32_t a = 0x3eu; ; a = 0x184u) {
                        g_mem[a]=0x00; g_mem[a+1]=0x00; g_mem[a+2]=0x0A; g_mem[a+3]=0x68;
                        if (a == 0x184u) break;
                    }
                }
                g_mem[0x20] = 0; g_mem[0x21] = (uint8_t)n;
                g_gameplay_entry = 0x577000u;
                g_enter_gameplay = 1;
                printf("[crepl] goto level %d — coroutine will restart at $577000 on next step\n", n);
            }
        }
        else if (!strcmp(cmd, "mpu")) {   /* mpu <hexaddr> <hexval> [w] — poke a PUAE byte/word */
            char av[32]={0}, vv[32]={0}, wf[8]={0};
            if (sscanf(line, "%*s %31s %31s %7s", av, vv, wf) >= 2) {
                unsigned a=(unsigned)strtoul(av,0,16), v=(unsigned)strtoul(vv,0,16);
                extern void puae_poke_mem(uint32_t,const void*,int);
                if (wf[0]=='w') { uint8_t b[2]={(uint8_t)(v>>8),(uint8_t)v}; puae_poke_mem(a,b,2); }
                else            { uint8_t b=(uint8_t)v; puae_poke_mem(a,&b,1); }
                printf("[crepl] poked PUAE $%06X = $%X\n", a, v);
            } else printf("[crepl] usage: mpu <hexaddr> <hexval> [w]\n");
        }
        else if (!strcmp(cmd, "pugoto")) {  /* pugoto <N> — drive PUAE through the intro/menu to gameplay
                                               at level N (1..60), poking $20.w at the engine entry $577000
                                               so we don't need a keyboard-typed password. The oracle. */
            int lvl = 1; sscanf(line, "%*s %d", &lvl);
            if (lvl < 1 || lvl > 60) { printf("[crepl] usage: pugoto <1..60>\n"); }
            else {
                extern int puae_run_to_pc(uint32_t,int,int);
                extern void puae_poke_mem(uint32_t,const void*,int);
                extern uint32_t puae_get_cop1lc(void);
                /* 1) fire-pulse through the intro/attract until the MENU ($008302). */
                int at_menu = 0;
                for (int pulse = 0; pulse < 40 && !at_menu; pulse++) {
                    fire = 1; for (int i=0;i<6;i++) STEP_PU();
                    fire = 0;
                    for (int i=0;i<110;i++) { STEP_PU();
                        if (puae_get_cop1lc() == 0x008302u) { at_menu = 1; break; } }
                }
                if (!at_menu) { printf("[crepl] pugoto: never reached menu ($008302)\n"); }
                else {
                    /* 2) PLAY GAME (default selection), then 3) stop at engine entry. */
                    fire = 1; for (int i=0;i<6;i++) STEP_PU();
                    fire = 0;
                    int hit = puae_run_to_pc(0x577000u, 0, 2000);
                    if (!hit) { printf("[crepl] pugoto: never reached engine entry $577000\n"); }
                    else {
                        /* 4) poke level word $20.w before level setup ($5782B4) reads it. */
                        uint8_t lw[2] = { (uint8_t)((lvl>>8)&0xFF), (uint8_t)(lvl&0xFF) };
                        puae_poke_mem(0x20u, lw, 2);
                        /* 5) run to the level CARD ($003914), then pulse fire until
                         *    gameplay ($003484) is STABLE (card dismissed). */
                        for (int i=0;i<600;i++) { STEP_PU(); if (puae_get_cop1lc()==0x003914u) break; }
                        int stable = 0;
                        for (int pulse=0; pulse<20 && stable<60; pulse++) {
                            if (puae_get_cop1lc()==0x003914u) {
                                fire=1; for(int i=0;i<8;i++) STEP_PU(); fire=0;
                            }
                            for (int i=0;i<30;i++) { STEP_PU();
                                if (puae_get_cop1lc()==0x003484u) stable++; else { stable=0; break; } }
                        }
                        uint8_t chk[2]; puae_dump_mem(0x20u, chk, 2);
                        printf("[crepl] pugoto %d: cop1lc=$%06X  $20.w=%u  (gameplay stable=%d)\n",
                               lvl, puae_get_cop1lc(), (chk[0]<<8)|chk[1], stable);
                    }
                }
            }
        }
        else if (!strcmp(cmd, "pufind")) {  /* pufind <hex> [max_frames] — step PUAE 1 frame at a time, watching when mem[hex..+3] first changes; reports the frame */
            extern int puae_dump_mem(uint32_t addr, void *buf, int len);
            unsigned a = 0, maxf = 5000;
            if (sscanf(line, "%*s %x %u", &a, &maxf) < 1) {
                printf("[crepl] usage: pufind <hex> [max_frames]\n");
            } else {
                uint8_t prev[4], cur[4];
                puae_dump_mem(a, prev, 4);
                printf("[crepl] pufind $%06X baseline = %02X %02X %02X %02X — stepping up to %u frames\n",
                       a, prev[0], prev[1], prev[2], prev[3], maxf);
                unsigned f;
                for (f = 0; f < maxf; f++) {
                    STEP_PU();
                    puae_dump_mem(a, cur, 4);
                    if (memcmp(prev, cur, 4) != 0) {
                        printf("[crepl] pufind: changed at frame +%u: %02X %02X %02X %02X -> %02X %02X %02X %02X\n",
                               f + 1, prev[0], prev[1], prev[2], prev[3], cur[0], cur[1], cur[2], cur[3]);
                        memcpy(prev, cur, 4);
                        /* Keep watching for additional changes — report each. */
                    }
                }
                printf("[crepl] pufind: done after %u frames\n", f);
            }
        }
        else if (!strcmp(cmd, "pcread")) {  /* pcread <hex>[-<hex>] — log PC reads (rt_read*) of chip-RAM addr/range */
            extern void rt_chip_rwatch_add(uint32_t lo, uint32_t hi);
            char arg[32] = {0};
            if (sscanf(line, "%*s %31s", arg) == 1) {
                unsigned lo = 0, hi = 0;
                const char *p = arg; if (*p == '$') p++;
                char *end = NULL;
                lo = (unsigned)strtoul(p, &end, 16);
                if (*end == '-') { p = end + 1; if (*p == '$') p++;
                                   hi = (unsigned)strtoul(p, &end, 16); }
                else hi = lo;
                rt_chip_rwatch_add(lo, hi);
            } else {
                printf("[crepl] usage: pcread <hex>[-<hex>]\n");
            }
        }
        else if (!strcmp(cmd, "pcreadclear")) {
            extern void rt_chip_rwatch_clear(void);
            rt_chip_rwatch_clear();
        }
        else if (!strcmp(cmd, "pcwatch")) {  /* pcwatch <hex>[-<hex>] — log PC writes (rt_write*) to chip-RAM addr/range */
            extern void rt_chip_watch_add(uint32_t lo, uint32_t hi);
            char arg[32] = {0};
            if (sscanf(line, "%*s %31s", arg) == 1) {
                unsigned lo = 0, hi = 0;
                const char *p = arg; if (*p == '$') p++;
                char *end = NULL;
                lo = (unsigned)strtoul(p, &end, 16);
                if (*end == '-') { p = end + 1; if (*p == '$') p++;
                                   hi = (unsigned)strtoul(p, &end, 16); }
                else hi = lo;
                rt_chip_watch_add(lo, hi);
            } else {
                printf("[crepl] usage: pcwatch <hex>[-<hex>]\n");
            }
        }
        else if (!strcmp(cmd, "pcwatchclear")) {
            extern void rt_chip_watch_clear(void);
            rt_chip_watch_clear();
        }
        else if (!strcmp(cmd, "puwatch")) {  /* puwatch <hex>[-<hex>] — log PUAE writes to chip-RAM addr/range */
            extern void puae_watch_chip_add(uint32_t lo, uint32_t hi);
            char arg[32] = {0};
            if (sscanf(line, "%*s %31s", arg) == 1) {
                unsigned lo = 0, hi = 0;
                const char *p = arg; if (*p == '$') p++;
                char *end = NULL;
                lo = (unsigned)strtoul(p, &end, 16);
                if (*end == '-') { p = end + 1; if (*p == '$') p++;
                                   hi = (unsigned)strtoul(p, &end, 16); }
                else hi = lo;
                puae_watch_chip_add(lo, hi);
            } else {
                printf("[crepl] usage: puwatch <hex>[-<hex>]\n");
            }
        }
        else if (!strcmp(cmd, "puwatchclear")) {
            extern void puae_watch_chip_clear(void);
            puae_watch_chip_clear();
        }
        else if (!strcmp(cmd, "save")) {  /* save [path] — dump PC coroutine state */
            char path[256] = "logs/savestate.bin";
            sscanf(line, "%*s %255s", path);
            extern int pc_savestate(const char *); pc_savestate(path);
        }
        else if (!strcmp(cmd, "load")) {  /* load [path] — restore PC coroutine state */
            char path[256] = "logs/savestate.bin";
            sscanf(line, "%*s %255s", path);
            extern int pc_loadstate(const char *); pc_loadstate(path);
        }
        else if (!strcmp(cmd, "loadmem")) {  /* loadmem [path] — load ONLY g_mem (chip/RAM) from a savestate,
                                                keeping the live game thread's own coroutine context. Lets the
                                                running engine continue into the saved scene's memory (camera,
                                                objects, tilemap) and re-render it — works across binaries since
                                                the Amiga memory map is binary-independent. Diagnostic only. */
            char path[256] = "logs/savestate.bin";
            sscanf(line, "%*s %255s", path);
            extern uint8_t *g_mem;
            FILE *f = fopen(path, "rb");
            if (!f) { printf("[crepl] loadmem: open %s failed\n", path); }
            else {
                fseek(f, 0, SEEK_END); long fsz = ftell(f);
                long mem_off = fsz - (long)RT_MEM_SIZE;
                long gs_off  = mem_off - (long)sizeof(GameState);
                if (gs_off < 0) { printf("[crepl] loadmem: %s too small (%ld)\n", path, fsz); }
                else {
                    /* Load the hardware-shadow + register state (g_state) AND chip/RAM
                     * (g_mem). We do NOT resume the parked game thread (its C-stack is
                     * stale), so this is render-only: after `render` the framebuffer
                     * shows the saved scene exactly. Skipping g_state was wrong — the
                     * renderer reads cop1lc/BPL pointers from g_state.regs[]. */
                    fseek(f, gs_off, SEEK_SET);
                    size_t gs = fread(&g_state, 1, sizeof(GameState), f);
                    size_t mm = fread(g_mem, 1, RT_MEM_SIZE, f);
                    printf("[crepl] loadmem: loaded g_state(%zu) + g_mem(%zu) from %s "
                           "(thread NOT resumed; use `render` then inspect)\n", gs, mm, path);
                }
                fclose(f);
            }
        }
        else if (!strcmp(cmd, "puloadmem")) {  /* puloadmem [path] — TELEPORT PUAE to a PC savestate's scene
                                                  by poking the game-state memory regions (chip $0..$80000 +
                                                  the a5 engine-state band) from the savestate's g_mem into
                                                  PUAE. PUAE is already in gameplay (a5=$57EE12), so its
                                                  per-frame loop re-reads this state and re-renders the scene
                                                  — giving the ORACLE's drawing of it. Run `pu N` after. */
            char path[256] = "logs/savestate.bin";
            sscanf(line, "%*s %255s", path);
            extern void puae_poke_mem(uint32_t,const void*,int);
            FILE *f = fopen(path, "rb");
            if (!f) { printf("[crepl] puloadmem: open %s failed\n", path); }
            else {
                fseek(f, 0, SEEK_END); long fsz = ftell(f);
                long mem_off = fsz - (long)RT_MEM_SIZE;
                if (mem_off < 0) { printf("[crepl] puloadmem: %s too small\n", path); }
                else {
                    /* Two regions that hold the chamber's live state. */
                    struct { uint32_t lo, hi; } regs[] = {
                        { 0x000000u, 0x080000u },  /* chip: low RAM, copper, bitplane bufs, object tables */
                        { 0x57D000u, 0x582000u },  /* a5-relative engine state band ($57EE12 +/-) */
                    };
                    static uint8_t buf[0x80000];
                    long total = 0;
                    for (unsigned r = 0; r < sizeof(regs)/sizeof(regs[0]); r++) {
                        uint32_t lo = regs[r].lo, hi = regs[r].hi, len = hi - lo;
                        fseek(f, mem_off + lo, SEEK_SET);
                        size_t got = fread(buf, 1, len, f);
                        puae_poke_mem(lo, buf, (int)got);
                        total += (long)got;
                    }
                    printf("[crepl] puloadmem: poked %ld bytes from %s into PUAE "
                           "(chip $0-$80000 + a5 band). Run `pu 2` then `fb`.\n", total, path);
                }
                fclose(f);
            }
        }
        else if (!strcmp(cmd, "render")) {  /* render — re-render s_fb straight from the current g_mem
                                               (copper + bitplanes) WITHOUT stepping the game thread. Use
                                               after `loadmem` to view a saved scene exactly as stored. */
            extern void hw_execute_copper(void);
            extern void native_render_frame(void);
            extern int g_native_render_delay;
            int saved_delay = g_native_render_delay;
            g_native_render_delay = 0;   /* read live g_mem, not the stale snapshot ring */
            hw_execute_copper();
            native_render_frame();
            g_native_render_delay = saved_delay;
            printf("[crepl] rendered current g_mem to framebuffer (delay bypassed)\n");
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
        else if (!strcmp(cmd, "pause")) {  /* toggle in-game pause-menu overlay */
            extern void pc_pause_toggle(void);
            pc_pause_toggle();
            printf("[crepl] toggled pause-menu overlay\n");
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
        else if (!strcmp(cmd, "audlc")) {  /* audlc — print PC + PUAE per-channel audio sample ptr (AUDxLC),
                                              len, vol, active. Diff a single jump to find the grunt channel. */
            printf("[audlc] PC ");
            for (int ch = 0; ch < 4; ch++) {
                int b = (0x0A0 + ch * 0x10) >> 1;
                uint32_t lc = ((uint32_t)s_regs[b] << 16) | s_regs[b + 1];
                printf("c%d=%06X/L%04X/V%02X%s ", ch, lc & 0xFFFFFF, s_regs[b + 2],
                       s_regs[b + 4] & 0x7F, s_audio[ch].active ? "*" : "");
            }
            FrameState ps; puae_snap_state(&ps);
            printf("| PU ");
            for (int ch = 0; ch < 4; ch++)
                printf("c%d=%06X/L%04X/V%02X ", ch, ps.audio[ch].lc & 0xFFFFFF,
                       ps.audio[ch].len, ps.audio[ch].vol);
            printf("\n");
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
