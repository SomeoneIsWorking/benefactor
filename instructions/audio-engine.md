# Benefactor gameplay AUDIO engine — RE map (for native port)

Status: **reverse-engineered, native port not yet started.** This is the foundation so
the port isn't blind (per user). a5(gameplay)=`$57EE12`, a6 in ISRs=`$dff000`.

## Big picture
A custom Amiga music **replayer** + **SFX** engine, unified per channel (SFX is mixed
into / steals the music voice). Two clocks drive it:
- **LVL3 vblank ISR `$578272`** (once/frame) → `$59BA7A` = the per-frame SEQUENCER +
  SFX streamer dispatch. This is the "tick".
- **CIA-B timer ISR** (the `$78` vector, two-phase: `$59BF3E` DMACON retrigger +
  `$59BFA6` AUDxLC/LEN copy) = the OUTPUT stage that pushes prepared buffers to Paula.
  Delivered on PC by `pc_music_tick` (gated `irq_level_enabled(LVL6)`).

The replayer prepares, per frame, each channel's next sample CHUNK in a work buffer and
sets the Paula shadow regs; the CIA ISR then latches them to hardware. PC's `hw_audio.c`
mixer currently RE-INTERPRETS those shadow regs (continuous DMA, reload-at-loop-boundary)
— a lossy second layer. The native port replaces the recompiled replayer with C we own
and (ideally) generates PCM directly, removing the guesswork.

## Per-frame dispatch — `$59BA7A` (from vblank `$578272`)
1. Tempo accumulator at `$59b806` (`+= $59b808`, clamp/wrap `$40`) — song speed.
2. `$586790` = per-frame housekeeping (gated `$1093(a5)` bit6).
3. Global gate `$586928` (audio off?).
4. Per channel, if its pending flag set, call the voice processor:
   - ch0: `$57fe4e` → `$586612` (a3=`$a0(a5)`=AUD0 regs)
   - ch1: `$57fe4f` → `$58684C` (a3=`$b0(a5)`=AUD1 regs)
   - (ch2/ch3 handled further down; `$57fea5` bit1 path, voice buffers `$59d154`)
5. Copies pending→ready flags for the output ISR: `$59cf1c = $57fe4e` (ch0),
   `$59cf1d` (ch1), `$57fea5` bit1 (ch2/3).

## SFX engine (`$586xxx`, 24 fns)
- `$58656E` (`$775c(a5)`) — **SFX TRIGGER / request**. Caller sets `a3` = &sound-def
  (`$5Bxxxx` table). Priority-arbitrates vs the active sound at `$57fe50` (fields
  `$6`/`$10`); if it wins, copies the def (5 longs) to `$57fe50`, saves base sample ptr
  to `$57fe78`, sets `$57fe4e=$FF`, kicks DMACON. 67 call sites game-wide.
- `$586612` — **ch0 voice STREAMER** (double-buffered). Descriptor `$57fe50` layout:
  `+0`=cur read ptr, `+4`=period, `+6`=vol, `+8`=len(words), `+a`=chunk words-1,
  `+c`=chunks remaining, `+e`=loop add, `+12`=chunk reload. Each frame: copy next chunk
  (`+a`+1 longs) from `(+0)` into ping-pong buffer (`$57e60` / `$57f8c`, toggled by
  `not.w $586610`); write buffer→AUDxLC, `+8`→LEN, `+4`→PER, `+6`→VOL; advance `+0`;
  `--(+c)`; on 0 apply loop (`+e`/`+12`) or end (`$586680`: clear `$57fe4e`, `$59d154`).
  Sets `$57fe4e=$FF` while streaming. **This is where back-to-back SFX must restart
  cleanly — the dropped-2nd-grunt bug lives in this streaming vs the PC mixer.**
- `$58684C` — ch1 voice streamer (sibling of `$586612`).
- `$586790` — per-frame housekeeping. `$5865BA`/`$5865E4`/`$58652E` — more play entry
  points. `$586940`/`$586982`/`$5869E2`/`$586B1C`/… — sub-helpers / note/effect.

## Output ISR (CIA-B timer, the `$78` vector)
- `$59BF3E` — reads enable mask `$59d240` + ready flags `$59cf1c`(ch0)/`$59cf1d`(ch1)/
  `$57fea5`.1; disable-then-enable DMACON (`$dff096`) to retrigger ready channels;
  advances `$78(a0)` self-mod vector by `$68`.
- `$59BFA6` — copies prepared AUDxLC/AUDxLEN from voice buffers `$59d14e` (per-channel
  blocks at +0/+`$3e`/+`$7c`/+`$ba`) into Paula `$a0/$b0/$c0/$d0(a5)`, gated by the
  same flags; sets INTREQ; reinstalls the other phase via `$59d140`/`$59d144`.

## Music replayer (`$59BA7A`..`$59BE9E`+, ~many fns)
Note sequencer + instruments + effects. Tables: song/pattern data, voice state at
`$59cf2a`+, voice buffers `$59d154`/`$59d14e`, instrument table `$C16A` (title-bank ref —
verify gpl), tempo `$59b806`/`$59b808`. Full per-note RE still TODO.

## Shared state (absolute addresses)
- `$57fe4e` ch0 SFX/voice pending (`$FF`/`0`); `$57fe4f` ch1; `$57fea5` flags byte
  (bit1 ch2/3 ready, bit5/6 gates); `$57fe50` active SFX descriptor (20B);
  `$57fe78` active SFX base sample ptr (stable id); `$57feb4` (40 refs — voice/var).
- `$59cf1c`/`$59cf1d` ready flags; `$59d240` DMACON enable mask; `$586610` buffer
  toggle; `$586928` audio-off gate; `$57e60`/`$57f8c` ch0 ping-pong buffers.

## Native port plan (staged)
1. **Own the per-channel state** as a C struct (mirror `$57fe4e/50/78`, buffers).
2. **Port the SFX path first** (bug locus): `$58656E` trigger + `$586612`/`$58684C`
   streamers + `$586790`, as native overrides (keep recomp alive, A/B via dispatch).
   Verify with `sfxcmp` (set parity) + WAV capture.
3. **Port the output stage** (`$59BF3E`/`$59BFA6`) — or better, have the native streamer
   write a clean per-channel state a native MIXER reads directly (bypass Paula-shadow
   reinterpretation in hw_audio.c).
4. **Port the music replayer** (`$59BA7A` core + note/instrument/effect helpers).
5. Remove the hw_audio boundary-reload guesswork once the native engine drives PCM.

Verify each stage behaviorally vs PUAE (WAV compare + sfxcmp), never blind.

## Status / progress (2026-06-03)

- **SFX TRIGGER `$58656E` → native** (`native_sfx_trigger`, pc_overrides_audio.c).
  Faithful priority arbitration; logs PLAY/REJECTED w/ frame+pri+vol under `SFX_NATIVE_LOG`.
- **SFX DROP BUG fixed (commit 2cd9e73).** Root cause: the streamer chunks the sample
  1 frame/chunk for Paula DMA; `hw_audio`'s continuous-DMA "reload at loop boundary"
  mis-followed those per-frame AUDxLC/LEN swaps → ~1/3 of grunts rendered near-silent
  even though they won arbitration. Fix = **native SFX voice**: on trigger we hand the
  WHOLE sample (from the descriptor) to `hw_audio_sfx_play(ch,...)`, which plays it
  one-shot/looped on Paula's fixed pan while `$57fe4e`/`$57fe4f` is set, bypassing the
  chunk-follow. Every jump now bursts consistently (14/14 vs ~10/14). Total length =
  `chunks(+c) * (chunk_longs_m1(+a)+1) * 4` bytes from base `$57fe78`; loop if `+e`≠0.
- **OPEN: overall level gap.** PC full-mix peaks ~18% vs PUAE ~48% (same fire+left run).
  Separate from the drop bug — looks like a master/mix-gain difference; not yet resolved.
- **Music replayer (`$59BA7A`/`$59BB5E`+)** still recompiled, not yet native-owned.

### Tooling
- `AUDIO_SFX_ONLY=1` — mixer renders only the native SFX voices (drops music) WITHOUT
  freezing the timer. Do NOT use `BENEFACTOR_MUTE_MUSIC` — it gates the LVL6 CIA timer,
  which also stalls GET READY + SFX output (everything goes silent, level never starts).
- `AUDIO_DUMP=path` (PC raw s16 stereo @22050) / `AUDIODUMP=1` (PUAE → logs/audio_puae.raw
  @44100). `scratch/audio_tools.py` = `raw2wav` + `bursts` (envelope/peak per sound).
- Drive: `rungame` (PC) + `pugoto N` (PUAE) — see instructions/current-state.md.
