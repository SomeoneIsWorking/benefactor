# Prior art — Benefactor (Amiga ECS, 1994 Digital Illusions/Psygnosis; WHDLoad disk images `Disk.1`–`Disk.3`, 1,003,520 bytes each, SHA-256 `25416a6e…`, `f3649c8d…`, `8dd262d0…` in `src/platform/disk_identity.cpp`)

The only code-level prior art is the WHDLoad install whose disk images we authenticate; its slave source names the boot and loader entry points.

| project | URL | kind | license | target build | build match (addresses checked) | what we may take | notes |
|---|---|---|---|---|---|---|---|
| WHDLoad install `Benefactor.lha` (Mr.Larmer/Wanted Team, CFou!) | https://www.whdload.de/games/Benefactor.html | WHDLoad slave with 68000 source (`src/Benefactor.asm`, `BenefactorCD32.asm`, `Benefactor.islave.asm`) | Public Domain (`:Copyright.` header of `Benefactor.asm`) | ECS v1.2 (12.04.2020) and CD32 v1.2 (08.05.2020); ECS disk images by Helmut Motzkau | matched (2026-10-13): archive `Benefactor.slave` is byte-identical to `whdload/Benefactor/Benefactor.slave`, and its `Disk.1` SHA-256 is our identity; slave loads Disk.1 `$0`–`$1600` to `$76000` and jumps `$412(a0)` = our boot loader `$76412`; `Decrunch` patched at `$5E4(a0)` = our ATN decruncher `$0765E4`; skip-intro jumps `$6D714` = our title loader; high-score load at disk offset `$F3780` = our title chunk E1 offset `$0F3780`; title image at `$3330` | evidence only (loader/decrunch entry points, keyboard-interrupt patch sites `$30B0`, `$3084`, `$33B2`, high-score save `$3698`) | install 2020-05-10; the CD32 slave is a separate build we do not target |
| Benefactor music (Exotica) | https://www.exotica.org.uk/wiki/Benefactor | music listing (Olof Gustafsson, Prorunner 2.0) | n/a | ECS | not applicable | evidence only (replayer family) | |

Not found (2026-10-13): a disassembly, decompilation, symbol map, IDA/Ghidra database, level editor, source release or remake of Benefactor (GitHub, Aminet, EAB, Games That Weren't, jotd666/whdload_slaves, decomp.dev).
