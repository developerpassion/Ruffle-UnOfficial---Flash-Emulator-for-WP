# Known issues

Status as of 2026-11. Details and history are in `ruffleUWP/PROGRESS.md` and `ruffleWP8/PROGRESS.md`.

## Rendering

- [ ] **Filters not implemented** (blur, glow, drop shadow, bevel...) in both GPU and CPU renderers.
- [ ] **Stage3D, PixelBender, video** are not supported.
- [ ] Blend modes **Overlay / HardLight** (and Difference on GPU) are drawn as Normal.
- [ ] CPU renderer has **no anti-aliasing** (jagged edges); device fonts / text differ slightly from Flash.
- [ ] Visual test: ~178/353 Ruffle reference tests match (<1% pixel diff); most failures are the unsupported features above.
- [ ] GPU feature level 9_x: textures limited to 4096 (9_3) / 2048 (9_1); MSAA disabled below FL 10_0.

## Windows 10 Mobile (ruffleUWP)

- [ ] **Back button = exit app.** Subscribing to `SystemNavigationManager.BackRequested` crashes natively on
      Lumia 650 (10586), so custom Back handling is disabled.
- [ ] On-screen keyboard does not open automatically when a TextField gets focus — use the Keyboard button.
- [ ] Long-press right-click menu and touch interactions need more testing on devices.
- [ ] Performance on low-end devices (Lumia 650) is limited — no JIT, AVM1/AVM2 are interpreted.
- [ ] Rust std `raw-dylib` imports: new nightly versions may import kernel32/ntdll functions missing on 10586
      → app closes right after splash. Stubs live in `app/RuffleUWP/UwpStubs.cpp`.

## Windows Phone 8.x (ruffleWP8)

- [ ] WP 8.0 `.xap` player not yet re-tested on a real WP 8.0 device (tested on 8.1 / HTC One M8).
- [ ] `wp81sl` (Silverlight 8.1 with file picker) not tested on device.
- [ ] On-screen keyboard and long-press right-click menu not implemented yet.
- [ ] WP 8.0 can only list `.swf` files from the SD card (no Documents/Downloads access, no file picker).
- [ ] WP 8.0: `Fls*` is emulated — TLS destructors don't run (small leak when threads exit).
- [ ] Low RAM (512 MB) devices may terminate the app (tombstone); the game then restarts from the beginning.
- [ ] Debug builds of the WP 8.0 component must use the release CRT (device has no `MSVCR110D`).
- [ ] Probe build (`/p:Probe=true`) does not work on WP 8.0 (`LoadPackagedLibrary` unavailable).

## Build

- [ ] ARM32 UWP requires **VS2017** (VS2019/2022 lack ARM32 libs); WP 8.x requires VS2015 + VS2012/2013 toolsets.
- [ ] Depends on a specific Rust nightly with `-Z build-std`; newer nightlies may break the build or device compatibility.
- [ ] `ruffleUWP/lib/` (Rust static lib, ~200 MB) is not committed — rebuild with `scripts/build-rust.ps1`.
- [ ] WP 8.1 signing certificate is not included — create your own test certificate (W10M test cert is included).
