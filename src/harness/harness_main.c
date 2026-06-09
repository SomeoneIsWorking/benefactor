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

#include "engine/hw.h"
#include "engine/hw_private.h"   /* BlitRec — for the `blits` REPL dump */
#include "render/native_renderer.h"  /* scene accessors — for `scenesdl` */
#include "render/scene_sdl.h"        /* scene_sdl_selftest */
#include "engine/rt.h"
#include "port/port.h"
#include "common/game_state.h"   /* g_state + legacy-name macros */
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

    /* Invocation matches the standalone PC port: disk files are POSITIONAL.
     *   benefactor-harness Disk.1 [Disk.2] [Disk.3] [flags]
     * PUAE (the reference oracle) restores a frozen save-state (logs/puae_sync.state),
     * so it does NOT need a real Kickstart ROM or WHDLoad path for normal runs — it
     * mounts an empty drive and relies entirely on the restored state. The --kick /
     * --whdload flags are only needed for BENEFACTOR_REFREEZE=1 (a fresh PUAE boot to
     * regenerate the sync state). This removes the old confusing positional
     * <kick> <whdload> <disk...> convention that silently consumed Disk.1/Disk.2. */
    const char *kick_dir     = "kickstart";   /* placeholder; unused unless REFREEZE */
    const char *whdload_path = "whdload/Benefactor";
    const char *disks[4]   = { NULL };
    int n_disks = 0;
    int headed  = 0;
    int play_mode = 0;
    int direct_level = 0;   /* 0 = full title boot; 1..60 = jump straight to that level */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headed")) headed = 1;
        else if (!strcmp(argv[i], "--play")) { play_mode = 1; headed = 1; }
        else if (!strcmp(argv[i], "--level") && i + 1 < argc) {
            direct_level = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--kick") && i + 1 < argc)    kick_dir = argv[++i];
        else if (!strcmp(argv[i], "--whdload") && i + 1 < argc) whdload_path = argv[++i];
        else if (n_disks < 3)             disks[n_disks++] = argv[i];
    }
    if (n_disks < 1) {
        fprintf(stderr,
            "Usage: %s <disk1> [disk2] [disk3] [--headed] [--play] [--level N]\n"
            "                          [--kick DIR] [--whdload PATH]\n"
            "\n"
            "Boots PUAE (reference, from logs/puae_sync.state) and the PC port\n"
            "(native disk boot) and drops into a REPL. Cores run INDEPENDENTLY.\n"
            "Disk files are positional, exactly like ./benefactor-pc.\n"
            "--kick/--whdload are only needed with BENEFACTOR_REFREEZE=1.\n"
            "\n"
            "REPL: fire 0|1 | pc [n] | pu [n] | both [n] | state | cmp | fb | q\n",
            argv[0]);
        return 1;
    }

    printf("[harness] === BENEFACTOR PC vs PUAE compare (no lockstep) ===\n");
    fflush(stdout);

    /* Boot PUAE (reference oracle) and leave it ready to step via retro_run().
     * DIAGNOSTIC: BENEFACTOR_SKIP_PUAE=1 skips the PUAE core entirely so the PC
     * boot can be observed in isolation (PUAE-dependent REPL cmds will crash). */
    char chip_ram_path[512] = "";
    if (!getenv("BENEFACTOR_SKIP_PUAE") &&
        run_puae_phase(kick_dir, whdload_path, /*boot_frames*/5000, /*n_frames*/1,
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
    { extern int g_pc_force_load_identity_mismatch;
      if (getenv("BENEFACTOR_FORCE_LOAD")) g_pc_force_load_identity_mismatch = 1; }

    /* PUAE reads joystick fire ($BFE001 bit7); configure both ports as joypads so
     * the injected fire reaches the game. */
    retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    retro_set_controller_port_device(1, RETRO_DEVICE_JOYPAD);

    int win_inited = 0;
    if (headed) { harness_combined_init(); win_inited = 1; harness_combined_present(); }

    int fire = 0, interact = 0;
    int ju = 0, jd = 0, jl = 0, jr = 0;   /* held joystick directions */
    #define STEP_PC() do { g_harness_compared_frame++; hw_set_joystick(ju, jd, jl, jr, fire); \
                           hw_set_mouse_lmb(fire); hw_set_interact(interact); pc_step(); \
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
        else if (!strcmp(cmd, "rungame")) {
            /* rungame — ONE reliable drive into controllable gameplay (level 1 via
             * PLAY GAME), from wherever we are (boot, menu, or the level card).
             * For an arbitrary level, launch with `--level N` (lands on the card)
             * and then `rungame` dismisses the card + waits out GET READY.
             *
             * The flow has three gates, each needing a specific input:
             *   menu  ($008302): hold fire to confirm PLAY GAME (cursor 0)
             *   card  ($003914): a fire EDGE (release→press) starts the level
             *   GET READY overlay on gameplay ($003484): clears on a timer
             * Each stage is detected by cop1lc, not a magic frame count, so it
             * stays correct if timings drift. */
            #define COP_MENU 0x008302u
            #define COP_CARD 0x003914u
            #define COP_PLAY 0x003484u
            int saved_fire = fire; unsigned i;
            /* 1. intro -> menu: the cover-art/attract is a hold-fire gate, so hold
             * fire to advance to the menu (skipped if --level put us past it). */
            if (hw_get_cop1lc() != COP_CARD && hw_get_cop1lc() != COP_PLAY) {
                fire = 1;
                for (i = 0; i < 3000 && hw_get_cop1lc() != COP_MENU; i++) STEP_PC();
                /* settle the menu fade-in with fire released */
                fire = 0; for (i = 0; i < 12; i++) STEP_PC();
            }
            /* 2. menu -> card: confirm PLAY GAME (cursor 0) by holding fire. */
            if (hw_get_cop1lc() == COP_MENU) {
                fire = 1;
                for (i = 0; i < 600 && hw_get_cop1lc() != COP_CARD; i++) STEP_PC();
                fire = 0;
            }
            /* (if --level landed us straight on the card, steps 1-2 are no-ops) */
            /* 3. card -> gameplay: a clean fire edge. */
            if (hw_get_cop1lc() == COP_CARD) {
                fire = 0; for (i = 0; i < 12; i++) STEP_PC();
                fire = 1;
                for (i = 0; i < 600 && hw_get_cop1lc() != COP_PLAY; i++) STEP_PC();
                fire = 0;
            }
            /* 4. wait out the GET READY banner so the player is controllable. */
            for (i = 0; i < 500; i++) STEP_PC();
            fire = saved_fire;
            printf("[crepl] rungame: cop1lc=$%06X %s\n", hw_get_cop1lc(),
                   hw_get_cop1lc() == COP_PLAY ? "(in gameplay)" : "(NOT in gameplay)");
            #undef COP_MENU
            #undef COP_CARD
            #undef COP_PLAY
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
        else if (!strcmp(cmd, "interact")) {   /* interact [0|1] — hold the dedicated interact key (pickup/lever) */
            sscanf(line, "%*s %d", &interact); printf("[crepl] interact=%d\n", interact);
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
            extern void native_render_frame(void);
            extern int g_native_render_delay;
            int saved_delay = g_native_render_delay;
            g_native_render_delay = 0;   /* read live g_mem, not the stale snapshot ring */
            native_render_frame();
            g_native_render_delay = saved_delay;
            printf("[crepl] rendered current g_mem to framebuffer (delay bypassed)\n");
        }
        else if (!strcmp(cmd, "wsmc") || !strcmp(cmd, "wsstatic")) {
            /* wsstatic — static-placement OBJECTS (caged Marry Men + level sprites):
             * how many descriptors the renderer scanned/drew from the object-only queue
             * $5A39EC last frame, + the displayed bp0 and the first descriptor's dst. */
            extern int native_wsstatic_drawn(void);
            extern int native_wsstatic_scanned(void);
            extern int native_wsstatic_cached(void);
            extern uint32_t native_wsstatic_dbg_bp0(void);
            extern uint32_t native_wsstatic_dbg_first(void);
            printf("[wsstatic] queue $5A39EC scanned=%d drawn=%d  marrymen(from-records,live)=%d  bp0=$%06X firstdst=$%06X\n",
                   native_wsstatic_scanned(), native_wsstatic_drawn(), native_wsstatic_cached(),
                   native_wsstatic_dbg_bp0(), native_wsstatic_dbg_first());
        }
        else if (!strcmp(cmd, "cfg")) {
            /* cfg               — list declared knobs + resolved value/source
             * cfg <key>         — show one
             * cfg <key> <value> — set a REPL/session override (ENV still wins; outranks JSON)
             * cfg <key> -       — clear the REPL override (fall back to JSON/default) */
            extern int  pc_cfg_show(const char *, char *, int, const char **);
            extern void pc_cfg_set(const char *, const char *);
            extern int  pc_cfg_count(void);
            extern const char *pc_cfg_key(int), *pc_cfg_desc(int);
            char key[48] = {0}, val[48] = {0};
            int got = sscanf(line, "%*s %47s %47s", key, val);
            if (got < 1) {
                printf("[cfg] precedence: ENV > REPL > JSON > default. knobs:\n");
                for (int i = 0; i < pc_cfg_count(); i++) {
                    char v[64]; const char *src = "default";
                    pc_cfg_show(pc_cfg_key(i), v, sizeof v, &src);
                    printf("  %-18s = %-10s [%s]   %s\n",
                           pc_cfg_key(i), v[0]?v:"(unset)", src, pc_cfg_desc(i));
                }
            } else {
                if (got >= 2) pc_cfg_set(key, strcmp(val, "-") ? val : NULL);
                char v[64]; const char *src = "default";
                pc_cfg_show(key, v, sizeof v, &src);
                printf("[cfg] %s = %s [%s]\n", key, v[0]?v:"(unset)", src);
            }
        }
        else if (!strcmp(cmd, "tp")) {
            /* tp <x> [y] — TELEPORT the player to world (x[,y]). Player block is
             * $10A6(a5)=$57FEB8 (worldX), $57FEBA (worldY), big-endian. Lets us drive
             * the game into arbitrary states (reach a key, a marry man, etc.) instead
             * of blind joystick navigation. */
            extern uint8_t *g_mem;
            long x = -999999, y = -999999;
            { const char *p = line; while (*p && *p!=' ') p++; sscanf(p, " %ld %ld", &x, &y); }
            if (!g_mem) { printf("[crepl] tp: no g_mem\n"); }
            else {
                if (x != -999999) { g_mem[0x57FEB8] = (uint8_t)((x>>8)&0xFF); g_mem[0x57FEB9] = (uint8_t)(x&0xFF); }
                if (y != -999999) { g_mem[0x57FEBA] = (uint8_t)((y>>8)&0xFF); g_mem[0x57FEBB] = (uint8_t)(y&0xFF); }
                int nx = (int16_t)(uint16_t)(((uint16_t)g_mem[0x57FEB8]<<8)|g_mem[0x57FEB9]);
                int ny = (int16_t)(uint16_t)(((uint16_t)g_mem[0x57FEBA]<<8)|g_mem[0x57FEBB]);
                printf("[crepl] tp player -> worldX=%d worldY=%d\n", nx, ny);
            }
        }
        else if (!strcmp(cmd, "wsmm")) {
            /* wsmm — dump each captured Marry Man (build-entry capture, $57C13A handler):
             * worldX, worldY, the post-sub-handler animation FRAME (d3), facing flags (d4),
             * variant. Watch FRAME across several `pc`/`s` steps to confirm animation. */
            extern int native_wsbuild_count(void);
            extern int native_wsbuild_get(int, int*, int*, int*, int*, int*);
            extern int g_ws_view_left, g_ws_view_w;
            extern uint32_t native_wsbuild_handler(int);
            int n = native_wsbuild_count();
            printf("[wsmm] %d marry man record(s)  view_left=%d view_w=%d:\n",
                   n, g_ws_view_left, g_ws_view_w);
            for (int k = 0; k < n; k++) {
                int x, y, fr, fl, bl;
                if (!native_wsbuild_get(k, &x, &y, &fr, &fl, &bl)) continue;
                uint32_t h = native_wsbuild_handler(k);
                printf("  [%d] worldX=%d (screenX=%d) worldY=%d frame=%d facing(d4)=$%04X(bit1=%d bit0=%d) hdlr=$%06X %s%s\n",
                       k, x, x - g_ws_view_left, y, fr, fl & 0xFFFF, (fl >> 1) & 1, fl & 1, h,
                       bl ? "BLIND" : "red", h == 0x57C13Au ? " (idle)" : " (active)");
                /* re-derived gfx EXACTLY as native_wsstatic_compose does (frame2=fr+($55 if !bit1);
                 * e=$4a72(a5)+frame2*8; data=r16(e)+$EEFA(+$4C38 blind)) — compare to `blits` src. */
                { extern uint8_t *g_mem;
                  if (g_mem) {
                    uint32_t a5=0x57EE12u, gtab=a5+0x4A72u;
                    int frame2 = fr + ((fl & 2) ? 0 : 0x55);
                    uint32_t e = gtab + (uint32_t)frame2*8u;
                    uint32_t bd = bl ? 0x4C38u : 0u;
                    #define R16(a) (((uint32_t)g_mem[(a)]<<8)|g_mem[(a)+1])
                    uint32_t rdata=(R16(e)+0xEEFAu+bd)&0xFFFFFFu, rmask=(R16(e+2)+0x12E7Eu+bd)&0xFFFFFFu;
                    uint16_t bsz=R16(e+6);
                    printf("        re-derived: frame2=%d data=$%06X mask=$%06X bltsize=$%04X (w=%d h=%d)\n",
                           frame2, rdata, rmask, bsz, bsz&0x3F, bsz>>6);
                    #undef R16
                  } }
            }
        }
        else if (!strcmp(cmd, "blits")) {  /* blits — dump the engine's captured object blits (BlitRec):
                                              src/mask/dpt/w/h. The MM's REAL gfx when on-page lives here. */
            extern int hw_blit_capture_count(void);
            extern const BlitRec *hw_blit_capture_recs(void);
            int bn = hw_blit_capture_count(); const BlitRec *br = hw_blit_capture_recs();
            printf("[blits] %d blit record(s):\n", bn);
            for (int i = 0; i < bn; i++)
                printf("  b%2d src=$%06X mask=$%06X dpt=$%06X w=%d h=%d shift=%d con0=$%04X\n",
                       i, br[i].src, br[i].mask, br[i].dpt, br[i].w, br[i].h, br[i].shift, br[i].con0);
        }
        else if (!strcmp(cmd, "blitskip")) {  /* blitskip <fn-hex|0> — DIAGNOSTIC: drop every blit issued
                                                 by routine <fn> (g_rt_last_call), to confirm which fn draws
                                                 a sprite by watching it vanish. 0 = disable. */
            extern uint32_t g_blit_skip_fn;
            unsigned f = 0; const char *p = line; while (*p && *p!=' ') p++; sscanf(p, " %x", &f);
            g_blit_skip_fn = f;
            printf("[crepl] blitskip fn=$%06X %s\n", f, f?"(active)":"(off)");
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
        else if (!strcmp(cmd, "wscmp")) {  /* wscmp [frames] — per-frame diff native(s_out) vs vanilla(s_fb)
                                              while scrolling right then left. Needs BENEFACTOR_WS_CMP=1. */
            extern const uint32_t *hw_get_framebuffer(void);
            extern const uint32_t *hw_get_output_framebuffer(void);
            unsigned frames = 500; sscanf(line, "%*s %u", &frames);
            int ow2 = hw_output_width();
            long total = 0; int worst = 0, worstf = -1, firstbad = -1, badframes = 0;
            static uint32_t wv[FB_W * FB_H], wn[FB_W * FB_H];   /* worst-frame snapshots */
            for (unsigned f = 0; f < frames; f++) {
                unsigned ph = f % 300;                 /* right 100, left 100, still 100 */
                ju = jd = jl = jr = 0;
                if (ph < 100) jr = 1; else if (ph < 200) jl = 1;
                STEP_PC();
                const uint32_t *van = hw_get_framebuffer();
                const uint32_t *nat = hw_get_output_framebuffer();
                int nd = 0, bx0 = 9999, by0 = 9999, bx1 = -1, by1 = -1;
                for (int y = 13; y < 237; y++)
                    for (int x = 24; x < 328; x++)
                        if ((van[y * FB_W + x] & 0xFFFFFF) != (nat[y * ow2 + x] & 0xFFFFFF)) {
                            nd++;
                            if (x < bx0) bx0 = x; if (x > bx1) bx1 = x;
                            if (y < by0) by0 = y; if (y > by1) by1 = y;
                        }
                total += nd;
                if (nd > 0) { badframes++; if (firstbad < 0) firstbad = (int)f; }
                if (nd > worst) {
                    worst = nd; worstf = (int)f;
                    memcpy(wv, van, sizeof wv);
                    for (int y = 0; y < FB_H; y++) memcpy(wn + y*FB_W, nat + y*ow2, FB_W*4);
                }
                /* CAUSE: for the first few bad frames, print where + sample pixels (van vs nat). */
                if (nd > 0 && badframes <= 6) {
                    printf("[wscmp] frame %u cam=$%04X: %d px diff, bbox x[%d..%d] y[%d..%d]; samples:",
                           f, (g_mem[0x57FDBA] << 8) | g_mem[0x57FDBB], nd, bx0, bx1, by0, by1);
                    int shown = 0;
                    for (int y = 13; y < 237 && shown < 6; y++)
                        for (int x = 24; x < 328 && shown < 6; x++) {
                            uint32_t v = van[y*FB_W+x]&0xFFFFFF, q = nat[y*ow2+x]&0xFFFFFF;
                            if (v != q) { printf(" (%d,%d van=%06X nat=%06X)", x, y, v, q); shown++; }
                        }
                    printf("\n");
                }
            }
            ju = jd = jl = jr = 0;
            if (worstf >= 0) {
                FILE *a = fopen("logs/wscmp_van.bin","wb"); if (a){fwrite(wv,4,FB_W*FB_H,a);fclose(a);}
                FILE *b = fopen("logs/wscmp_nat.bin","wb"); if (b){fwrite(wn,4,FB_W*FB_H,b);fclose(b);}
                printf("[wscmp] wrote worst frame %d -> logs/wscmp_{van,nat}.bin (%dx%d)\n", worstf, FB_W, FB_H);
            }
            printf("[wscmp] %u frames: total=%ld badframes=%d worst=%d@%d firstbad=%d\n",
                   frames, total, badframes, worst, worstf, firstbad);
        }
        else if (!strcmp(cmd, "wsdiff")) {  /* wsdiff [maxframes] — step frame-by-frame with the CURRENT
                                               held input (fire/joy), comparing native@352 vs vanilla each
                                               frame, and STOP at the first frame that diverges. Reports the
                                               bbox + sample pixels and dumps that frame to logs/wsdiff_{van,nat}.bin
                                               so the exact divergence can be root-caused. Needs BENEFACTOR_WS_CMP=1. */
            extern const uint32_t *hw_get_framebuffer(void);
            extern const uint32_t *hw_get_output_framebuffer(void);
            unsigned maxf = 1500; sscanf(line, "%*s %u", &maxf);
            int ow2 = hw_output_width();
            int hit = -1;
            unsigned thresh = 0; sscanf(line, "%*s %*u %u", &thresh);  /* optional min-px to count as diverge */
            for (unsigned f = 0; f < maxf; f++) {
                STEP_PC();
                const uint32_t *van = hw_get_framebuffer();
                const uint32_t *nat = hw_get_output_framebuffer();
                /* Restrict to vanilla's DISPLAYED content window: columns that are all-black
                 * across the playfield are the DIW border (or the widescreen extension) — there
                 * is no vanilla ground truth there, so comparing them is meaningless (the check
                 * would just flag the native extension). Only compare columns vanilla draws. */
                int colhas[FB_W];
                for (int x = 0; x < FB_W; x++) {
                    colhas[x] = 0;
                    for (int y = 13; y < 201; y++)
                        if ((van[y*FB_W+x] & 0xFFFFFF) != 0) { colhas[x] = 1; break; }
                }
                /* Skip an edge band: the native renderer has a known camera-alignment
                 * bug toward the left/right edges (off by a few px), which produces
                 * spurious diffs there. WSDIFF_EDGE px are excluded from EACH side of
                 * the compared content window so the check reflects the real interior. */
                static int wsedge = -1;
                if (wsedge < 0) { const char *e = getenv("WSDIFF_EDGE"); wsedge = e ? atoi(e) : 32; }
                int nd = 0, bx0 = 9999, by0 = 9999, bx1 = -1, by1 = -1;
                for (int y = 13; y < 237; y++)
                    for (int x = 24 + wsedge; x < 328 - wsedge; x++) {
                        if (!colhas[x]) continue;
                        if ((van[y*FB_W+x] & 0xFFFFFF) != (nat[y*ow2+x] & 0xFFFFFF)) {
                            nd++;
                            if (x<bx0)bx0=x; if (x>bx1)bx1=x; if (y<by0)by0=y; if (y>by1)by1=y;
                        }
                    }
                if ((unsigned)nd <= thresh) nd = 0;
                if (nd > 0) {
                    hit = (int)f;
                    printf("[wsdiff] FIRST DIVERGE at frame %u: %d px, bbox x[%d..%d] y[%d..%d] cam=$%04X\n",
                           f, nd, bx0, bx1, by0, by1, (g_mem[0x57FDBA]<<8)|g_mem[0x57FDBB]);
                    int shown = 0;
                    for (int y = by0; y <= by1 && shown < 8; y++)
                        for (int x = bx0; x <= bx1 && shown < 8; x++) {
                            uint32_t v=van[y*FB_W+x]&0xFFFFFF, q=nat[y*ow2+x]&0xFFFFFF;
                            if (v!=q){ printf("   (%d,%d) van=%06X nat=%06X\n", x,y,v,q); shown++; }
                        }
                    FILE *a=fopen("logs/wsdiff_van.bin","wb");
                    if(a){ for(int y=0;y<FB_H;y++)fwrite(van+y*FB_W,4,FB_W,a); fclose(a);}
                    FILE *b=fopen("logs/wsdiff_nat.bin","wb");
                    if(b){ for(int y=0;y<FB_H;y++)fwrite(nat+y*ow2,4,FB_W,b); fclose(b);}
                    printf("[wsdiff] dumped diverging frame -> logs/wsdiff_{van,nat}.bin\n");
                    break;
                }
            }
            if (hit < 0) printf("[wsdiff] no divergence in %u frames (all match)\n", maxf);
        }
        else if (!strcmp(cmd, "bannercmp")) {  /* bannercmp [x0 y0 x1 y1] — diff native(s_out) vs
                                                  vanilla(s_fb) in a REGION (default = the GET READY
                                                  banner box), reporting per-pixel mismatches with
                                                  van/nat colours, to drive the banner to 100% match.
                                                  Needs BENEFACTOR_WS_CMP=1 (native renders at 352). */
            extern const uint32_t *hw_get_framebuffer(void);
            extern const uint32_t *hw_get_output_framebuffer(void);
            int x0=44, y0=88, x1=296, y1=140;
            sscanf(line, "%*s %d %d %d %d", &x0, &y0, &x1, &y1);
            int ow2 = hw_output_width();
            const uint32_t *van = hw_get_framebuffer();
            const uint32_t *nat = hw_get_output_framebuffer();
            int nd = 0, tot = 0, shown = 0;
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) {
                    tot++;
                    uint32_t v = van[y*FB_W+x]&0xFFFFFF, q = nat[y*ow2+x]&0xFFFFFF;
                    if (v != q) {
                        nd++;
                        if (shown < 16) { printf("   (%d,%d) van=%06X nat=%06X\n", x, y, v, q); shown++; }
                    }
                }
            printf("[bannercmp] region x[%d..%d] y[%d..%d]: %d/%d px differ (%.2f%% match)\n",
                   x0, x1, y0, y1, nd, tot, 100.0 * (tot - nd) / (tot ? tot : 1));
        }
        else if (!strcmp(cmd, "scenesdl")) {  /* scenesdl — P2 gate: verify the per-sprite SDL
                                                 consumer reproduces the CPU rasterizer on the
                                                 current BenRen scene. Run after `pc 1` with
                                                 BENEFACTOR_RENDERER=benren (or WS_CMP=1). */
            const Scene *sc = native_render_scene();
            int lo, hi, w, h;
            native_render_scene_yrange(&lo, &hi);
            native_render_scene_dims(&w, &h);
            if (!sc || sc->nquads == 0 || hi <= lo) {
                printf("[scenesdl] empty scene — run `pc 1` in benren mode first "
                       "(BENEFACTOR_RENDERER=benren)\n");
            } else {
                int mc = 0;
                long nd = scene_sdl_selftest(sc, w, h, lo, hi, &mc);
                if (nd < 0)
                    printf("[scenesdl] SDL error (software renderer unavailable?)\n");
                else
                    printf("[scenesdl] %d quads, rows[%d..%d], %dx%d: %ld px differ vs CPU "
                           "rasterizer (max channel diff %d) -> %s\n",
                           sc->nquads, lo, hi, w, h, nd, mc,
                           nd == 0 ? "BYTE-IDENTICAL" : "MISMATCH");
            }
        }
        else if (!strcmp(cmd, "blitlog")) {  /* blitlog [minw] — dump EVERY blit of the last stepped frame
                                                with its capture decision (REPL replacement for BLIT_LOG).
                                                reason: C=captured u=dest-off p=not-in-pages g=gfx-too-high
                                                o=cap-off f=full. minw filters by width-in-words. */
            typedef struct { uint32_t apt,bpt,cpt,dpt; uint16_t con0,con1; int w,h; char reason; } Lr;
            extern int hw_blitlog_count(void); extern const Lr *hw_blitlog_recs(void);
            unsigned minw = 0; sscanf(line, "%*s %u", &minw);
            const Lr *L = hw_blitlog_recs(); int nL = hw_blitlog_count();
            printf("[blitlog] %d blits last frame (minw=%u)\n", nL, minw);
            for (int k = 0; k < nL; k++) {
                if ((unsigned)L[k].w < minw) continue;
                printf("  con0=%04X con1=%04X %dx%d apt=%06X bpt=%06X cpt=%06X dpt=%06X cap=%c\n",
                       L[k].con0, L[k].con1, L[k].w, L[k].h, L[k].apt, L[k].bpt, L[k].cpt, L[k].dpt, L[k].reason);
            }
        }
        else if (!strcmp(cmd, "wscap")) {  /* wscap — widescreen capture state: object/char counts + player */
            extern int native_wsobj_count(void);
            extern int native_wschar_count(void);
            extern int native_wsplayer_get(int*,int*,uint32_t*,uint32_t*,int*);
            int px, py, pblack; uint32_t pdb, pmb;
            int have_p = native_wsplayer_get(&px, &py, &pdb, &pmb, &pblack);
            printf("[wscap] cop1lc=$%06X  wsobj=%d wschar=%d  player=%s",
                   hw_get_cop1lc(), native_wsobj_count(), native_wschar_count(),
                   have_p ? "yes" : "no");
            if (have_p) printf(" (x=%d y=%d black=%d)", px, py, pblack);
            { extern void native_ws_diag(long*,long*,long*); long ow=0,od=0,ch=0;
              native_ws_diag(&ow,&od,&ch);
              printf("  [diag objwalk=%ld objdraw=%ld char=%ld]", ow, od, ch); }
            printf("\n");
        }
        else if (!strcmp(cmd, "wsobjs")) {  /* wsobjs — dump captured widescreen object/char lists
                                               (worldX/Y, w, h, src) + screenX, to track culling. */
            extern uint8_t *g_mem;
            extern int native_wsobj_count(void);
            extern int native_wsobj_get(int,int*,int*,int*,int*,uint32_t*,uint32_t*);
            extern int native_wschar_count(void);
            extern int native_wschar_get(int,int*,int*,int*,int*,uint32_t*,uint32_t*,int*);
            int cam = g_mem ? (int)(int16_t)(((uint16_t)g_mem[0x57FDBA]<<8)|g_mem[0x57FDBB]) : 0;
            { extern int g_ws_view_left, g_ws_view_w;
              printf("[wsobjs] view_left=%d view_w=%d (worldX=view_left+screenx)\n", g_ws_view_left, g_ws_view_w); }
            int nobj = native_wsobj_count(), nchr = native_wschar_count();
            printf("[wsobjs] cam=%d  %d objs:\n", cam, nobj);
            for (int i=0;i<nobj;i++){ int x,y,w,h; uint32_t s,m;
                if (!native_wsobj_get(i,&x,&y,&w,&h,&s,&m)) continue;
                printf("  obj%2d x=%5d y=%4d screenX=%5d w=%d h=%d src=$%06X\n",i,x,y,x-cam,w*16,h,s); }
            printf("[wsobjs] %d chars:\n", nchr);
            for (int i=0;i<nchr;i++){ int x,y,w,h,rsd; uint32_t d,mk;
                if (!native_wschar_get(i,&x,&y,&w,&h,&d,&mk,&rsd)) continue;
                printf("  chr%2d x=%5d y=%4d screenX=%5d w=%d h=%d data=$%06X\n",i,x,y,x-cam,w,h,d); }
        }
        else if (!strcmp(cmd, "wswalk")) {  /* wswalk — DIAGNOSTIC: walk the $1162(a5) object-ptr list
                                               read-only and classify each object's walker branch
                                               (multitile $57D81C / animated $57D8B4 / main $57D804 / bit6
                                               fast-path) and whether it PASSes or is CULLed by that branch's
                                               camera window. (Note: the Marry Man is NOT a walker-cull case —
                                               he's an uncaptured char drawn by executor $57D6C4; see
                                               remaining-issues #1. This inspects the list-A/B walker only.) */
            extern uint8_t *g_mem;
            if (!g_mem) { printf("[wswalk] no g_mem\n"); }
            else {
              #define RD16(A) ((uint16_t)((g_mem[(A)&0x7FFFFF]<<8)|g_mem[((A)+1)&0x7FFFFF]))
              #define RD32(A) ((uint32_t)((RD16(A)<<16)|RD16((A)+2)))
              #define RD8(A)  (g_mem[(A)&0x7FFFFF])
              uint32_t a5 = 0x57EE12u;
              uint32_t a2 = a5 + 0x1162u;          /* object-ptr list */
              uint32_t a0 = a5 + 0x16a6u;          /* anim/aux table */
              int cam = (int)(int16_t)RD16(0x57FDBAu);
              printf("[wswalk] cam=%d list=$%06X\n", cam, a2);
              int idx = 0;
              while (RD32(a2) != 0 && idx < 64) {
                uint32_t a1 = RD32(a2); a2 += 4;
                uint16_t w0 = RD16(a1);
                const char *branch; int worldX=-1, d1=-1, win=-1, pass=-1; uint32_t handler=0;
                if ((int16_t)w0 < 0) {
                  /* multi-tile path $57D81C: worldX = $6(a0), cull cmpi.w #$150 */
                  branch = "MULTI";
                  worldX = (int16_t)RD16(a0 + 6u);
                  d1 = (uint16_t)(worldX - (uint16_t)cam);
                  win = 0x150; pass = (d1 <= win);
                } else {
                  uint32_t a1b = a1 + (int16_t)w0;   /* adda.w d2,a1 */
                  uint32_t a0loc = a0;
                  uint16_t aux = RD16(a0loc); a0loc += 2;  /* tst.w (a0)+ */
                  if (aux == 0) { branch = "ZERO-AUX"; }
                  else {
                    uint16_t animn = (uint16_t)(0x3F & RD16(a1b - 12u));
                    if (animn != 0) {
                      branch = "ANIM($57D8B4)";
                      handler = a1b;
                      if ((int16_t)RD16(a1b - 2u) >= 0) { pass = 1; win=-1; }
                      else { worldX = (int16_t)RD16(a0loc);
                             d1 = (uint16_t)(worldX + 0x30 - (uint16_t)cam); win=0x1b0; pass=(d1<=win); }
                    } else if (RD8(a1b - 2u) & (1u<<6)) {
                      branch = "BIT6-FAST"; handler=a1b; pass=1; win=-1;
                    } else {
                      branch = "MAIN($57D804)"; handler=a1b;
                      worldX = (int16_t)RD16(a0loc);
                      d1 = (uint16_t)(worldX + 0x30 - (uint16_t)cam); win=0x170; pass=(d1<=win);
                    }
                  }
                }
                printf("  [%2d] a1=$%06X w0=$%04X %-14s worldX=%5d screenX=%5d d1=%5d win=$%X %s hdlr=$%06X\n",
                       idx, a1, w0, branch, worldX, worldX<0?0:worldX-cam, d1, win<0?0:win,
                       pass<0?"-":(pass?"PASS":"CULL"), handler);
                a0 += (((int16_t)w0 < 0) ? 0x1c : 0x20);  /* multi-tile advances $1c, else $20 */
                idx++;
              }
              printf("[wswalk] %d objects\n", idx);
              #undef RD16
              #undef RD32
              #undef RD8
            }
        }
        else if (!strcmp(cmd, "bpos")) {  /* bpos — print banner box/anim/text captured page offsets +
                                             derived box-relative (row,col px) placement, vs the box mask's
                                             right-circle hole, to align the teleport anim/text. */
            extern uint8_t *g_mem;
            extern int native_wsbanner_get(int*,int*,uint32_t*,uint32_t*,int*,int*,int*,int*);
            extern int native_wstelanim_get(uint32_t*,int*,int*,int*);
            extern int native_wstext_get(uint32_t*,int*);
            int row,brel,ps,rs,ww,rows; uint32_t bdata,bmask;
            if (!native_wsbanner_get(&row,&brel,&bdata,&bmask,&ps,&rs,&ww,&rows)) {
                printf("[bpos] banner not active\n");
            } else {
                const int PRS = 46;
                int cam = (int)(int16_t)(((uint16_t)g_mem[0x57FDBA]<<8)|g_mem[0x57FDBB]);
                printf("[bpos] cam=%d  brel=%d  box row=%d  ww=%d(%dpx) rows=%d\n",
                       cam, brel, row, ww, ww*16, rows);
                uint32_t tsrc; int trel,tw,th;
                if (native_wstelanim_get(&tsrc,&trel,&tw,&th)) {
                    int d=trel-brel, dr=(d>=0)?d/PRS:-(((-d)+PRS-1)/PRS), dc=d-dr*PRS;
                    printf("[bpos] anim: trel=%d delta=%d -> row+%d col=%dpx (w=%dpx h=%d) src=$%X\n",
                           trel,d,dr,dc*8,tw*16,th,tsrc);
                }
                uint32_t str; int xrel;
                if (native_wstext_get(&str,&xrel)) {
                    int d=xrel-brel, dr=(d>=0)?d/PRS:-(((-d)+PRS-1)/PRS), dc=d-dr*PRS;
                    printf("[bpos] text: xrel=%d delta=%d -> row+%d col=%dpx str=$%X\n",
                           xrel,d,dr,dc*8,str);
                }
                /* right-circle hole bounds from the mask (cols 120..ww*16) */
                int hlo=9999,hhi=-1,rlo=9999,rhi=-1;
                for (int r=0;r<rows;r++) for (int c=120;c<ww*16;c++){
                    int wo=c>>4,bit=15-(c&15);
                    uint32_t a=bmask+(uint32_t)r*rs+(uint32_t)wo*2;
                    uint16_t v=((uint16_t)g_mem[a]<<8)|g_mem[a+1];
                    if(!((v>>bit)&1)){ if(c<hlo)hlo=c; if(c>hhi)hhi=c; if(r<rlo)rlo=r; if(r>rhi)rhi=r; }
                }
                printf("[bpos] right hole: cols %d..%d  rows %d..%d\n",hlo,hhi,rlo,rhi);
            }
        }
        else if (!strcmp(cmd, "pal")) {   /* pal — print the live 32-entry ARGB palette (g_state.palette) */
            printf("[crepl] palette (ARGB):\n");
            for (int i = 0; i < 32; i += 8) {
                printf("  %2d:", i);
                for (int j = i; j < i + 8; j++) printf(" %06X", g_state.palette[j] & 0xFFFFFF);
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
        else if (!strcmp(cmd, "sfxcmp")) {  /* sfxcmp [n] — step BOTH cores n frames with current
                                               fire/joy held; log every SFX trigger ($57fe4e 0->FF) on
                                               each side with its sample ptr ($57fe50). Diff the two
                                               grunt sequences to find where PC diverges from PUAE. */
            unsigned n = 200; sscanf(line, "%*s %u", &n); if (!n) n = 200;
            extern uint8_t *g_mem;
            extern int puae_dump_mem(uint32_t, void*, int);
            /* $57fe4e = SFX pending flag; $57fe78 = the descriptor's ORIGINAL sample
             * ptr (set at trigger, NOT stream-advanced) = a stable grunt id. Log the
             * base on each 0->FF trigger so the SETS of sounds can be cross-compared
             * even when the two cores are in different (un-synced) gameplay state. */
            uint8_t pcpend = g_mem[0x57FE4Eu], pupend; puae_dump_mem(0x57FE4E,&pupend,1);
            uint8_t pc_prev = pcpend, pu_prev = pupend;
            int pc_n=0, pu_n=0;
            for (unsigned i=0;i<n;i++) {
                STEP_PC(); STEP_PU();
                pcpend = g_mem[0x57FE4Eu]; puae_dump_mem(0x57FE4E,&pupend,1);
                uint8_t b[4];
                if (pcpend==0xFF && pc_prev!=0xFF) {
                    for (int k=0;k<4;k++) b[k]=g_mem[0x57FE78u + k];
                    uint32_t s=((uint32_t)b[1]<<16)|((uint32_t)b[2]<<8)|b[3];
                    printf("[sfxcmp] f+%-3u PC base %06X\n", i, s); pc_n++;
                }
                if (pupend==0xFF && pu_prev!=0xFF) {
                    puae_dump_mem(0x57FE78, b, 4);
                    uint32_t s=((uint32_t)b[1]<<16)|((uint32_t)b[2]<<8)|b[3];
                    printf("[sfxcmp] f+%-3u PU base %06X\n", i, s); pu_n++;
                }
                pc_prev=pcpend; pu_prev=pupend;
            }
            printf("[sfxcmp] done %u frames: PC triggers=%d PU triggers=%d\n", n, pc_n, pu_n);
        }
        else if (!strcmp(cmd, "mute")) {  /* mute [0|1] — toggle/set the music kill-switch (SFX isolation) */
            extern int g_mute_music;
            int v = -1;
            if (sscanf(line, "%*s %d", &v) == 1) g_mute_music = v; else g_mute_music = !g_mute_music;
            printf("[mute] music %s (hw frame=%d)\n", g_mute_music ? "OFF (muted)" : "ON",
                   hw_get_frame_num());
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
        else if (!strcmp(cmd, "fbw")) {   /* fbw [tag] — dump the WIDE output surface */
            extern int hw_output_width(void);
            extern const uint32_t *hw_get_output_framebuffer(void);
            char tag[64] = {0}; sscanf(line, "%*s %63s", tag);
            char path[160]; snprintf(path, sizeof path, "logs/fbw_%s.bin", tag[0]?tag:"out");
            int w = hw_output_width();
            FILE *q = fopen(path, "wb");
            if (q) { fwrite(hw_get_output_framebuffer(), 4, (size_t)w * FB_H, q); fclose(q); }
            printf("[crepl] wrote %s (%dx%d)\n", path, w, FB_H);
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
