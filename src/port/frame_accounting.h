/* src/port/frame_accounting.h — where a frame's guest time and frames go.
 *
 * A screen that runs at the wrong speed is one of a few very different faults:
 * the game flow doing too much, an interrupt handler doing too much, or frames
 * simply not being presented. Guessing between them costs hours, so the frame
 * loop records the split and /state (plus the frame watchdog) reports it.
 *
 * Every "_max" is a peak, because the per-frame values are only readable
 * between iterations: a single iteration that runs away leaves the sampler
 * looking at whatever the previous, short iteration wrote. */
#ifndef BENEFACTOR_PORT_FRAME_ACCOUNTING_H
#define BENEFACTOR_PORT_FRAME_ACCOUNTING_H

#include <stdint.h>

/* Guest cycles spent, per owner, during the last host loop iteration.
 * One PAL frame is 141,648 cycles — anything much above that is the fault. */
extern uint64_t g_pc_cycles_flow, g_pc_cycles_flow_max;    /* the parkable game flow */
extern uint64_t g_pc_cycles_irq3, g_pc_cycles_irq3_max;    /* level-3 (vblank) vector */
extern uint64_t g_pc_cycles_irq6, g_pc_cycles_irq6_max;    /* level-6 (timer) vector */
extern uint64_t g_pc_cycles_frame;                         /* game flow + its present */
extern uint64_t g_pc_cycles_present;                       /* inside hw_present_frame */
extern uint64_t g_pc_cycles_outside, g_pc_cycles_iter_max; /* the whole iteration */

/* Which host-side entry is running guest code: 0 = the game flow, 3/6 = that
 * interrupt vector. Guest code entered through a vector runs on the MAIN
 * thread, where the per-frame wait cannot park anything. */
extern volatile uint32_t g_pc_guest_owner;

/* How many times each vector was DELIVERED. The cycle totals say how long a
 * delivery ran; these say how often it happened, which is what a screen driven
 * from inside an interrupt (the intro crawl) actually advances on. */
extern volatile uint32_t g_pc_irq3_calls, g_pc_irq6_calls;

/* Per-frame wait accounting: reached, refused (not the game flow), parked. */
extern volatile uint32_t g_pc_yield_calls, g_pc_yield_refused, g_pc_yield_parks;

/* How many times the title/intro screen redrew itself ($0041A4). Compare it to
 * the presented frame count: more than one draw per frame means the screen is
 * being stepped faster than it is shown, which is what "the crawl is too fast"
 * looks like from here. */
extern volatile uint32_t g_pc_title_draws;

/* Record one measured span into its slot, keeping the running peak. */
void pc_account(uint64_t *slot, uint64_t *peak, uint64_t cycles);

/* Name every screen change with the frame it happened on. A phase timeline is
 * what makes two runs comparable: `tools/oracle_diff.py` builds the retired
 * reference product out of tree, has it emit the same lines, and diffs the two
 * timelines phase by phase — a screen that runs at the wrong speed shows up as a
 * frame count that does not match. Logged once per presented frame, only when
 * cop1lc actually changes. */
void pc_note_frame_phase(void);

#endif
