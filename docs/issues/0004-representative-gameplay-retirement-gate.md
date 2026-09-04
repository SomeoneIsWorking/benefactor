---
id: 4
title: Pass representative gameplay before removing the static pipeline
status: open
symptom: Historical boot, menu, level-entry, and four-frame matches do not prove representative native/dynarec gameplay or product composition.
state_items: S023, S024
tags: conformance,gameplay,retirement
created: 2026-09-04
updated: 2026-09-04
---

Pass the complete gate in `docs/migration.md` on every released host. Only then
remove the offline translator, generated corpus/dispatcher, generation-only
metadata, static-only tests, and active static methodology together. Do not
retain a compatibility mode or permanent generated-code oracle.
