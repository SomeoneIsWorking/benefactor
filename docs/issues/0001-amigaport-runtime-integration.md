---
id: 1
title: Integrate Benefactor with an on-demand 68000 dynarec
status: open
symptom: The intended gameplay product cannot be built because every non-native guest path still depends on offline-generated host functions.
state_items: S005
tags: dynarec,amigaport,execution
created: 2026-09-04
updated: 2026-09-04
---

Create the narrow Benefactor executor adapter around `shared/amigaport`. Preserve
the production disk/image, memory, OCS/CIA, native override, and host subsystem
boundaries. The first discriminator is a separately built JIT-versus-test-oracle
block from the authenticated main image; do not compose mixed static/JIT gameplay.
