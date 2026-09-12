---
id: 1
title: Integrate Benefactor with a maintained 68000 interpreter
status: open
symptom: The shared/amigaport adapter and authenticated-disk boot now exist, but representative gameplay conformance and cross-platform execution are still unproven.
state_items: S005
tags: interpreter,amigaport,execution
created: 2026-09-04
updated: 2026-09-08
---

The narrow Benefactor executor adapter around `shared/amigaport` is implemented
and the first authenticated Disk.1-Disk.3 run reaches `$577000`, performs native
ATN loads, and runs the retail `=SB=` level-data dispatcher. Preserve the
production disk/image, memory, OCS/CIA, native override, and host subsystem
boundaries. Remaining work is the complete four-image representative gameplay
gate on each claimed host; do not compose mixed static/interpreter gameplay.

On 2026-09-08, the first live gameplay interaction exposed a control-flow bug in
the adapter: `rt_jump` recursively entered the interpreter while a native
override was active, which corrupted the executor boundary. The shared runtime
now supports an in-frame native continuation and the audio override consumes its
guest JSR return address. The run advances past that crash into a later title
memory fault at `$57CE72`; the current discriminator reports the faulting address,
registers, and most recent original-call boundary, so representative gameplay
remains open rather than being claimed from boot evidence.

Further live classification showed that the remaining native capture wrappers were
calling guest JSR routines as unbounded streams. The build family (`$57B19E`,
`$57B856`, `$57B07C`), banner family (`$578974`, `$578B94`, `$578860`,
`$57889C`), password builder (`$57901E`), and game-over renderer (`$59C5B0`)
now use the shared subroutine boundary, which consumes the guest RTS return from
the actual stack. With the `$59C5B0` boundary corrected, a Clang headless Disk.1–3
run remained in gameplay through held-right, fire, interact, and directional-fire
input; the player block advanced from `[32,166,0,2]` to `[179,166,0,2]` without
the previous memory fault. Full title, platform, and performance conformance is
still open.
