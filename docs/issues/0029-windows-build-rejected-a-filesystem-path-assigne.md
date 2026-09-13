---
id: 29
title: Windows build rejected a filesystem path assigned to a string
status: resolved
symptom: MinGW build of the setup flow failed: no match for operator= between std::string and std::filesystem::path
state_items: S031
tags: build,windows,portability,setup
created: 2026-09-14
updated: 2026-09-14
---

## Root cause

`SetupFlow::Impl::view_options()` assigned the setup screen's font path straight
into a `std::string`:

```cpp
screen.font_path = options.font_path;   /* std::filesystem::path -> std::string */
```

`std::filesystem::path` has no conversion to `std::string` on any platform — it
converts to `string_type`, which is `std::wstring` where `path::value_type` is
`wchar_t`. That is Windows, so MinGW rejects the assignment while Linux accepts it
silently, because there `path::value_type` is `char` and the implicit conversion
lands in `std::string` by accident. The Linux verifier and the local Clang build
therefore could not see it; the Windows CI job could.

## Resolution

The assignment calls `.string()`, the path's own narrow-encoding conversion, which
is what every other site in `src/platform/` already did
(`android_setup.cpp`, `desktop_setup.cpp`, `disk_selection_store.cpp`,
`staged_disks.cpp`, `web_setup.cpp`). The single missing call was the whole defect.

Checked locally with a Windows cross-compiler rather than by waiting for CI: the
bare assignment fails under `i686-w64-mingw32-g++` and `.string()` compiles, and
`-fsyntax-only` over the Windows-relevant `src/platform/` sources is clean
(`android_setup.cpp` and `web_setup.cpp` exclude themselves by including `jni.h`
and `emscripten.h`). Worth repeating whenever a Windows job reports a conversion
error the Linux gate cannot reproduce.
