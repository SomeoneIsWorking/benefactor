/* src/port/control/control_server.h — the loopback control channel.
 *
 * One place to inspect and DRIVE the running game: read engine state, memory
 * and registers, grab the screen, and play it — from a script or from a
 * browser. Enabled by `http=<port>` (BENEFACTOR_HTTP); nothing is created
 * otherwise.
 *
 * The sockets, request limits, concurrency and response framing belong to
 * lucent::http; this module owns only the routes. It replaced a hand-rolled
 * accept() loop that could serve one request at a time, so a screen grab
 * blocked the input route it was being used with.
 *
 * Routes — inspection:
 *   /state                    engine state, frame accounting, audio registers
 *   /cpu                      the guest's D0-D7, A0-A7, PC and SR
 *   /mem?addr=HEX&len=N       guest memory as hex
 *   /poke?addr=HEX&val=HEX    write one byte
 *   /fb.ppm  /fb.bin          the presented screen, as PPM or raw ARGB
 *   /trace   /recent          retired guest instructions, recent call targets
 *
 * Routes — playing it:
 *   /hold?fire=1&l=1          buttons stay set until changed
 *   /press?fire=1&frames=3    set for exactly N frames, released by the frame
 *                             loop — a press is not a race against two round
 *                             trips any more
 *   /step?frames=N            run N frames, then hold still
 *   /pause  /resume           hold still / carry on
 *   /save   /load   /gameover
 *   /                         a page with buttons and the live screen
 */
#ifndef BENEFACTOR_PORT_CONTROL_CONTROL_SERVER_H
#define BENEFACTOR_PORT_CONTROL_CONTROL_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Start the control channel if a port is configured. Safe to call once. */
void pc_control_server_start(void);

#ifdef __cplusplus
}
#endif

#endif /* BENEFACTOR_PORT_CONTROL_CONTROL_SERVER_H */
