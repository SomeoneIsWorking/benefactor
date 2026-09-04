# Benefactor recovered behavior notes

These files contain durable title-specific reverse-engineering evidence. Active
product intent, status, ownership, and execution order live in
`docs/project-goals.md`, `docs/project-state.md`, `docs/codemap.md`, and
`docs/migration.md`.

| File | Evidence subject |
| --- | --- |
| `gameplay-engine-map.md` | gameplay execution, addresses, state, objects, transitions, and images |
| `audio-engine.md` | replayer, SFX, Paula/CIA paths, and interrupt facts |
| `harness.md` | PUAE comparison observations and confirmed root causes |
| `rom-analysis.md` | title-specific binary analysis notes |
| `widescreen-plan.md` | recovered world/view/culling facts and enhancement design |
| `rendering-overhaul-plan.md` | current renderer ownership and presentation facts |
| `gpu-renderer-plan.md` | Vulkan and scene-path facts |
| `benmotion-plan.md` | alternate-physics behavior and evidence |
| `current-state.md` | accumulated dated evidence; not the project-state authority |
| `run-harness.md` | PUAE driving details that remain useful to the independent oracle |
| `scratchpad.md` | investigation-note discipline |

Do not follow an old command, generated-function name, or static execution plan
from these evidence notes. Translate the underlying address/ABI/behavior fact
into the native/dynarec owners described by `docs/migration.md`.
