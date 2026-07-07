# Instructions

Per-topic deep dives for the Benefactor PC port. These are reference documents — read them when you need detailed knowledge on a specific topic.

| File | Topic |
|------|-------|
| `master-workflow.md` | Master workflow, self-evolution rules, debugging philosophy, key facts |
| `current-state.md` | Active debugging state, current DIFFs, porting progress, recent findings |
| `harness.md` | Established facts about the comparison harness (DO NOT RE-EXAMINE list) |
| `run-harness.md` | How to run the harness and interpret its output |
| `recompiler.md` | Recompiler usage, improvement targets, workflow |
| `create-override.md` | Native override patterns and existing overrides table |
| `rom-analysis.md` | How to disassemble and trace the M68K binary |
| `scratchpad.md` | How to keep a debugging scratchpad to avoid circular reasoning |
| `gameplay-engine-map.md` | Working RE map of the `$577000+` gameplay engine (per-frame loop, objects, a5 state vars) |
| `audio-engine.md` | Full RE map of the gameplay audio engine (replayer + SFX) + native-port plan |
| `widescreen-plan.md` | Widescreen gameplay plan (wide tile renderer + margin sprites) |
| `gpu-renderer-plan.md` | Per-sprite GPU renderer plan (Scene draw list, SDL/Vulkan consumers) |
| `rendering-overhaul-plan.md` | Three-renderer (Vanilla/Software/Hardware) + BenRen VK plan |
| `benmotion-plan.md` | Native player-motion (flight/jump) port plan |

> **Self-evolution:** When understanding deepens, update the relevant file here. Add DO NOT RE-EXAMINE entries to `harness.md`. Keep `current-state.md` as a current snapshot, not a history.
