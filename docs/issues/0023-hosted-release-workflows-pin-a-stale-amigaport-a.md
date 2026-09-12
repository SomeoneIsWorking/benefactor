---
id: 23
title: Hosted release workflows pin a stale amigaport API
status: resolved
symptom: The local verifier passes, but the hosted source-policy and release workflows compile against a shared runtime older than the product API and fail at clang-tidy on continue_original.
state_items: S025,S026,S027,S028,S029,S030
tags: ci,release,amigaport
created: 2026-09-12
updated: 2026-09-12
---

Root cause: both workflow files checked out e9fe2251ff060ad65f6fb6bf7d1f9f5b56806c56, while Benefactor requires Executor::continue_original() from the later shared/amigaport commit 8050b197e14e9e4c583d255c80d958d8c859b4fa. Updated every workflow checkout to the exact pushed runtime commit. Local locked verifier passes with that runtime. Hosted rerun is required to prove the repair.
