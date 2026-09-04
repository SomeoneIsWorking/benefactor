---
id: 1
title: Integrate Benefactor with a maintained 68000 interpreter
status: open
symptom: The intended gameplay product cannot be built because shared/amigaport and its Benefactor runtime adapter do not exist yet.
state_items: S005
tags: interpreter,amigaport,execution
created: 2026-09-04
updated: 2026-09-04
---

Create the narrow Benefactor executor adapter around `shared/amigaport`. Preserve
the production disk/image, memory, OCS/CIA, native override, and host subsystem
boundaries. The first discriminator compares a bounded authenticated main-image
slice against an independent oracle; do not compose mixed static/interpreter gameplay.
