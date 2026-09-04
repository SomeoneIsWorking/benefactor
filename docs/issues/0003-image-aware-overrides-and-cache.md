---
id: 3
title: Key translations and overrides by four-image generation
status: open
symptom: Main, title, gameplay, and credits images reuse guest addresses, so address-only dispatch can execute stale code or select the wrong native override.
state_items: S021
tags: dynarec,cache,overlays,overrides
created: 2026-09-04
updated: 2026-09-04
---

Give every load/restore an image-generation identity and include it in block and
override keys. Prove title and credits at `$003330`, executable mutation/reload,
override installation, and scoped original call in positive and controlled-
negative cases. An original call executes through the dynarec and cannot name a
generated host body.
