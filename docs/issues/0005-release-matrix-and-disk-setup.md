# Release matrix and user disk setup

State: open

Affected state: S004, S026, S027, S028, S029, S030, S031

The project needs real hosted artifact jobs for Windows, macOS, Linux
AppImage, Android arm64-v8a, and WASM on GitHub Pages. Each package must be
asset-free and route first-run setup to a user-facing disk browser that accepts
the exact three supported disk identities. The release builders, web selection
contract, and a real Benefactor adapter to the pinned `shared/amigaport`
runtime are now present. A local Clang product build and Linux AppDir staging
pass without player files; hosted artifact and runtime-conformance gates remain
open.

Resolution requires the native/interpreter product to boot authenticated disks
on each claimed host, hosted jobs to produce each artifact, desktop setup to
persist OS user data, Android setup to retain its SAF import contract, and the
browser bridge to pass only a fully validated disk set into the WASM runtime.
