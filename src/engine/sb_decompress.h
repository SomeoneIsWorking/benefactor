/* Native decoder for the game's =SB= LH5-style level-data container. */
#ifndef BENEFACTOR_SB_DECOMPRESS_H
#define BENEFACTOR_SB_DECOMPRESS_H

#include <stdint.h>

/* Decompress the =SB= object at source into destination in guest memory.
 * Returns the header's unpacked byte count, or zero for a malformed object. */
uint32_t sb_decompress(uint32_t source, uint32_t destination);

#endif
