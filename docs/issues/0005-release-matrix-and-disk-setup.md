# Release matrix and user disk setup

State: open

Affected state: S004, S026, S027, S028, S029, S030, S031

The project needs real hosted artifact jobs for Windows, macOS, Linux
AppImage, Android arm64-v8a, and WASM on GitHub Pages. Each package must be
asset-free and route first-run setup to a user-facing disk browser that accepts
the exact three supported disk identities. The release builders and web
selection contract are now present, but all builders correctly stop at the
missing `shared/amigaport`/Benefactor runtime adapter until S005 is complete.

Resolution requires the native/interpreter product to build on each claimed
host, desktop setup to call the native file picker and persist OS user data,
Android setup to retain its SAF import contract, and the browser bridge to pass
only a fully validated disk set into the WASM runtime.
