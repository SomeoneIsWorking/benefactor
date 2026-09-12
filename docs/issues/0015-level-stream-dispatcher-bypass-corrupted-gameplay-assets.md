# Issue 0015: Level stream dispatcher bypass corrupted gameplay assets

## Status

Resolved for level-1 entry; later gameplay conformance remains under S005/S023.

## Symptom

After the level card, music continued with the wrong samples, spawn sounds were
missing, and some gameplay graphics looked corrupt. In a frame-indexed level-1
run, the interpreter and static oracle both entered the playfield on frame 248,
but the interpreter's first gameplay audio pointers were near `$057F8C` while
the oracle's were near `$07BE12`.

## Root cause

The native override of `$59DC02` called `sb_decompress(D0, A0)` and returned
whenever D0 pointed at an `=SB=` stream. The retail `$59DC02` is a stream
*dispatcher*, not just a decoder: it installs callbacks, initializes decoder
state, and executes a chain of level-data operations. Bypassing the whole body
skipped that setup. Between frames 32 and 37 of the level card, the incomplete
path overwrote the `SNT!` segment at `$073880` and the surrounding audio sample
bank. The static oracle retained the original bytes there.

## Resolution

`native_level_load` retains its per-level presentation reset hooks, then runs
the complete guest dispatcher through `rt_call`. The unused standalone
`sb_decompress` implementation and its build entry were removed. A future
native decoder must target a proven leaf contract rather than replace this
dispatcher.

## Verification

- With `BENEFACTOR_PRESSES=200:4`, both products show the card at frame 2 and
  gameplay at frame 248. The interpreter reaches frame 500 without a fault.
- At 30, 34, and 40 frames into gameplay, the interpreter and oracle have
  identical bytes in `$070000-$0FFFFF`, including the sample at `$07BE12`.
  Full 8 MiB snapshots differ by 55, 91, and 91 bytes respectively; before
  the correction they differed by roughly 250,000 bytes.
- The first gameplay music-pointer, period, volume, and DMA signatures at
  frames 280–350 match the oracle. Headless stereo PCM is byte-for-byte equal
  through frame 463; the first mismatch is byte 818,524 in frame 464.
  Later audio signatures still need separate conformance work; this issue does
  not claim complete audio parity.
- The composed 352×282 frame at frame 350 is pixel-identical to the oracle
  (ImageMagick absolute-error pixel count: 0). This is one frame, not a
  full-scene graphics conformance claim.
