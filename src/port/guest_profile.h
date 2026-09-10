/* src/port/guest_profile.h — WHERE the guest's time went, and WHOSE it was.
 *
 * The retired-instruction ring says which PCs ran most recently; the frame
 * accounting says which owner (game flow / level-3 / level-6 vector) burned
 * the cycles. Neither answers the question that actually blocks a fix: which
 * PCs burned THIS owner's cycles. The crawl's starved music needed exactly
 * that — the ring was saturated by the game flow's own frame wait while the
 * cycles were being charged to the level-6 vector, and the two instruments
 * could not be joined up.
 *
 * The hook is the custom-chip access, the same place the beam is sampled: a
 * guest busy-wait must read a hardware register to make progress, so a loop
 * that burns time cannot hide from it. Each sample buckets the guest PC under
 * the current owner. Reported through the debug server (`/profile`) and the
 * frame watchdog.
 *
 * Implemented in C++ (src/port/guest_profile.cpp) behind this C interface —
 * the engine and the override layer are still C. */
#ifndef BENEFACTOR_PORT_GUEST_PROFILE_H
#define BENEFACTOR_PORT_GUEST_PROFILE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Record that `program_counter` was running when the guest touched a custom
 * chip register, charged to frame-accounting owner `owner` (0 = game flow,
 * 3/6 = that interrupt vector). Cheap enough for every access. */
void pc_profile_sample(uint32_t program_counter, uint32_t owner);

/* Write the hottest PCs per owner into `text` (NUL-terminated, never longer
 * than `capacity`). Returns the number of characters written. */
int pc_profile_report(char *text, int capacity);

/* Forget every sample. Called when a screen changes so one screen's hot loop
 * is not read as the next one's. */
void pc_profile_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* BENEFACTOR_PORT_GUEST_PROFILE_H */
