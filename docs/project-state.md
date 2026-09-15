# Project state

This is the factual capability inventory for the intended Benefactor
native/interpreter product. Goals are in `docs/project-goals.md`, execution order
and gates in `docs/migration.md`, and atomic work in `docs/issues/`.

## Comparison baseline

The user-facing baseline is the unmodified 1994 Amiga release under a
conventional emulator: 320-pixel presentation, original controls and jump
behavior, password flow, floppy timing, and Amiga startup.

The implementation baseline is this repository's native host plus
offline-generated 68000-to-C execution. That execution method is retired from
the product, but the working `origin/oracle` branch remains a separate local
behavioral oracle for interpreter comparisons. It is never a shipping path or
build input for this tree.

## Current focus

S001 and S005 are the current focus, with S023 as their gate: `shared/amigaport` is
established and its interpreter already executes the retail game from the player's
disks, so what remains is coverage rather than integration — driving the complete
game through every level and recording where the native/interpreter path diverges
or stalls, then holding that play through the conformance and performance gate.
Issue 0028's reported level-progression freeze lives here and is still waiting on
the reporter's entry route, but the general sweep does not need it.

## Capability inventory

| ID | Capability or outcome | State | Factual dependency | Goals |
| --- | --- | --- | --- | --- |
| S001 | The complete game boots, transitions among all runtime images, and plays all 60 levels through the intended product | partial | S005, S023 | G001, G002 |
| S002 | Native pause/options apply and persist modern settings in game | partial | S005; the panel draws this build's version beside its title, and an EXTRA-page `UPDATE CHECK` row toggles the release check that S034 performs and reports (verified on the `codex_shared_api35` emulator) | G002 |
| S003 | Keyboard, hot-pluggable controllers, touch, rebinding, and alternate controls share logical actions | partial |  S005; issue 0013 mapped the primary face button (`s_hop` / Pad A / Space under modern controls) to dismiss the level card into gameplay (`cop1lc = $003914`) and preserved real mouse clicks (`s_mouse_lmb_raw`) across bound input application The touch overlay routes through the shared `touch-ui` layout (rotation-safe: the layout is reported whenever the window's pixel size changes, because an Android window reports its pre-rotation surface on the first frame); issue 0030 corrected the `/state` pause probe. The shared overlay's discs and glyphs grew (unit fraction, icon fraction, and held-icon fraction all raised), a held control now draws a gold ring and a filled disc, and two controls were added: turbo (hold-to-fast-forward, the same logical action the keyboard and pad hold) and camera (the same free-camera edge every other device sends), both verified on the emulator. While the pause menu is open the overlay withdraws the controls that screen has no use for (turbo, camera, and use), because on a narrow device their discs cover the panel's values. Verified on the `codex_shared_api35` emulator: the on-screen pause control opens the PAUSED panel, D-pad touches move the cursor between rows, the action control activates the highlighted row and returns the game to gameplay. The release APK re-checked this on the emulator through the control channel: tapping the pause control set `paused=1`, the PAUSED panel appeared with the gameplay-only controls (turbo, camera, use) withdrawn, a held control drew its gold ring, and the panel's highlighted RESUME row returned the game to gameplay (`paused=0`). The desktop binary holds the pause menu open as a runtime invariant, not a screenshot: `/menu` returns `{"menu":1}` and three consecutive `/state` reads 1.5 s apart each report `paused=1`, which falsifies the earlier build's open-then-close behaviour that predated the escape-close fix.| G002 |
| S004 | Android provides no-terminal disk setup without packaged game assets | verified | S022; the shared in-app setup screen (`shared/setup-ui`) renders inside the game window and opens Android's own document picker for the three images or one ZIP. Verified on the persistent `codex_shared_api35` emulator: a three-file multi-select and a `benefactor-disks.zip` import each published an identity-checked set into private app storage and booted the game; a single-image selection reports the still-missing disks with Start disabled and does not publish; two Android Back presses cancel back to the screen with nothing published; the activity's staged copy is released after the flow and abandoned session staging is pruned. Physical-device re-verification rides with S023 | G003 |
| S005 | Native owners plus `shared/amigaport` execute every non-native 68000 path directly from authenticated runtime images | partial | S020; the local Clang/headless run loads Disk.1-Disk.3, reaches `$577000`, and now runs the complete retail `=SB=` level-data dispatcher through the interpreter after native ATN loads. It sustains mixed input in gameplay after the shared JSR subroutine-boundary fixes and the `$1E.w` gameplay-mode-word normalisation (issue 0006 — the attract and direct-to-gameplay hand-offs entered `$577000` with a stale mode word, tripping the demo-input branch). Both the attract path and `--level` + fire injection now reach the level-intro card and run in-level frames without an illegal-instruction exit. Issue 0007 closed the interpreter's boundary and timing gaps. Issue 0009 established clean cooperative frame handoffs by adding scanline wait and displaced blitter idiom recognition to `src/port/wait_idiom.h`, registering wait idioms for all overlays, and eliminating mid-instruction beam-yielding and boundary-holding compensatory hacks (`hw_boundary_hold`, `s_boundary_owed`, `hw_present_paused_frame`). Issue 0010 resolved subroutine original call boundaries across native overrides and restored classic jump/fall/walk physics and in-game savestates. Issue 0011 resolved scanline wait presentation and consolidated override calling into a single unified `rt_call` across host and interpreter dispatch; issue 0014 moved the JSR/BSR-versus-JMP/BRA boundary choice into the Benefactor title adapter. Issue 0012 resolved nested override host-exit propagation across `rt_call` frames, fixing a watchdog hang at PC `$00000622` (`cop1lc = $0000097E`) when selecting Continue / Play Game from the title menu. Issue 0013 isolated interrupt vectors ($6C, $78) across overlay boundaries, removed an invalid subroutine override on music branch $59C5B0, and unified level-card dismissal inputs across modern and vanilla schemes. Issue 0015 restored the complete retail level-data dispatcher and sample-bank integrity on level-1 entry. A headless Disk.1-Disk.3 run boots unaided through the attract sequence, enters level 1 on fire, and moves the player over 11,000+ frames with no runtime errors; a windowed run holds a steady 50 fps. Issue 0026 closed the level-complete stall: the banner's bodyless `tst.b $BFE001; bmi self` fire poll is now a recognised wait kind that yields a frame per test and then lets the guest's own test and branch decide, verified by completing level 1 into a running level 2 with no watchdog report. Four-image, platform, and performance conformance remain open | G001 |
| S006 | The working static recomp provides independent behavioral comparison for the shipping interpreter | partial | S005; `origin/oracle` and `tools/oracle_diff.py` reach level 1, and the title-owned first-gameplay frame handoff now closes the direct animation/SFX phase identified in issue 0016. The static product is separate from the shipping build; representative first-divergence coverage remains open | G001 |
| S007 | Turbo, hyper, and hold-to-fast-forward change gameplay pace while audio remains at normal speed | partial | S005; the on-screen turbo control holds the same logical fast-forward action, measured on the emulator at 215 frames in two seconds against a 101-frame baseline | G002 |
| S008 | Optional platformer physics provides variable jump, air control, momentum, and tunable motion while classic physics remains | partial | S005; issues 0010 and 0011 unified call boundaries in `src/port/overrides/platformer.c`, restoring classic jump trajectories, falling, landing, directional hopping, and long jumps to match the reference oracle | G002 |
| S009 | A 60-level selector, completion progress, and locks replace password entry | partial | S005 | G002 |
| S010 | Faithful, native software, and Vulkan renderers provide selectable lighting and shadow effects | partial | S005 | G002 |
| S011 | 16:9, 21:9, and live-window widescreen reveal additional simulated world rather than stretching | partial | S005, S010; issue 0027 gave the presenter its own opaque black clear, so the area beside the game frame no longer inherits the touch overlay's fill colour (emulator samples read (0,0,0) where they read (242,247,250) before) | G002 |
| S012 | Native boot and disk/decompression owners remove Amiga startup and floppy waits | partial | S005 | G002 |
| S013 | Free camera pans in real time or while paused | partial | S005, S011; the on-screen camera control sends the same free-camera edge as the keyboard, and under the vanilla renderer the existing refusal explains that the software or hardware renderer is required (verified on the emulator) | G002 |
| S014 | Easy, Normal, and Hard selection is restored in the main menu | partial | S005 | G002 |
| S015 | Skip intro, unlock all levels, and reduced/disabled fall damage are configurable | partial | S005 | G002 |
| S016 | Pickup and interaction reach can be extended independently of control scheme | partial | S005 | G002 |
| S017 | Savestates, direct level entry, headless driving, profiling, and runtime probes support development | partial | S020; issue 0010 enabled CLI argument forwarding in `tools/config.py` and `tools/launcher.py` (`./run.sh --level 1 --headless`), accepted scanline wait `$577130` as a valid savestate resume point, and verified in-game save and restore via `/save` and `/load` HTTP endpoints | G002 |
| S018 | Player-facing save slots provide names, timestamps, and screenshot previews | missing | S017 | G002 |
| S019 | Hold-to-rewind restores recent states from a bounded history | missing | S017, S020 | G002 |
| S020 | `shared/amigaport` supplies complete 68000 PC/SR/exception/cycle state and a maintained interpreter owner | partial | Shared runtime tests cover interrupt/RTE and original-subroutine boundaries, the unterminated-override guard, native-continuation image re-authorization, and the retired-instruction trace; the Benefactor disk run boots to the menu and plays, while title conformance and ARM64 gameplay evidence remain open | G001 |
| S021 | Image-generation-aware execution, overrides, and scoped original calls work across all four address-reusing images | partial | S005; adapter binds image tags/generations, native registrations, interrupt calls, title-owned `GuestCallPolicy` boundaries, unified `rt_call` dispatch with scoped self-suppression, and opaque CPU snapshots; calls and continuations now reject a stale image token, and the host rebinds its context before IRQ delivery; issue 0012 propagates host hand-off exits across nested override calls; four-image runtime evidence remains open | G001, G003 |
| S022 | Gameplay uses one shared interpreter CPU owner and contains no generated/static execution or direct diagnostic-emulator dependency | partial | S005; the Clang product target links only `amigaport::amigaport`, and an authenticated Disk.1-Disk.3 run reaches `$577000`; title-wide conformance and symbol/build audit remain open | G001, G003 |
| S023 | Representative interactive gameplay conforms and meets performance gates through native/interpreter execution on x86-64, Apple Silicon macOS, and Android arm64-v8a | missing | S005, S021, S022 | G001, G002, G003 |
| S024 | Offline translator, generated corpus/dispatcher, generation-only seeds, and static-only tests are absent | verified | — | G001, G003 |
| S025 | Asset-free source-policy CI runs from a full-history checkout | verified | Hosted run `34704766301` passed the locked source-policy verifier from a full-history checkout with the current pinned shared runtime and SDL3 headers, without game inputs. | G003 |
| S026 | Windows CI produces an asset-free package from the native/interpreter product | verified | Hosted release run `34704766306`, Windows job `103582713796`, uploaded the MinGW package after its PE import gate found no unbundled runtime DLLs. | G004 |
| S027 | macOS CI produces an Apple Silicon `.app` from the native/interpreter product | verified | Hosted release run `34704766306`, macOS job `103582713792`, built and uploaded the CMake bundle with current pinned shared runtime, SDL3, and Lucent inputs. | G004 |
| S028 | Linux CI produces an asset-free x86-64 AppImage | verified | Hosted release run `34704766306`, Linux job `103582713702`, built and uploaded the disk-free AppImage after verifying the pinned appimagetool. | G003, G004 |
| S029 | Android CI produces a signed arm64-v8a release APK | partial | Hosted release run `34704766306`, Android job `103582713838`, assembled an arm64-v8a APK with a CI-only ephemeral key. Manual signing-verification run `34694150527` passed Android job `103554610317` using the existing persistent release secrets and checking against the v0.1.0 public signer fingerprint; a hosted tagged build, device performance, and gameplay evidence remain open. | G003, G004 |
| S030 | WASM builds and deploys the same product boundary to GitHub Pages | verified | Source run `34708373281` built the asset-free pthread-capable package from `4f41501`; Pages run `34708732681` deployed it from Pages commit `3d415f4`. A fresh WebLua session at the live route reported secure context, cross-origin isolation, and an active service worker, created the SDL canvas, and continued rendering past frame 500. The browser product now renders the shared in-canvas setup screen and runs the same staged-set validator as desktop and Android; that package boots from a local headless-Chrome ZIP import but has not been re-deployed to the live route Release run `34793537255` built the package from `9b66ca5` (the v0.3.0 tag commit) and the Pages route serves it: `publication.json` names that commit and run, and `release_check.js` answers 200. Browser gameplay remains unqualified as its own item records.| G004 |
| S031 | Desktop and browser first-run setup browse for and validate the three player disks | partial | S004, S026-S030; desktop and browser now render the same in-app setup screen over the same staged-set resolver, and a headless-Chrome run imported a `benefactor-disks.zip` through the screen, published an identity-checked set, and booted the game. No desktop run has exercised the ZIP path end to end, and no browser run has exercised a multi-file selection (the browser chooser is one document picker whose multi-file result the bridge already covers by test) | G004 |
| S032 | A version tag publishes one GitHub Release with all four native packages | verified | S026-S031; the tag-only publisher stages Windows ZIP, macOS `.app` ZIP, AppImage, and signed APK after all five CI jobs; no tag has exercised publication Release `v0.3.0` is published from run `34793537255` with all four native packages attached — Windows ZIP (5.5 MB), macOS ZIP (2.3 MB), x86_64 AppImage (5.0 MB), and the signed arm64-v8a APK (4.2 MB) — plus `SHA256SUMS`; the Windows job also passed the runtime-import gate after winhttp.dll was allowed as a system component.| G004 |
| S033 | One version identifies the whole product: the build banner, the pause panel, the Android package, and the published tag all name the same release | verified | S032; `version.txt` is the source of truth. Desktop runs log `Benefactor 0.3.0`, the pause panel draws `v0.3.0` beside its title (verified on the `codex_shared_api35` emulator and in-device screenshots), `aapt2 dump badging` reports `versionCode='300' versionName='0.3.0'` for the built APK, and the publisher now refuses a tag that is not `v` + that file The release APK itself was installed on the `codex_shared_api35` emulator (built from the clean, pinned shared checkouts) and its PAUSED panel shows `v0.3.0` beside `UP TO DATE (v0.3.0)`.| G004 |
| S034 | The product asks the release service once per launch whether a newer version exists and reports the answer without ever claiming a check that did not run succeeded | partial | S033; the policy in `src/port/update_check.{h,cpp}` parses and compares the tag with `lucent::version`, and one line names each state. Verified on the desktop (libcurl) and on the `codex_shared_api35` emulator (the activity's own HTTP client, results crossing back through JNI): both logged `update: CHECKING FOR UPDATES...` then `update: UP TO DATE (v0.3.0)` against the published `v0.2.0` release, and the pause panel drew that line under its rows. `tests/test_update_policy.cpp` covers a newer tag, an equal one, an older one, a failure with a reason, an unparsable tag, a result reported from another thread, and a restarted check; the Windows transport is verified against the live service under Wine (`info: update: winhttp transport: OK`, state `PC_UPDATE_AVAILABLE`, line `UPDATE v0.2.0 AVAILABLE` for a build claiming 0.0.1) and now runs in the Windows CI job as well (issue 0031 records the defect that only a real request showed). The AVAILABLE and FAILED panel renderings are produced by the policy's own states and were driven on the desktop through `/update`; a device run of those two renderings remains open On that same release APK the live check against `api.github.com/repos/SomeoneIsWorking/benefactor/releases/latest` reported `UP TO DATE (v0.3.0)` in the panel's status line, with `v9.9.9` reporting AVAILABLE and a forced error reporting the specific reason. The exact artifact verified was the debug APK built at 03:21 from the clean pinned checkouts, sha256 `e3b2190f…`; it is a different artifact from the release APK, which CI signs.| G004 |
| S035 | Every shipped platform presents an authored app icon instead of a placeholder | verified | One mark is authored once — the game's own hero, amber head, arms and legs with his red tunic, standing on the teal water line, on the cave brown his levels are dug from (`platforms/icons/benefactor.svg`, drawn by `tools/draw_app_icon.py`) and every platform's form of it is generated from that: the Android adaptive layers plus the API 21-25 filled tile and a monochrome layer, the Windows `.ico` compiled into the executable by `benefactor.rc`, the macOS bundle `.icns`, and the AppImage/freedesktop SVG and browser favicon. `python -m tools.draw_app_icon --check` re-derives all ten authored files and reads both binary containers back. Verified by installing the built arm64-v8a APK on the `codex_shared_api35` emulator: `aapt2 dump badging` resolves `application` and every `application-icon-*` density to `res/mipmap-anydpi-v26/ic_launcher.xml`, and the launcher's app drawer draws the cave tile, the mark with its cut-out open, and the teal water line (measured on the gold key this artwork replaced, which the same generator drew). On the Windows side a local MinGW CMake configure compiled the resource through `enable_language(RC)` into a linked PE whose `.rsrc` section is the icon, and `tools.build_desktop.check_windows_icon` accepts that executable and refuses one without the icon; the macOS packager likewise refuses a bundle that does not hold and name `Benefactor.icns`. The mark is additionally measured rasterised at 16, 32, and 48 px over light, mid, and dark backgrounds, with a negative control (a bare tile) that fails the same measurement. Hosted run `34814462708`’s five platform jobs each passed with these gates naming what they found — `benefactor-pc.exe carries the 256x256 frame of the 9-size icon` and `Benefactor.app carries Benefactor.icns (132897 B) and names it in its Info.plist` — and the browser package that job produced holds `icon.svg` byte-identical to the authored mark, with the page linking it. Not yet exercised: an API 21-25 device (the pre-26 tile is covered by its committed resource and preview raster), a macOS `.icns` renderer (the container is verified structurally, by chunk inventory and PNG dimensions), and a launcher applying the themed monochrome layer on a device (the layer's own preview and the alpha hole between the hero's legs are inspected instead). | G004 |

## Capability details

For S002-S016, “Evidence” describes retained native implementation and the
last pre-migration observations. Each remains partial until the same behavior
is exercised through the native/interpreter product; deleted static execution is
not current verification.

### S001 — Complete game flow

The pre-migration native host has documented routes through the three disks,
all 60 levels, title/gameplay transitions, game over, and ending/credits.

Gap: that frontier has not been re-established through the intended
native/interpreter product; pre-migration runs are not current product
evidence.

### S002 — Native pause and options

Evidence: the in-game pause UI provides Resume, Options, Retry, Exit to main
menu, and Quit; supported Graphics, Controls, and Extra settings apply live and
persist.

Gap: re-exercise and verify the retained owner through S005's interpreter product.

### S003 — Unified input

Evidence: keyboard, SDL controller, and Android touch feed logical actions;
controllers hot-plug, bindings are captured in game, and classic/alternate
Interact and Drop policies are selectable per device.

Gap: re-exercise and verify the retained owner through S005's interpreter product.

### S004 — Packaged setup

Evidence: On the persistent `codex_shared_api35` Android emulator, the native/interpreter
`arm64-v8a` APK launched into the disk-requirement dialog, presented the system SAF
directory picker via `AndroidDocumentImport.pickTree`, accepted user-supplied `Disk.1`,
`Disk.2`, and `Disk.3` files from `/sdcard/Download/benefactor-emulator-test`, validated and
committed the set into private storage, and started gameplay in landscape mode with touch
controls. Subsequent launches detect the committed disks and boot straight into the intro
and title flow without re-prompting. The temporary emulator directory was cleared after
verification under the shared lock.

Desktop setup is independently tracked under S031.

### S005 — Native/interpreter execution

Evidence: `src/runtime/guest_runtime.cpp` owns the narrow Benefactor adapter to
`shared/amigaport`: canonical CPU views, checked big-endian guest memory,
OCS/CIA forwarding, image-qualified native registrations, interrupt/original
subroutine boundaries, and opaque CPU-state savestates. A local Clang/headless
run with the user-provided Disk.1-Disk.3 boots through the interpreter, reaches
`$577000`, performs native ATN segment loads, and runs the retail `=SB=`
level-data dispatcher through the interpreter before entering gameplay. The Clang CMake product links this
adapter with no generated guest corpus or diagnostic PUAE target.

Issue 0015 restored the complete retail `$59DC02` stream dispatcher after a
native `=SB=` shortcut bypassed its setup and corrupted the level-1 sample
bank. A frame-indexed level-1 run now preserves the oracle's sample bytes and
matches its initial gameplay audio-register sequence and stereo PCM through
the transition; issue 0016 added the title-owned first-gameplay display-frame
handoff so the direct level-1 SFX/PCM event is no longer one frame early. The
frame-350 composed image is pixel-identical to the oracle, while broader
natural-flow audio and representative gameplay conformance remain open.

Gap: representative gameplay correctness/performance, Android arm64, browser
execution, and the four-image conformance gate remain open. Issue #1 remains
open for those checks.

### S006 — Independent oracle and control

The working static recomp is available separately on `origin/oracle` and
`tools/oracle_diff.py` drives local comparisons without restoring generated
source into the product. Its level-1 comparison has isolated the first
animation and SFX frame-phase difference (issue 0016).

Gap: extend deterministic first-divergence comparisons across representative
interactive gameplay, audio, image reloads, and credits.

### S007 — Speed controls

Evidence: Normal, Turbo, Hyper, and 5x hold-to-fast-forward change gameplay
pacing while the audio clock remains wall-time based.

Gap: re-exercise and verify pacing and audio through S005's interpreter product.

### S008 — Alternate physics

Evidence: Classic and Platformer policies are live-selectable; the native model
owns variable-height rise/fall, air steering, momentum, terminal velocity,
trampoline hand-off, collision feedback, animation, and sound side effects.

Gap: re-exercise and verify both policies through S005's interpreter product.

### S009 — Level selection

Evidence: the main menu exposes all 60 levels by world, persists completion,
marks finished levels, and enforces locks without password entry.

Gap: re-exercise and verify the retained flow through S005's interpreter product.

### S010 — Renderer choices and effects

Evidence: the running host switches among faithful, native software, and Vulkan
renderers and exposes ambient darkness and character drop shadows.

Gap: rebuild and verify every renderer through S005's interpreter product.

### S011 — True widescreen

Evidence: curated 16:9 and ultrawide captures plus native view/camera/object
routes show additional simulated level tiles and actors with preserved source
geometry rather than a stretched final frame.

Gap: reproduce that evidence through S005's interpreter product.

### S012 — Native boot and loading

Evidence: the native disk/ATN owners enter title flow without Kickstart,
Workbench, or timed floppy I/O and preserve the loader/relocation effects.

Gap: connect those retained owners to S005's runtime memory/image boundary and
verify the product path.

### S013 — Free camera

Evidence: the Free Cam action detaches the widescreen camera, exposes an
indicator, pans horizontally, and supports running or paused policies.

Gap: re-exercise and verify the retained owner through S005's interpreter product.

### S014 — Difficulty selector

Evidence: left/right on Play Game cycles Easy, Normal, and Hard through the
original difficulty state.

Gap: re-exercise and verify the retained menu behavior through S005.

### S015 — Cheats and accessibility

Evidence: Extra options can skip the intro, unlock levels, and select vanilla,
light, or no fall damage while retaining landing animation/sound/state effects.

Gap: re-exercise and verify each retained option through S005's interpreter product.

### S016 — Interaction reach

Evidence: the Controls setting extends horizontal pickup/interaction windows
without changing vertical reach and applies to both control schemes.

Gap: re-exercise and verify the retained behavior through S005's interpreter product.

### S017 — Runtime diagnostics

The existing host supports save/load state, direct level entry, headless
execution, frame profiling, framebuffer/scene probes, and interactive control.

Gap: savestate and CPU inspection still need serialization through the complete
`amigaport` CPU state; diagnostics must then exercise the shipping interpreter
path.

### S018 — Save-slot UI

Missing capability: provide a player-facing slot browser with names,
timestamps, thumbnails, validation, and OS user-data storage.

### S019 — Rewind

Missing capability: provide a bounded recent-state history and hold-to-rewind
action over complete image-aware runtime state.

### S020 — Complete 68000 framework

Evidence: shared commit `a155d04` owns the CPU state, exception, timing, guest
RTE interrupt-call, and native-subroutine continuation APIs consumed by the
Benefactor adapter.

Gap: title conformance, representative gameplay, and Apple Silicon/Android
arm64 evidence remain open. Issue #2.

### S021 — Four-image runtime identity

Evidence: the adapter keys registrations by shared image identity, replaces the
identity on overlay changes, and routes scoped original calls through the shared
override suppression boundary. The host-owned subroutine continuation now gives
native-to-other `rt_call` targets their own guest return while retaining a
bounded stop boundary across intermediate exits (issue 0019); synthetic tests
and a Clang product build pass, but changed gameplay behavior is not yet verified.

Gap: execute and compare all four address-reusing images with authenticated
disks, including restore behavior and negative recursion checks. Issue #3.

### S022 — Single interpreter-owner product composition

Evidence: the native product target links `amigaport::amigaport` and the
Benefactor adapter only; generated sources, static dispatch, and the diagnostic
PUAE target are absent from the CMake product.

Gap: complete the runtime symbol/build audit and prove a real disk boot does not
enter another CPU owner.

### S023 — Representative conformance

Missing capability: pass the bounded interactive cavern scenario and four-image
transition checks in `docs/migration.md`, including complete CPU, memory,
exception/interrupt, timing, service, audio, frame, and host-performance
evidence. Issue #4.

### S024 — Static pipeline removed

Evidence: `tools/recomp/`, `src/engine/generated/`, `src/engine/rt.c`, static
generation/build scripts, generated-symbol calls, and the old delayed-removal
methodology are absent. `tools/source_policy.py` rejects their return.

### S025 — Asset-free CI

Evidence: hosted run `34200853713` checked out full Git history with read-only repository
permissions and pinned action revisions, installed no game disks, and passed the
locked retained-source verifier.

### S026 — Windows package

Evidence: the release workflow checks out pinned `amigaport`, SDL3, and Lucent
inputs, invokes `tools.build_desktop` on a native Windows runner, and uploads
the produced package only after inspecting its PE imports. Hosted run
`34691039895` passed job `103546198321`; `tools.check_windows_imports` verified
that no MinGW runtime DLL is required outside the ZIP.

Gap: disk boot and real-title package inspection remain open.

### S027 — macOS app bundle

Evidence: CMake owns a real macOS bundle target and the release workflow invokes
`tools.build_desktop` on macOS 14 with pinned shared inputs. Hosted run
`34691039895` passed job `103546198331`.

Gap: disk boot and real-title bundle inspection remain open.

### S028 — Linux AppImage

Evidence: the Clang product build and `tools.build_appimage --stage-only` produce a real
disk-free AppDir locally. The hosted workflow downloads and verifies the pinned
appimagetool before requiring an AppImage output; hosted run `34691039895`
passed job `103546198266`.

Gap: disk boot and real-title package inspection remain open.

### S029 — Android release APK

The workflow provisions pinned `amigaport`, `android-port`, and Lucent checkouts,
builds the shared SDL3 and Android application-framework prefix from the title's
profile, then invokes the Android builder for arm64-v8a and inspects the APK.
Hosted run `34691039895` passed job `103546198330` using an explicit CI-only
ephemeral test key. Existing GitHub signing secrets now feed version-tag builds;
the shared `apksigner` verifier passed on the published v0.1.0 APK and returned
its public certificate SHA-256 fingerprint, which the title builder checks for
future non-ephemeral release APKs.

Gap: the new tag-signing path still needs a hosted run with the existing key;
Android device performance and gameplay evidence are not yet available.

### S030 — WASM Pages delivery

Evidence: `CMakeLists.txt` owns a real `benefactor_web` Emscripten target.
`src/platform/web_setup.cpp` steps the shared setup flow on the browser's
animation frame, takes the page's picker result, and only after the set is
published calls `pc_init_from_disk`; `tools/build_wasm.py` requires real
JS/WASM outputs. The source workflow uploads the package as a normal CI
artifact and the sibling `pages` repository owns publication. Source run
`34708373281` built the pthread-capable package from `4f41501`; Pages run
`34708732681` deployed it from Pages commit `3d415f4`.

The live WebLua session reported secure context, `crossOriginIsolated=true`,
an active service worker, no failed network requests, and the SDL-owned canvas
at `704x564` with `image-rendering: pixelated`; it rendered a live frame
capture after frame 500. Issues 0024 and 0025 cover the pthread/isolation and
SDL canvas contracts that were required to reach that evidence.

A later headless-Chrome session at the local package drove the new in-canvas
screen: the setup context reported `crossOriginIsolated=true`, the screen drew
with the embedded Liberation Sans face, a real pointer click on Choose files
opened the page's chooser, and a `benefactor-disks.zip` import published
`disk-set-a` with a selection record and continued rendering past frame 900.
That run also produced the fix in `lucent::content::sha256_file`: its 64 KB
read buffer lived on the caller's stack, which a 64 KB browser worker stack
cannot hold, so identity validation aborted the page instead of hashing.

The remaining browser gap belongs to S023's title-wide conformance and
performance evidence, and to re-deploying this package to the live route; it is
not the WASM build boundary.

### S031 — Cross-platform disk browse

Android has a shared `android-port` SAF directory browser that validates the three filenames
before promotion. The browser package omits the HTML `accept` attribute so
numeric-suffix `Disk.1`, `Disk.2`, and `Disk.3` files are not MIME-filtered; it accepts
those three files directly or one bounded ZIP containing them at any folder
depth, then validates names, archive safety, byte sizes, and SHA-256 identities
before publishing a committed set. The locked source launcher also exposes
`./run.sh --browse` through a native Tk file picker and validates the same set
before building or launching the product; that remains a maintainer path, while
the player-facing first run uses the shared in-app setup screen.

The browser's own half is one job only: `platforms/web/disk_setup.js` opens a
single unrestricted multi-file chooser, writes the chosen documents into the
module's filesystem under a directory the native side names, and answers the
native pick once per change or cancellation. It decides nothing about identity
or archives — that is the same `validate_staged_disks` resolver the desktop and
Android products use. Picks are answered in order, so a second choice can never
mix its documents into the first answer. An eight-case Node gate exercises the
shipping script's ordering, byte budget, cancellation, late-choice-after-
dismissal, failed-write, missing-filesystem, and status paths; it found two real
defects, both fixed: a failed delivery left the native pick unanswered, and an
absent `Module.FS` crashed instead of being reported.

The packaged SDL3 desktop setup has a native multi-file dialog and verifies
the exact disk set before persisting it. ZIP imports now publish through two
fixed install slots: a focused synthetic-file test confirms that failed
persistence leaves the previous slot and selection intact, and that a later
successful import commits the complete new set. This covers storage behavior,
not a packaged first-run UI run.

The live deployment accepts numeric-suffix disk files without an HTML MIME
filter and accepts one bounded ZIP containing all three authenticated disks.
WebLua verified the ZIP path through the running pthread-backed browser target
and observed the SDL canvas rendering. The browser's one-shot selection rule
still preserves the committed set after success; the page asks the user to
reload before choosing a different set.

Gap: packaged desktop first-run/reselection UX and Android device evidence
remain unverified; the live browser file-dialog UI itself is still host-owned
and is not substituted by the validator.

### S032 — Tagged GitHub Release

The tag-only `tools.publish_release` refuses a branch
run or incomplete package set, inspects the downloaded ZIP/APK entries for
unsafe or player-owned paths, and stages SHA-256 checksums for the four native
packages. The macOS package is a ZIP containing `Benefactor.app` with its
executable mode preserved. Focused positive and negative tests pass.

The state-qualification gate was removed at the operator's direction: releases
are no longer blocked by conformance state items. Publication still requires a
pushed tag and a complete, inspected package set. Gap: no tag has exercised
publication end to end.
