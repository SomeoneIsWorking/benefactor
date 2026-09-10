/* The 68000 exception vector table, by name.
 *
 * A vector is four bytes of big-endian guest memory at a fixed low address, so
 * reading one open-coded is four byte indexes and three shifts:
 *
 *   uint32_t v6 = ((uint32_t)g_chip[0x78] << 24) | ((uint32_t)g_chip[0x79] << 16) |
 *                 ((uint32_t)g_chip[0x7a] << 8)  |  (uint32_t)g_chip[0x7b];
 *
 * which says nothing about WHICH interrupt that is, and where a typo in one of
 * the four indexes reads a neighbouring vector instead. Say it once, by name.
 *
 * The game rewrites these at run time — each screen installs its own handlers,
 * and the intro's level-6 handlers chain by rewriting $78 as they go — so a
 * vector is always read fresh, never cached. See coro_deliver_timer_irq.
 */
#ifndef BENEFACTOR_PORT_GUEST_VECTORS_H
#define BENEFACTOR_PORT_GUEST_VECTORS_H

#include <stdint.h>

/* Autovector addresses in the 68000 exception vector table. */
typedef enum {
    GUEST_VECTOR_LEVEL3_VBLANK = 0x6Cu, /* sets the frame's copper list / COP1LC */
    GUEST_VECTOR_LEVEL6_TIMER = 0x78u,  /* CIA-B timer: the music player chain */
} GuestVector;

/* The handler address currently installed for `vector`, or 0 if none is. */
uint32_t guest_vector_handler(GuestVector vector);

#endif /* BENEFACTOR_PORT_GUEST_VECTORS_H */
