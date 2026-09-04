---
id: 3
title: Key execution and overrides by four-image generation
status: open
symptom: Main, title, gameplay, and credits images reuse guest addresses, so address-only dispatch can execute from the wrong image or select the wrong native override.
state_items: S021
tags: interpreter,images,overlays,overrides
created: 2026-09-04
updated: 2026-09-04
---

Give every load/restore an image-generation identity and include it in active
execution and override keys. Prove title and credits at `$003330`, reload,
override installation, and scoped original call in positive and controlled-
negative cases. An original call executes through the interpreter and cannot
bypass the shared CPU owner.
