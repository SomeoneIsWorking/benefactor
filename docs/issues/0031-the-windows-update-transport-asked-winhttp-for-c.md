---
id: 31
title: The Windows update transport asked WinHTTP for components it never returned
status: resolved
symptom: >-
  On Windows the release check reported "the release address is not usable" and
  the pause menu showed UPDATE CHECK FAILED, while the same policy on Linux
  reported UP TO DATE. The MinGW compile of the WinHTTP source was clean, so
  only a real request showed it.
state_items: [S034]
tags: [windows, update-check, transport]
created: 2026-09-14
updated: 2026-09-14
---

## Root cause

The WinHTTP transport derived the host and path from the release URL by calling
`WinHttpCrackUrl` with every `lpsz*` component left NULL and its length left 0,
expecting pointers into the URL string back. WinHTTP does not return a component
that way: with the pointer NULL it reports the component's length and leaves the
pointer NULL, so `lpszHostName` stayed NULL and the transport refused to make a
request. Measured under Wine with the same source: `WinHttpCrackUrl` returned
success, `nScheme`/`nPort` filled, and `dwHostNameLength`/`dwUrlPathLength` both
0.

The deeper cause was the design, not the call: a transport should not parse a
URL at all when the title already knows its release host and path. The policy now
exposes the parts (`pc_update_release_host`, `pc_update_release_path`) alongside
the composed URL, so no host parses anything and no host can ask a different
service.

## What was tried / dead ends

- Syntax-checking the Windows source with `i686-w64-mingw32-g++ -fsyntax-only`.
  It passed both before and after the fix: a compile cannot see this class of
  defect, which is why it shipped broken in the first place.
- Setting `dwStructSize` on the `URL_COMPONENTS` before the call. Necessary, and
  it did not change the outcome: the components were still never returned.
- Filling caller-provided buffers with `WinHttpCrackUrl` instead of taking
  pointers into the URL. Rejected: it needs a magic buffer size, and the title
  already owns these two strings.

## Resolution

`src/port/update_check.{h,cpp}` owns the release host and path as the single
authority and composes the URL from them; `src/platform/winhttp_update.cpp` asks
for the parts directly and no longer parses anything. `tests/winhttp_transport.cpp`
runs the transport against the real release service and fails unless an older
build is told a newer release exists, which requires the tag to have been both
received and parsed.

That check now runs two ways, so this defect cannot return unobserved:

- `tools/check_windows_update_transport.py`, run by the Windows CI job on real
  Windows, wired into `release.yml`.
- `tools/verify.py`, which cross-compiles the same sources and runs them under
  Wine whenever `i686-w64-mingw32-g++` and `wine` are installed, and otherwise
  says the check did not run here rather than reporting it as passing.

Verified locally under Wine: `info: update: winhttp transport: OK` with the
resulting state `PC_UPDATE_AVAILABLE` and the line `UPDATE v0.2.0 AVAILABLE` for
a build claiming version 0.0.1.
