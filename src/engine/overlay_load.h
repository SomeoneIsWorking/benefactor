/* overlay_load.h — pure loaders for the four executable runtime images.
 *
 * Main/intro, title, gameplay, and credits load different executable bytes into
 * overlapping guest addresses. These functions preserve the disk read, ATN!
 * decrunch, and relocation effects independently of CPU execution.
 *
 * They currently operate on the transitional global g_mem. The Benefactor
 * executor adapter must inject amigaport's active memory mapping and assign a
 * new image generation after each successful load. Title-specific wrappers own
 * the corresponding runtime side effects; this unit owns bytes only.
 *
 * Each overlay's block-copy reads boot-decrunch source at $6D734, so call
 * overlay_load_main() first (fresh) before overlay_load_title/gameplay().
 */
#ifndef BENEFACTOR_OVERLAY_LOAD_H
#define BENEFACTOR_OVERLAY_LOAD_H

void overlay_load_main(void);     /* main/intro bank: boot decrunch Disk.1 -> $3000 */
void overlay_load_title(void);    /* gp/title bank: title overlay */
void overlay_load_gameplay(void); /* gpl/gameplay bank: gameplay overlay + relocation */
void overlay_load_credits(void);  /* credits/end-game bank: Disk.3 overlay -> $3330 */

#endif
