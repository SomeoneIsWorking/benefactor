---
id: 4
title: Pass representative native/interpreter gameplay conformance
status: open
symptom: Historical boot, menu, level-entry, and four-frame matches do not prove representative native/interpreter gameplay correctness or performance.
state_items: S023
tags: conformance,gameplay,interpreter,performance
created: 2026-09-04
updated: 2026-09-04
---

Pass the complete gate in `docs/migration.md` on every released host. The static
pipeline was removed first and must not be restored as a bridge, compatibility
mode, fallback, or permanent oracle.
