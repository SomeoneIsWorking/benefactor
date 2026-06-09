/* dump_banks.c — standalone recompiler bank-input dumper.
 *
 * Produces the three recompiler bank inputs from the Disk.* images alone (no
 * PUAE, Kickstart, WHDLoad or slave), using ONLY the pure overlay loaders
 * (overlay_load.c) + disk_boot.c. It deliberately links WITHOUT the generated
 * game code, so it can run from a fresh checkout where src/generated/ is absent
 * — the bootstrap for regenerating the recompiled code from the user's disks.
 *
 *   dump_banks <out_dir> <Disk.1> [Disk.2] [Disk.3]
 *
 * Writes <out_dir>/{chip_ram_dump.bin, chip_flow_gp.bin, gmem_after_load.bin}.
 * Each bank is produced from a fresh boot decrunch so the overlay loads see
 * clean $6D734 block-copy source (mirrors the real boot -> overlay sequence).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "engine/disk_boot.h"
#include "engine/overlay_load.h"

#define MEM_SIZE (8u * 1024u * 1024u)

/* The loaders + disk_boot reference this global; we own it here (no rt.c, so no
 * generated-code dependency). */
uint8_t *g_mem = NULL;

static int dump(const char *dir, const char *name, uint32_t len)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[dump_banks] cannot open %s\n", path); return -1; }
    size_t n = fwrite(g_mem, 1, len, f);
    fclose(f);
    fprintf(stderr, "[dump_banks] wrote %s (%zu bytes)\n", path, n);
    return (n == len) ? 0 : -1;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <out_dir> <Disk.1> [Disk.2] [Disk.3]\n", argv[0]);
        return 2;
    }
    const char *out_dir = argv[1];
    const char *disks[4] = { NULL };
    int n = 0;
    for (int i = 2; i < argc && n < 4; i++) disks[n++] = argv[i];

    g_mem = (uint8_t *)calloc(1, MEM_SIZE);
    if (!g_mem) { fprintf(stderr, "[dump_banks] out of memory\n"); return 1; }
    if (disk_boot_open(disks, n) < 0) {
        fprintf(stderr, "[dump_banks] could not open disk images\n");
        return 1;
    }

    /* main / intro bank */
    overlay_load_main();
    if (dump(out_dir, "chip_ram_dump.bin", 0x80000u) < 0) return 1;

    /* gp / title bank (fresh decrunch first for clean block-copy source) */
    overlay_load_main();  overlay_load_title();
    if (dump(out_dir, "chip_flow_gp.bin", 0x80000u) < 0) return 1;

    /* gpl / gameplay bank */
    overlay_load_main();  overlay_load_gameplay();
    if (dump(out_dir, "gmem_after_load.bin", 0x600000u) < 0) return 1;

    /* credits / end-game bank */
    overlay_load_main();  overlay_load_credits();
    if (dump(out_dir, "gmem_after_credits.bin", 0x600000u) < 0) return 1;

    disk_boot_close();
    fprintf(stderr, "[dump_banks] done -> %s\n", out_dir);
    return 0;
}
