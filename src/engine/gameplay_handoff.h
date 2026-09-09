/* src/engine/gameplay_handoff.h — low-memory state the retail $150 loader body
 * establishes before the gameplay engine runs at $577000.
 *
 * The port reconstructs the $150 body in native code and reaches it from two
 * entry points: the $150 override (menu "Start Game" and the attract hand-off)
 * and the direct-to-gameplay developer entry. Both must leave low memory in the
 * exact shape the $577000 prologue and the per-frame engine expect.
 */
#pragma once

/* Write the fixed low-memory words the gameplay engine reads on entry:
 *
 *   $3E / $184  Display-list tail sentinels ($00000A68), chased by the
 *               title-card glyph renderer ($578162, dest = *($3E) + $1C).
 *               Left at zero the renderer scribbles the disk-chunk pointer
 *               table at $100 and the dispatcher later walks a null chain.
 *
 *   $1E.w       Gameplay mode / difficulty word. The $577000 prologue indexes
 *               a per-difficulty record by ($1E.w & 7); $57DEAC treats the
 *               exact value 8 as attract-demo / game-over input playback and
 *               reads scripted joystick tokens from an uninitialised stream.
 *               A level entry must carry a real difficulty bit, so an
 *               out-of-range value is normalised to NORMAL while a
 *               menu-selected EASY / NORMAL / HARD is preserved.
 */
void gameplay_handoff_prepare_low_memory(void);
