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

S005 is the current focus: establish `shared/amigaport` and integrate its
maintained 68000 interpreter without disturbing the existing native host owners.

## Capability inventory

| ID | Capability or outcome | State | Factual dependency | Goals |
| --- | --- | --- | --- | --- |
| S001 | The complete game boots, transitions among all runtime images, and plays all 60 levels through the intended product | partial | S005, S023 | G001, G002 |
| S002 | Native pause/options apply and persist modern settings in game | partial | S005 | G002 |
| S003 | Keyboard, hot-pluggable controllers, touch, rebinding, and alternate controls share logical actions | partial | S005; issue 0013 mapped the primary face button (`s_hop` / Pad A / Space under modern controls) to dismiss the level card into gameplay (`cop1lc = $003914`) and preserved real mouse clicks (`s_mouse_lmb_raw`) across bound input application | G002 |
| S004 | Android provides no-terminal disk setup without packaged game assets | partial | S022, S023 | G003 |
| S005 | Native owners plus `shared/amigaport` execute every non-native 68000 path directly from authenticated runtime images | partial | S020; the local Clang/headless run loads Disk.1-Disk.3, reaches `$577000`, and now runs the complete retail `=SB=` level-data dispatcher through the interpreter after native ATN loads. It sustains mixed input in gameplay after the shared JSR subroutine-boundary fixes and the `$1E.w` gameplay-mode-word normalisation (issue 0006 — the attract and direct-to-gameplay hand-offs entered `$577000` with a stale mode word, tripping the demo-input branch). Both the attract path and `--level` + fire injection now reach the level-intro card and run in-level frames without an illegal-instruction exit. Issue 0007 closed the interpreter's boundary and timing gaps. Issue 0009 established clean cooperative frame handoffs by adding scanline wait and displaced blitter idiom recognition to `src/port/wait_idiom.h`, registering wait idioms for all overlays, and eliminating mid-instruction beam-yielding and boundary-holding compensatory hacks (`hw_boundary_hold`, `s_boundary_owed`, `hw_present_paused_frame`). Issue 0010 resolved subroutine original call boundaries across native overrides and restored classic jump/fall/walk physics and in-game savestates. Issue 0011 resolved scanline wait presentation and consolidated override calling into a single unified `rt_call` across host and interpreter dispatch; issue 0014 moved the JSR/BSR-versus-JMP/BRA boundary choice into the Benefactor title adapter. Issue 0012 resolved nested override host-exit propagation across `rt_call` frames, fixing a watchdog hang at PC `$00000622` (`cop1lc = $0000097E`) when selecting Continue / Play Game from the title menu. Issue 0013 isolated interrupt vectors ($6C, $78) across overlay boundaries, removed an invalid subroutine override on music branch $59C5B0, and unified level-card dismissal inputs across modern and vanilla schemes. Issue 0015 restored the complete retail level-data dispatcher and sample-bank integrity on level-1 entry. A headless Disk.1-Disk.3 run boots unaided through the attract sequence, enters level 1 on fire, and moves the player over 11,000+ frames with no runtime errors; a windowed run holds a steady 50 fps. Four-image, platform, and performance conformance remain open | G001 |
| S006 | The working static recomp provides independent behavioral comparison for the shipping interpreter | partial | S005; `origin/oracle` and `tools/oracle_diff.py` reach level 1 and identify the first animation/SFX frame-phase difference in issue 0016. The static product is separate from the shipping build; representative first-divergence coverage remains open | G001 |
| S007 | Turbo, hyper, and hold-to-fast-forward change gameplay pace while audio remains at normal speed | partial | S005 | G002 |
| S008 | Optional platformer physics provides variable jump, air control, momentum, and tunable motion while classic physics remains | partial | S005; issues 0010 and 0011 unified call boundaries in `src/port/overrides/platformer.c`, restoring classic jump trajectories, falling, landing, directional hopping, and long jumps to match the reference oracle | G002 |
| S009 | A 60-level selector, completion progress, and locks replace password entry | partial | S005 | G002 |
| S010 | Faithful, native software, and Vulkan renderers provide selectable lighting and shadow effects | partial | S005 | G002 |
| S011 | 16:9, 21:9, and live-window widescreen reveal additional simulated world rather than stretching | partial | S005, S010 | G002 |
| S012 | Native boot and disk/decompression owners remove Amiga startup and floppy waits | partial | S005 | G002 |
| S013 | Free camera pans in real time or while paused | partial | S005, S011 | G002 |
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
| S025 | Asset-free source-policy CI runs from a full-history checkout | verified | Hosted run `34696922050` passed the locked source-policy verifier from a full-history checkout with pinned shared runtime and SDL3 headers, without game inputs. | G003 |
| S026 | Windows CI produces an asset-free package from the native/interpreter product | verified | Hosted release run `34691039895`, Windows job `103546198321`, uploaded the MinGW package after its PE import gate found no unbundled runtime DLLs. | G004 |
| S027 | macOS CI produces an Apple Silicon `.app` from the native/interpreter product | verified | Hosted release run `34691039895`, macOS job `103546198331`, built and uploaded the CMake bundle with pinned shared runtime, SDL3, and Lucent inputs. | G004 |
| S028 | Linux CI produces an asset-free x86-64 AppImage | verified | Hosted release run `34691039895`, Linux job `103546198266`, built and uploaded the disk-free AppImage after verifying the pinned appimagetool. | G003, G004 |
| S029 | Android CI produces a signed arm64-v8a release APK | partial | Hosted release run `34691039895` assembled an arm64-v8a APK with a CI-only ephemeral key. Manual signing-verification run `34694150527` passed Android job `103554610317` using the existing persistent release secrets and checking against the v0.1.0 public signer fingerprint; a hosted tagged build, device performance, and gameplay evidence remain open. | G003, G004 |
| S030 | WASM builds and deploys the same product boundary to GitHub Pages | verified | Source run `34700630248` built the asset-free package at `dbcfdd7`; central Pages run `34700756608` deployed it, and the live `publication.json` names that commit and source run. Browser gameplay execution remains open under S023. | G004 |
| S031 | Desktop and browser first-run setup browse for and validate the three player disks | partial | S004, S026-S030 | G004 |
| S032 | A qualified version tag publishes one GitHub Release with all four native packages | partial | S023, S026-S031; the tag-only publisher stages Windows ZIP, macOS `.app` ZIP, AppImage, and signed APK after all five CI jobs and an explicit state gate; no qualified tag has exercised publication | G004 |

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

Android imports a validated three-disk set into private storage without
packaging the disks through the shared `android-port` SAF importer. The retired
AppImage shell setup flow is no longer a shipping implementation.

Gap: prove the Android path through the native/interpreter product and keep
desktop setup in S031.

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
frame 463; the frame-350 composed image is pixel-identical to the oracle. PCM
first differs in frame 464, so later audio conformance remains open.

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

Evidence: `CMakeLists.txt` now owns a real `benefactor_web` Emscripten target. Its browser
entry mounts the three validated disk files into the production disk path before
calling `pc_init_from_disk`; `tools/build_wasm.py` requires real JS/WASM outputs.
The source workflow uploads the package as a normal CI artifact; the sibling
`pages` repository owns publication. Source run `34700630248` passed its WASM
job and central Pages run `34700756608` deployed that artifact. The live
`/benefactor/publication.json` confirms source commit `dbcfdd7` and the run.
WebLua loaded that deployment with an unrestricted multi-file disk input and
no failed network requests.

Gap: browser gameplay execution and real disk boot remain unverified locally and
under S023.

### S031 — Cross-platform disk browse

Android has a shared `android-port` SAF directory browser that validates the three filenames
before promotion. The browser package omits the HTML `accept` attribute so
numeric-suffix `Disk.1`, `Disk.2`, and `Disk.3` files are not MIME-filtered; it accepts
those three files directly or one bounded ZIP containing them at any folder
depth, then validates names, archive safety, byte sizes, and SHA-256 identities
before dispatching a committed selection to the WASM bridge. The locked source
launcher also exposes `./run.sh --browse` through a native Tk file picker and
validates the same set before building or launching the product.

The browser runtime is one-shot. Its chooser now allows another try only when
disk validation fails before committing to the guest filesystem; once native
startup is attempted, it requires a page reload to change disks or retry a
failed start. A five-case Node regression gate exercises the shipping picker
script's success, failure, and overlapping-selection paths (issue 0020).

The packaged SDL3 desktop setup has a native multi-file dialog and verifies
the exact disk set before persisting it. ZIP imports now publish through two
fixed install slots: a focused synthetic-file test confirms that failed
persistence leaves the previous slot and selection intact, and that a later
successful import commits the complete new set. This covers storage behavior,
not a packaged first-run UI run.

On the deployed Pages route, WebLua/CDP selected the player's `Disk.1` through
the unrestricted input and received the expected missing-Disk.2/3 validation
message. A ZIP lacking the disk set reached the ZIP-content validator. Neither
selection started the game or changed the last valid disk set. This proves the
browser input accepts both file types, not the operating system's chooser UI or
a complete browser game boot.

On the current deployment, WebLua again selected the player's `Disk.1`; the
missing-two-disks error re-enabled the chooser without committing a set. A
local ZIP containing all three authenticated disks reached the WASM boot path
(audio device opened and disk boot began), but WebLua lost its Chrome DevTools
heartbeat and self-shut down, closing its isolated Chromium process. That
harness behavior does not establish whether the browser page or Chromium failed
first. This run cannot establish gameplay, ZIP-start success, or one-shot
behavior in a running browser.

Gap: packaged desktop first-run/reselection UX, the deployed browser's native
file-dialog behavior, and end-to-end browser/native runtime handoff remain
unverified on release artifacts.

### S032 — Qualified GitHub Release

The tag-only `tools.publish_release` gate requires the core gameplay, player
setup, signing, and platform state items to be verified. It refuses a branch
run or incomplete package set, inspects the downloaded ZIP/APK entries for
unsafe or player-owned paths, and stages SHA-256 checksums for the four native
packages. The macOS package is a ZIP containing `Benefactor.app` with its
executable mode preserved. Focused positive and negative tests pass.

Gap: no tagged workflow has passed the release gate; S023, S029, and S031
remain unverified, so a new GitHub Release must not publish yet.
