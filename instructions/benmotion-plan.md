# BENMOTION — native ownership of all player physics & movement

Status 2026-06-12: **Stages 0(core)+1+2+3 SHIPPED** (platformer.c rewritten:
native flight owns $579D84 rise + $579F3A descent with NO super-call; JUMP
trigger wraps the terrain pass $57A934; vanilla UP-hop/long-jump commits
suppressed; JUMP defaults pad A / Space on the modern scheme). Knob OFF
verified byte-identical (90-frame hop+long-jump A/B). Stage 0 RE results are
in gameplay-engine-map.md — KEY REVISION to this plan: landing/walls/ledge-
fall live in the per-frame TERRAIN PASS `$57A934`, not in the fall handler;
`$579F86` is the landing-impact state and carries FALL DAMAGE
($3b9a[$f6e/2] off health $1c.w); tile landing requires state == $579F3A,
so native flight uses $579D84 while rising and $579F3A while falling and
needs NO landing port of its own. Remaining: Stage 4 (grounded movement),
R0.1 leftovers (knockback/water states stay vanilla — fine), user feel pass.
Known wrinkle: holding UP (or fire+dir) grounded flaps $f70 commit/revert
once per 2 frames — harmless (no motion/SFX) but visible in state traces.

User goals, verbatim:
1. **Own all the player physics and movement** (no more hybrid super-call/TRACK
   riding of the vanilla table arcs).
2. **One jump, dedicated button** — replace vanilla's two jumps (UP = hop,
   Fire+direction = long/rolling jump) with a single jump on its own binding.

All of this stays behind the existing `platformer_physics` knob (vanilla mode must
remain byte-identical, same as today). Builds on `src/port/overrides/platformer.c`
(hop/fall full-physics + TRACK mode, aaeea7d) which this plan REPLACES in the air
and EXTENDS to the ground.

## Why the current shape isn't enough

Today's platformer mode is a hybrid: hop/fall super-call the recompiled handler and
re-shape d1/d2; the long-jump family runs the vanilla arc in TRACK mode with a steer
bolted on. That means three different feels mid-air, the vanilla arc's fixed shape
still dominates the long jump, and two separate jump inputs with different physics.
Owning the whole thing gives ONE consistent movement model and frees FIRE entirely
for interactions (pickup/drop/MM — the fire-vs-longjump conflict documented in
gameplay-engine-map.md "held-item USE" section disappears).

## Stage 0 — RE prerequisites (read gameplay-engine-map.md first; extend it as found)

The known facts (a5 = $57EE12; dispatcher $5796A4 loads $10A6.. into d1=X d2=Y
d3=ANIM d4=flags d5=phase*2, calls $f70(a5), stores back):

- **R0.1 State inventory.** Enumerate every $f70 state reachable in normal play and
  classify movement-owned vs interaction-owned. Known: $579D84 hop, $579A62 long
  jump, $579D00/0E/1C/2A/38/46→$579D52 abort arcs, $579F3A fall, $57E43C/$57E4E6/
  $57E4EE grounded input dispatch, $5799E6 default, plus unclassified $579F0E,
  $579F86, $57A018, $57A2A2, $57A2D6, $57A30C (suspects: ladder, rope, turn, death,
  lever, place-item). Method: REPL `pcwatch` on $57FD82 ($f70) while playing every
  mechanic on a savestate tour (ladder, rope, carry, lever, door, death).
- **R0.2 Landing tail of $579F3A.** To own descent we must own landing: the ground
  probe, the Y snap, the state written back to $f70, the landing SFX descriptor
  (the $775c(a5)=$58656E call, a3 from -$273c(a5)), anim reset (clr.w d3), and the
  $f6e ramp bookkeeping. Disassemble + live-trace one landing.
- **R0.3 Grounded movement.** RE walk speed/step, turn handling, and the walk-off-
  ledge → fall transition inside $57E43C/$57E4E6/$57E4EE (and where the decoded
  input word $10ac/d4 gates each branch). Needed for Stage 4 and for suppressing
  the vanilla jump triggers without breaking ladders/doors (UP must keep climbing).
- **R0.4 Anim cell vocabulary.** Map d3 anim cells for: walk cycle, jump rise,
  apex, fall, land squash, turn — from the vanilla tables ($309c/$2ce6/$5d02(a5)
  arcs + walk anim) so the native model can drive d3 itself.
- **R0.5 Carry interactions.** Jump-while-carrying must keep working (vanilla long
  jump works while carrying; MM carry = $10AC bit14). Verify which flags the arc
  states maintain that the carry path reads (d4 bit6 "jumped", $f6e, $f82).

## Stage 1 — dedicated JUMP input

- Add a JUMP action to the binding layer in `src/port/input.c` alongside FIRE/
  INTERACT: `hw_get_jump()`, default keyboard key + controller face button (A),
  row in the pause BINDINGS menu, persisted via pc_cfg_persist.
- Platformer physics reads raw `hw_get_jump()` (the engine's $f80 decode is gated
  off mid-air by $1093 bit0 — established fact; raw reads are the pattern).
- DEMO mode caveat: $57DEAC plays/records an RLE input stream when $1e.w==8 — JUMP
  is not representable there; platformer mode must not engage during demo playback.

## Stage 2 — ONE native flight state (own the air completely)

Replace hop/long-jump/abort/fall handling with a single native flight model:

- Physics: 8.8 velocity (vx, vy), gravity, variable height via jump-cut on JUMP
  release, air control accel toward held direction, vx cap; existing pf_* knobs
  (pf_gravity, pf_jump_vy, pf_air_accel, pf_vx_max) plus new pf_walk_speed,
  pf_ground_accel/friction (Stage 4).
- Collision: own probes (tile(x,y) = word($5A8C7E + (x>>4)*2 + rowtab($5A211A)
  [y>>4]) ≠ 0): head bonk, leading-edge wall at two body heights, ground probe for
  landing (R0.2 tells us the exact feet offset and snap rule the engine uses).
- Landing: replicate R0.2 natively — Y snap, $f70 → grounded state, landing SFX
  via the same descriptor/trigger ($775c(a5)), anim/flags bookkeeping ($f6e, d4).
- Engine-visible state: hold $f70 at ONE canonical air state ($579F3A — already
  a legitimate "airborne" value every other system tests against) for the whole
  flight; our override fully replaces its body in platformer mode (NO super-call).
  The other air states ($579D84, $579A62, $579Dxx) become entry shims: seed the
  model from how vanilla entered (shouldn't trigger at all once Stage 3 lands,
  but must be safe if something engine-internal enters them, e.g. knockback).
- Anim: drive d3 from vy sign/magnitude using the R0.4 cell map (rise/apex/fall/
  land squash).
- Takeoff/landing grunts: fire the same SFX descriptors vanilla uses via $58656E.

## Stage 3 — one jump, dedicated button

- Trigger: hook the player dispatch (wrap $5796A4, or the grounded handlers from
  R0.3) — when platformer on AND grounded AND `hw_get_jump()` pressed (edge) AND
  headroom probe passes (vanilla's own pre-validation idea from $57E43C, but
  against OUR first-frame arc): enter native flight, vy = pf_jump_vy, vx = current
  ground velocity (momentum-preserving), takeoff grunt.
- Suppress vanilla triggers (platformer mode only), at the SPECIFIC trigger sites:
  - UP-hop: the $f70←$579D84 writers ($57E43C@$57E526, $57E4E6, $57E4EE). UP must
    still climb ladders/enter doors — suppress only the hop commit, not UP input.
  - Fire+dir long jump: the entry into $579A62 from the grounded dispatch.
  Do NOT strip bits from $f80/$10ac globally — documented dead end (the drop-
  re-gate failure): selection reads the DECODED word $10ac/d4, and input-level
  patching breaks unrelated consumers.
- FIRE is thereby freed for interactions only (pickup/drop/MM lift-drop), which
  also unblocks the parked "held-item use on interact key" idea — out of scope
  here, but note the dependency.
- Jump-while-carrying must work (R0.5): same trigger, carry flags preserved.

## Stage 4 — own grounded movement

- Native walk: vx accel/decel/friction (knobs), direction facing, drive d1 (X) and
  d3 (walk anim cycle from R0.4) — start as re-shape-after-super (the proven
  pattern: vanilla keeps interactions/transitions, we own position/velocity), then
  evaluate going full-native per handler once R0.3 is complete.
- Walk off a ledge → enter native flight with current vx (no vanilla fall handoff).
- Out of scope (stay vanilla): ladders, ropes, levers, doors, death, cutscene
  states — they're interaction, not platforming; flight/ground states must enter
  and exit them exactly like vanilla (state-pointer compatibility is the contract).

## Stage 5 — verification & ship gates

- Knob OFF: byte-identical trajectory A/B vs pre-change build (the established
  `mp 57FDA6`/`mp 57FD82` per-frame harness-script diff from logs/savestate.bin).
- Knob ON: scripted harness checks — jump on JUMP only (UP no longer hops, fire+dir
  no longer long-jumps), variable height, air control both directions, wall/head/
  land probes, landing state + SFX descriptor matches vanilla's, jump while
  carrying MM, ladder still climbable with UP, drop (fire+down) still works.
- Regression tour: one full level played via HTTP-probe session with the user
  (their feel pass decides the default knob values).
- Each stage = its own verified commit; never land a stage that breaks vanilla.

## Risks / open questions

- Unclassified states (R0.1) may include movement states we'd miss (e.g. knockback,
  bounce pads, water?) — inventory first, don't assume.
- Fall damage: unknown if the engine has any fall-height consequence; R0.2 will
  reveal it (if yes, the native model must track fall distance).
- Enemy/hazard collision may read the phase counter d5 or $f82 of air states —
  holding a canonical state must not change hit behavior; verify on a hazard level.
- The $5796A4 wrap point must respect the rt_jump trampoline limitation (can't
  bracket-promote captures — read state at present; see rope memory note).
