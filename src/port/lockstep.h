/* src/port/lockstep.h — hold this product at every frame boundary so the
 * reference product can be run beside it, frame for frame.
 *
 * `tools/oracle_diff.py` compares two finished runs; by the time a screen's
 * frame count is wrong, the cause is hundreds of frames upstream. This is the
 * other end of that: with `lockstep=<readfd>:<writefd>` set (the driver in
 * `tools/lockstep.py` sets it), the product reports what its guest memory
 * hashes to at the end of every presented frame and then BLOCKS until the
 * driver says to go on. The driver holds the reference product the same way,
 * so neither can run ahead, and the first frame whose two reports differ is
 * where the divergence was born — with both products still parked on it.
 *
 * The protocol and the hash live in `port/lockstep_digest.h`, which the
 * reference product includes too. This file is only the wiring: where the
 * frame's state comes from in THIS product, and where the knob is read. */
#ifndef BENEFACTOR_PORT_LOCKSTEP_H
#define BENEFACTOR_PORT_LOCKSTEP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Report this frame and wait for the driver. Does nothing unless `lockstep` is
 * configured. Called from hw_present_frame, beside pc_note_frame_signature(). */
void pc_lockstep_frame(void);

#ifdef __cplusplus
}
#endif

#endif /* BENEFACTOR_PORT_LOCKSTEP_H */
