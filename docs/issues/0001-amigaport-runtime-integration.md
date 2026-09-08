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
ATN loads, and completes the native `=SB=` level-data path. Preserve the
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
