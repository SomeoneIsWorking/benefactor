/* overlay_load.h — pure overlay loaders for the recompiler bank inputs.
 *
 * Each recompiler bank (main/intro, gp/title, gpl/gameplay) corresponds to an
 * overlay the game loads into chip RAM at $3000+. These functions reproduce
 * those loads with disk read + ATN! decrunch + relocation ONLY — no dependency
 * on the generated game code, so the bank-input dumper can build and run from a
 * fresh checkout with src/generated/ absent (the bootstrap for regenerating the
 * recompiled code from the user's own disks).
 *
 * They operate on the global g_mem (the caller allocates it and opens the disks
 * via disk_boot_open). The pc_overrides wrappers layer runtime side effects
 * (bank-routing flags) on top — those are NOT done here, to keep this unit free
 * of game-state / generated-code dependencies.
 *
 * Each overlay's block-copy reads boot-decrunch source at $6D734, so call
 * overlay_load_main() first (fresh) before overlay_load_title/gameplay().
 */
#ifndef BENEFACTOR_OVERLAY_LOAD_H
#define BENEFACTOR_OVERLAY_LOAD_H

void overlay_load_main(void);      /* main/intro bank: boot decrunch Disk.1 -> $3000 */
void overlay_load_title(void);     /* gp/title bank: title overlay */
void overlay_load_gameplay(void);  /* gpl/gameplay bank: gameplay overlay + relocation */
void overlay_load_credits(void);   /* credits/end-game bank: Disk.3 overlay -> $3330 */

#endif
