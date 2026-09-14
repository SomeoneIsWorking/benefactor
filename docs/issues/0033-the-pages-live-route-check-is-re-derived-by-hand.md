---
id: 33
title: The Pages live-route check is re-derived by hand at every release
status: open
symptom: Each release re-runs: fetch the CI artifact, import it, curl publication.json, hash every served file, and load the page with WebLua. The page opens a native picker with no <input type=file>, so the setup screen can be reached headlessly but not completed.
tags: verification,release,pages
created: 2026-09-14
updated: 2026-09-14
---

## What is known

The v0.3.0 publication was verified by hand, and each step worked and is worth
keeping:

- `publication.json` on the live route names the source commit and run.
- Every served file is byte-compared against the CI artifact (all seven matched).
- WebLua loads the route and reaches the setup screen, whose title is
  "Benefactor setup".

The last step cannot be driven further: the page declares no file input, because
the chooser is opened natively, so an automated run stops at setup. That is a real
boundary of the method, not a defect — but a script that pretends otherwise (an
earlier one uploaded to `#disk-files`, which no longer exists) is exactly the
drifted pre-baked script the working rules warn about.

## Why it matters

The release rules require the Pages entry and the deployed artifact to be verified
after every release, and the sibling `pages` repository owns the import tooling but
not this end-to-end check. Re-deriving it by hand each time invites a partial check
that passes without touching the route.

## Resolution

Not started. A project tool should own it: compare the deployed files against a
named CI artifact by hash, assert `publication.json` agrees, and drive the route
with WebLua when a browser is available — reporting the setup-screen boundary
explicitly instead of skipping it silently.
