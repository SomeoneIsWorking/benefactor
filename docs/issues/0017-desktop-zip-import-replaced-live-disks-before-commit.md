---
id: 17
title: Desktop ZIP import replaced live disks before selection commit
status: resolved
symptom: A failed ZIP import could destroy the last installed desktop disk set.
state_items: S031
tags: desktop,disks,zip,persistence
created: 2026-09-12
updated: 2026-09-12
---

The SDL3 first-run flow extracted and validated a ZIP, then removed the existing
`disk-set` directory before renaming the import into place. It only wrote the
new `disk-selection.txt` afterward. If the directory rename or selection-file
write failed, the previous paths could point to deleted or replaced files.
An invalid persisted selection also left stale paths in the output array, so a
subsequent Browse attempt could be rejected as duplicate disks.

`DiskSelectionStore` now owns the storage transaction. It prepares a fresh
import, publishes to the inactive one of two fixed slots, and replaces the
selection file only after the new paths are complete. On publication or
persistence failure, the previous slot and selection remain untouched. Both
direct and ZIP resolvers build local candidates, publishing the output array
only after complete title-identity validation. The ZIP extraction is bounded
to 128 entries, 32 MiB archive, 16 MiB expanded, and 4 MiB per entry.

The focused synthetic-file test exercises successful publication, forced
selection-write failure with rollback, out-of-staging input rejection, and an
invalid direct path. A packaged GUI run and cross-platform file-replacement
behavior still need release-level verification under S031.
