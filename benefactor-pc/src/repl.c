/*
 * repl.c  –  Interactive REPL for the Benefactor PC port
 *
 * Build & run:
 *   ./benefactor-pc --repl chip_ram_dump.bin Disk.1 Disk.2 Disk.3
 *
 * Commands:
 *   step [N]   – run N frames (default 1)
 *   dump A N   – dump N bytes at hex address A
 *   press      – send fire/LMB press for one frame
 *   release    - release fire/LMB
 *   reset-a4   – reset frame dispatch table to entry 0
 *   regs       – show current 68k register values
 *   cop        – show current COP1LC value
 *   quit       – exit
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <ctype.h>

#include "recomp/rt.h"
#include "recomp/hw.h"
#include "generated/game.h"
#include "engine.h"

static M68KCtx ctx;
static int s_repl_running = 1;

void repl_signal(int sig) { (void)sig; s_repl_running = 0; }

/* Expose input state from hw.c (declared in hw.h or extern) */
extern void hw_set_fire(int pressed);
extern void hw_set_mouse_lmb(int pressed);

static void cmd_step(char *arg)
{
    int n = 1;
    if (arg) n = atoi(arg);
    if (n < 1) n = 1;
    if (n > 10000) n = 10000;

    /* Temporarily disable SDL event handling so stdin commands work */
    fprintf(stderr, "[repl] stepping %d frame(s)...\n", n);

    /* The engine loop reads from dispatch table A4.
     * We need to run N frames of the game loop.
     * Re-use engine_run's logic but bypass SDL events. */
    for (int i = 0; i < n && s_repl_running && hw_running; i++) {
        /* Run one iteration of the game loop */

        /* Read dispatch entry */
        uint32_t a4_addr = 0;  /* we need to maintain this across calls */
        /* For now, just run a single native_00366A call if that's the current
         * frame function. Actually we need the dispatch table A4.
         * This is a partial implementation – see below. */

        /* We'll use an external persistent dispatch state (TBD) */
    }

    printf("ok\n");
}

static void cmd_dump(char *arg)
{
    if (!arg) { printf("usage: dump <hex_addr> [<len>]\n"); return; }
    char *end;
    unsigned long addr = strtoul(arg, &end, 16);
    int len = 64;
    if (*end == ' ' || *end == '\t')
        len = atoi(end + 1);
    if (len < 1) len = 1;
    if (len > 4096) len = 4096;

    addr &= 0xFFFFFF;
    for (int i = 0; i < len; i += 16) {
        printf("%06lX:", addr + i);
        for (int j = 0; j < 16 && i + j < len; j++) {
            uint8_t b = rt_read8(&ctx, (uint32_t)(addr + i + j));
            printf(" %02X", b);
        }
        printf("\n");
    }
}

static void cmd_press(char *arg)
{
    (void)arg;
    hw_set_fire(1);
    hw_set_mouse_lmb(1);
    printf("fire/LMB pressed\n");
}

static void cmd_release(char *arg)
{
    (void)arg;
    hw_set_fire(0);
    hw_set_mouse_lmb(0);
    printf("fire/LMB released\n");
}

static void cmd_regs(char *arg)
{
    (void)arg;
    printf("D0=%08X D1=%08X D2=%08X D3=%08X\n", ctx.D[0], ctx.D[1], ctx.D[2], ctx.D[3]);
    printf("D4=%08X D5=%08X D6=%08X D7=%08X\n", ctx.D[4], ctx.D[5], ctx.D[6], ctx.D[7]);
    printf("A0=%08X A1=%08X A2=%08X A3=%08X\n", ctx.A[0], ctx.A[1], ctx.A[2], ctx.A[3]);
    printf("A4=%08X A5=%08X A6=%08X A7=%08X\n", ctx.A[4], ctx.A[5], ctx.A[6], ctx.A[7]);
    printf("NZVC=%d%d%d%d\n", ctx.N, ctx.Z, ctx.V, ctx.C);
}

static void cmd_quit(char *arg)
{
    (void)arg;
    s_repl_running = 0;
    hw_running = 0;
    printf("bye\n");
}

typedef struct {
    const char *name;
    void (*fn)(char *arg);
    const char *help;
} ReplCmd;

static ReplCmd s_cmds[] = {
    {"step",    cmd_step,    "step [N]  – run N frames"},
    {"dump",    cmd_dump,    "dump A [N] – dump N bytes at hex address A"},
    {"press",   cmd_press,   "press     – simulate fire/LMB press"},
    {"release", cmd_release, "release   – release fire/LMB"},
    {"regs",    cmd_regs,    "regs      – show 68k registers"},
    {"quit",    cmd_quit,    "quit      – exit"},
    {NULL, NULL, NULL}
};

void repl_run(void)
{
    char line[256];

    signal(SIGINT, repl_signal);

    printf("Benefactor REPL – type 'help' for commands\n");

    while (s_repl_running && hw_running) {
        printf("> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) break;

        /* Remove trailing newline */
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        /* Skip empty lines */
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0') continue;

        /* Split command and args */
        char *cmd = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        char *arg = NULL;
        if (*p) {
            *p++ = '\0';
            while (*p && isspace((unsigned char)*p)) p++;
            if (*p) arg = p;
        }

        if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
            for (int i = 0; s_cmds[i].name; i++)
                printf("  %s\n", s_cmds[i].help);
            continue;
        }

        int found = 0;
        for (int i = 0; s_cmds[i].name; i++) {
            if (strcmp(cmd, s_cmds[i].name) == 0) {
                s_cmds[i].fn(arg);
                found = 1;
                break;
            }
        }

        if (!found)
            printf("unknown command '%s' (type 'help')\n", cmd);
    }
}
