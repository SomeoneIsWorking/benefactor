/* src/engine/hw_testrun.h — the scripted, unattended run.
 *
 * A headless run can be told to press fire on a given frame, to save the
 * presented frame as a bitmap, to dump memory regions and to stop after N
 * frames. That is how a fault gets reproduced without a human at the keyboard,
 * and it is entirely diagnostics: none of it belongs in the display path it
 * used to sit inside (`hw_present_frame`).
 *
 * Everything is configured through `src/port/config.c` — the only module
 * allowed to read the environment:
 *
 *   dump_frame=<n>   save the composed output surface as scratch/frame_dump.bmp
 *   press=<n>        press fire (and the left mouse button) on that frame
 *   release=<n>      release them on that frame
 *   dump=<addr>:<len>[,…]   dump those memory regions when the run ends
 *   test=<n>         end the run after that many frames
 *   limit=<n>        arm the frame watchdog for that many frames
 */
#ifndef BENEFACTOR_ENGINE_HW_TESTRUN_H
#define BENEFACTOR_ENGINE_HW_TESTRUN_H

#include <stdint.h>

/* Save the composed output the player just saw, if `frame` is the asked-for
 * one. `surface`, `width` and `height` describe that output. */
void hw_testrun_capture(int frame, const uint32_t *surface, int width, int height);

/* Apply the scripted input for `frame`, and end the run when it is the last.
 * Called with the frame number the display path had reached at this point —
 * one past the frame just captured, which is the numbering the scripted press
 * and release frames have always been compared against. */
void hw_testrun_script(int frame);

#endif /* BENEFACTOR_ENGINE_HW_TESTRUN_H */
