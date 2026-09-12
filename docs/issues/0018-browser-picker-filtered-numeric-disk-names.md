---
id: 18
title: Browser picker filtered numeric disk names
status: investigating
symptom: The GitHub Pages file picker greyed out Disk.1, Disk.2, and Disk.3.
state_items: S031
tags: browser,disks,picker,wasm
created: 2026-09-12
updated: 2026-09-12
---

The browser input claimed to be unrestricted but specified `accept="*/*"`.
That remains a MIME-type filter; the original disks' numeric suffixes need
not receive a recognized MIME type from the host file dialog. The page now
omits `accept` entirely. A markup test checks the actual file input has no
filter and still allows multiple files.

The code change is not yet proof that a deployed browser/OS picker enables the
player's files. Verify the Pages artifact in a real file dialog, then confirm
that all three authenticated disks reach the WASM runtime and only then mark
the issue resolved.

Hosted release run `34689816660` built the updated WASM package and deployed
it. An isolated browser session at the live `/benefactor/` route observed the
new `<input type="file" multiple>` with no `accept` attribute, whereas the
previous deployment exposed `accept=".1,.2,.3,.zip,..."`. The network and
console error buffers were empty. This proves the live filter is removed; an
actual OS file-dialog selection and validated-disk gameplay boot remain open.
