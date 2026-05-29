#pragma once
/*
 * amiga/disk.h  –  Raw disk image loader.
 *
 * Benefactor uses a custom sector format (DMFM_NR) on its original ADFs,
 * but the WHDLoad-converted disk images (Disk.1 / Disk.2 / Disk.3) are
 * raw binary dumps that the WHDLoad slave maps directly into chip RAM
 * via resload_LoadDisk.
 *
 * We replicate that behaviour: the caller specifies which byte range of
 * which disk image to load and where to put it in chip RAM.
 */

#include <stdint.h>

/*
 * Open up to 4 disk image files.
 * disk_paths[0] = Disk.1, [1] = Disk.2, [2] = Disk.3, etc.
 * Returns 0 on success, -1 on error.
 */
int disk_open(const char *const *disk_paths, int n_disks);
void disk_close(void);

/*
 * Load `length` bytes from disk `disk_no` (1-based) at byte offset
 * `src_offset` into chip RAM at `dst_addr`.
 * Returns number of bytes actually loaded, or -1 on error.
 */
int disk_load(int disk_no, uint32_t src_offset, uint32_t dst_addr, uint32_t length);

/* Return the raw size (bytes) of disk `disk_no`. */
uint32_t disk_size(int disk_no);
