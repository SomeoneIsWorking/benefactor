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
