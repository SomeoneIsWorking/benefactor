#ifdef __cplusplus
extern "C" {
#endif

/* src/port/frame_signature.h — what the frame PLAYED and SHOWED, per frame.
 *
 * Per-screen frame counts (tools/oracle_diff.py) prove a screen lasts the
 * right number of frames. They cannot see a melody that stalls or a colour
 * fade that never ramps — a timing change was landed and reverted on exactly
 * that blind spot (docs/issues/0008). This is the missing half: one line per
 * CHANGE in what the frame plays and shows, so two products reduce to two
 * event streams that can be diffed.
 *
 *   sig: frame=<n> pal=<fnv1a of the 32 palette entries>
 *        alc=<AUD0..3 sample pointer> aper=<period> avol=<volume> adma=<DMACON>
 *
 * The palette hash is the fade instrument: a fade is a ramp of palette writes,
 * so a broken fade is a stream that jumps or stands still. The audio pointers
 * are the melody instrument: the music player rewrites AUDxLC every time it
 * moves to the next sample, so a stalled tune stops emitting.
 *
 * The reference product emits the identical line from the equivalent point in
 * its own hw_present_frame (tools/oracle_diff.py, REFERENCE_EDITS). Keep the
 * two formats byte-identical or the diff is meaningless. */
#ifndef BENEFACTOR_PORT_FRAME_SIGNATURE_H
#define BENEFACTOR_PORT_FRAME_SIGNATURE_H

/* Emit the signature of the frame being presented, if it differs from the last
 * one emitted. Called from hw_present_frame, beside pc_note_frame_phase(). */
void pc_note_frame_signature(void);

#endif /* BENEFACTOR_PORT_FRAME_SIGNATURE_H */

#ifdef __cplusplus
}
#endif
