#include "disk.h"
#include "memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DISKS 4

static FILE    *disk_fp[MAX_DISKS];
static uint32_t disk_sz[MAX_DISKS];
static int      n_open = 0;

int disk_open(const char *const *paths, int n)
{
    disk_close();
    if (n > MAX_DISKS) n = MAX_DISKS;
    n_open = 0;

    for (int i = 0; i < n; i++) {
        if (!paths[i]) { disk_fp[i] = NULL; disk_sz[i] = 0; continue; }

        disk_fp[i] = fopen(paths[i], "rb");
        if (!disk_fp[i]) {
            fprintf(stderr, "disk_open: cannot open '%s'\n", paths[i]);
            /* Non-fatal: continue without that disk */
            disk_sz[i] = 0;
        } else {
            fseek(disk_fp[i], 0, SEEK_END);
            disk_sz[i] = (uint32_t)ftell(disk_fp[i]);
            rewind(disk_fp[i]);
            n_open++;
        }
    }
    return (n_open > 0) ? 0 : -1;
}

void disk_close(void)
{
    for (int i = 0; i < MAX_DISKS; i++) {
        if (disk_fp[i]) { fclose(disk_fp[i]); disk_fp[i] = NULL; }
        disk_sz[i] = 0;
    }
    n_open = 0;
}

int disk_load(int disk_no, uint32_t src_off, uint32_t dst_addr, uint32_t length)
{
    int idx = disk_no - 1;
    if (idx < 0 || idx >= MAX_DISKS || !disk_fp[idx]) return -1;
    if (src_off >= disk_sz[idx]) return 0;
    if (src_off + length > disk_sz[idx])
        length = disk_sz[idx] - src_off;

    /* Allocate a temporary buffer, read from disk, load into chip RAM */
    uint8_t *buf = (uint8_t *)malloc(length);
    if (!buf) return -1;

    fseek(disk_fp[idx], (long)src_off, SEEK_SET);
    size_t got = fread(buf, 1, length, disk_fp[idx]);

    if (got > 0)
        mem_load(dst_addr, buf, (uint32_t)got);

    free(buf);
    return (int)got;
}

uint32_t disk_size(int disk_no)
{
    int idx = disk_no - 1;
    if (idx < 0 || idx >= MAX_DISKS) return 0;
    return disk_sz[idx];
}
