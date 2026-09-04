---
id: 2
title: Model complete 68000 state in amigaport
status: open
symptom: The existing reduced generated-function context has no PC and cannot represent complete SR, exception, supervisor, interrupt, or cycle state.
state_items: S020
tags: dynarec,cpu,state,exceptions
created: 2026-09-04
updated: 2026-09-04
---

`shared/amigaport` must own one canonical architectural context used by
translated blocks and the separately linked test interpreter. Benefactor must
adapt once to it rather than synchronizing a second reduced context. Positive
and negative instruction/exception tests must exercise the shipping decoder and
lowered blocks.
