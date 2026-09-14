---
id: 32
title: CI actions are pinned to revisions that target the deprecated Node.js 20 runtime
status: open
symptom: Every hosted run logs: Node.js 20 is deprecated. The following actions target Node.js 20 but are being forced to run on Node.js 24: actions/checkout@11d5960a326750d5838078e36cf38b85af677262, actions/upload-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02
tags: ci,maintenance,dependency
created: 2026-09-14
updated: 2026-09-14
---

## What is known

Both release-workflow actions are pinned to revisions that still ship a Node 20
entry point, and every hosted run prints the deprecation notice that they are being
forced onto Node.js 24:

- `actions/checkout@11d5960a326750d5838078e36cf38b85af677262` (7 uses in `release.yml`)
- `actions/upload-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02`
- `actions/setup-python@5fda3b95a4ea91299a34e894583c3862153e4b97` (not named in the notice)

Latest releases at the time of writing: checkout `v7.0.1`, upload-artifact `v7.0.1`
(`043fb46d1a93c77aae656e7c1c64a875d1fc6a0a`), download-artifact `v8.0.1`
(`3e5f45b2cfb9172054b4087a40e8e0b5a5461e7c`).

## Why it matters

A deprecation deadline becomes a hard failure: the workflow stops running at all,
which takes the release path with it. The pins must stay full commit SHAs — a
moving tag is what pinning exists to avoid — so each bump needs the tag's exact
commit.

## Resolution

Not started. Resolve each action to the commit behind its released tag, update
every workflow that uses it (including `source-policy.yml` and the sibling `pages`
repository), keep the YAML valid, and let a hosted run prove the new pins before
considering it done.
