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

Root cause: both workflow files checked out 25779223d8e763bedf7b36872a6bef8ef7e82eb9, while Benefactor requires Executor::continue_original() from the later shared/amigaport commit 724411733b59c313c7ca0afb906bb4ebe7f4a8fc. Updated every workflow checkout to the exact pushed runtime commit. Local locked verifier passes with that runtime. Hosted rerun is required to prove the repair.
