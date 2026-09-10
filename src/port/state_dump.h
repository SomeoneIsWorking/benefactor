/* src/port/state_dump.h — snapshot the guest's own memory at a known moment.
 *
 * The per-screen frame counts and the frame signature say THAT the two
 * products diverge. They cannot say WHERE. This does: both products write out
 * the whole guest address space at the same moment in the game, and
 * `tools/state_diff.py` diffs the two images and reports the addresses that
 * differ. An address is a lead a person can act on — it can be looked up in
 * the disassembly, watched with a breakpoint, or read as a game variable —
 * where "gameplay looks glitchy" cannot.
 *
 * The moment is anchored to a SCREEN, not to a frame number, because the two
 * products do not agree on frame numbers: gameplay begins at frame 7608 in the
 * reference and 7612 here. Diffing frame 7700 against frame 7700 would compare
 * two different instants and report the whole playfield as divergent. So the
 * anchor is the frame a given copper list first appears on, and the dumps are
 * taken at fixed offsets after it.
 *
 * Configured by `state_dump=<cop1lc>:<offset>,<offset>,...`, e.g.
 * `state_dump=003484:0,30,120` — at the gameplay screen, and 30 and 120 frames
 * into it. Off unless asked for; writing 8MB a frame is not something to do by
 * accident. */
#ifndef BENEFACTOR_PORT_STATE_DUMP_H
#define BENEFACTOR_PORT_STATE_DUMP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Write a snapshot if this frame is one of the configured moments. Called from
 * hw_present_frame, beside pc_note_frame_phase(). */
void pc_note_state_dump(void);

#ifdef __cplusplus
}
#endif

#endif /* BENEFACTOR_PORT_STATE_DUMP_H */
