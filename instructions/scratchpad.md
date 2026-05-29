# Debugging Scratchpad

## Purpose

When a divergence or bug is not immediately obvious, the risk of **circular debugging** is high: the same hypothesis gets re-examined in different wording, the same dead ends get revisited, and hours are lost. A scratchpad prevents this by forcing every test, result, and elimination to be written down before moving on.

**Rule: if you have run more than two harness iterations without writing in the scratchpad, stop and write.**

## Where to Keep It

Create a scratchpad for the current debugging session:
```bash
# Start a new scratchpad for a new investigation
echo "# Scratchpad: <one-line symptom>" > session/scratchpad.md
```

Keep it short. One scratchpad per major divergence investigation. Archive or delete once resolved.

## Format

```markdown
# Scratchpad: Frame 1 pixel diff at (120,17)

## Symptom
Frame 1 PIXEL DIFF, 6.7%, first pixel at (120,17). Copper list matches. BPL1-BPL4 chip RAM bytes differ.

## Hypothesis Tree

### H1: Renderer x-origin misaligned
- **Test**: sweep BENEFACTOR_TOP_BORDER and BENEFACTOR_XDELTA_4200
- **Result**: diff reduced 5499→5048 with TOP=16, XDELTA=-6. Still 5048 wrong.
- **Status**: PARTIAL — explains some pixels but not all. NOT root cause alone.

### H2: Blitter writes wrong data before render
- **Test**: compare PC vs PUAE blitter launch stream
- **Result**: streams match exactly after flag-macro fix. No unique BLTSIZE values.
- **Status**: FALSIFIED

### H3: BPL1MOD/BPL2MOD timing mismatch
- **Test**: BENEFACTOR_ADVANCE_BEFORE=1
- **Result**: diff worsens to 5282+
- **Status**: FALSIFIED

### H4: $3600 mode pf1 fetch misalignment
- **Test**: BENEFACTOR_PF1_SHIFT_3600=-16
- **Result**: diff 4902→4834. Translational fix — moves wrong islands, doesn't eliminate them.
- **Status**: PARTIAL — indicates pf1 sampling offset but not semantic rule.

## Open Questions
1. Why does y=60 blow up specifically when plane-1 line-offset is active?
2. Is the $4200 transition rule at line 43 a real hardware behavior or an alignment artifact?

## Next Step
Run BENEFACTOR_TRACE_3600_PIXELS on a representative row to see exactly which plane bits are sampled at wrong x positions. Do NOT run another broad parameter sweep until this data is in hand.

## Anti-Circular Checklist
- [ ] I have not tested this exact knob combination before
- [ ] I am interpreting new data, not re-interpreting old data
- [ ] I can name the specific observation that would falsify my current hypothesis
```

## Rules

1. **Write BEFORE running the next test.** State what you expect, what would confirm, what would falsify.
2. **Write the result immediately after.** Include numbers, not impressions.
3. **Mark hypotheses as FALSIFIED, CONFIRMED, or PARTIAL.** Never leave them open-ended.
4. **If you find yourself wanting to re-run an old test, re-read the scratchpad first.** The answer is probably already there.
5. **When the issue is resolved, copy key findings to `instructions/current-state.md` and `instructions/harness.md`, then delete the scratchpad.**

## When to Start a Scratchpad

Start one when any of these are true:
- The harness shows a DIFF and the cause is not obvious after 10 minutes
- You have tried more than 2 different fixes without success
- You are about to explore a parameter space (sweeps, probes, env vars)
- You find yourself reasoning about the same address or register for the third time

## When NOT to Use a Scratchpad

- Trivial one-line fixes (just fix it)
- Recompiler test failures (the test IS the scratchpad)
- Harness PERFECT MATCH (nothing to debug)
