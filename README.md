# Ruffle for Windows 10 Mobile & Windows Phone 8.x

<p align="center"><img src="logo.svg" width="320" alt="Ruffle"></p>

Unofficial port of the [Ruffle](https://github.com/ruffle-rs/ruffle) Flash Player emulator to
**Windows 10 Mobile (UWP, ARM)**, **Windows Phone 8.1** and **Windows Phone 8.0**.
Play `.swf` games and animations on old Lumia / HP Elite x3 / HTC One M8 devices.

> [!IMPORTANT]
> **This is NOT an official Ruffle repository.** It is an independent, community port that *uses*
> the open-source [Ruffle](https://ruffle.rs/) emulator ([source](https://github.com/ruffle-rs/ruffle)).
> It is not affiliated with, endorsed by, or supported by the Ruffle team — please do not report
> issues of this port to the Ruffle project.
>
> Ruffle itself is not modified — this repo only contains a thin Rust wrapper crate,
> a native host (C++/CX, C#) and a Direct3D 11 renderer.

## Features

- Runs SWF content (AVM1 / AVM2) via `ruffle_core`
- **Direct3D 11 GPU renderer** (feature level 9_1 – 11_1), CPU software renderer as fallback
- XAudio2 sound, touch → mouse, long-press → right-click menu, two-finger zoom/pan
- Keyboard (hardware + on-screen), landscape rotation
- Save games (SharedObject) stored in `LocalState`, offline web cache for `http://` loads
- Settings: GPU/CPU, MSAA, render resolution, quality, frame limit, FPS overlay, sound

## Tested devices

| Device | OS | Package | Status |
|---|---|---|---|
| HP Elite x3 (Snapdragon 820) | W10M | `ruffleUWP` ARM `.appx` | Runs smoothly (GPU) |
| Lumia 650 (Snapdragon 212) | W10M 10586 | `ruffleUWP` ARM `.appx` | Runs |
| HTC One M8 | WP 8.1 | `wp80` / `wp81sl` `.xap` | Runs (GPU, sound, rotation) |
| Lumia (WP 8.0) | WP 8.0 | `wp80` `.xap` | Core test passes |
| Desktop x64 | Windows 10/11 | `ruffleUWP` x64 `.appx` | Runs (dev/testing) |

## Repository layout

```
ruffleUWP/            Windows 10 / 10 Mobile UWP app
  rust/ruffle_uwp/    Rust crate wrapping ruffle_core -> static lib + C FFI
  include/            C header for the FFI (ruffle_uwp.h)
  app/RuffleUWP/      C++/CX XAML app, D3D11 renderer, HLSL shaders, XAudio2
  scripts/            build / package / test scripts (PowerShell)
ruffleWP8/            Windows Phone 8.x ports (reuse the Rust lib built by ruffleUWP)
  wp80/               WP 8.0 Silverlight (.xap) + C++ component
  wp81/               WP 8.1 C++/CX (.appx)
  wp81sl/             WP 8.1 Silverlight (.xap, with file picker)
  shim/               CRT / Win32 compatibility stubs for WP 8.x
```

Each folder has `PLAN.md`, `WORK.md`, `PROGRESS.md` (development notes, Vietnamese).

## Building

### Requirements

- Rust **nightly** + `rust-src` (uses `-Z build-std` for `thumbv7a-uwp-windows-msvc` / `x86_64-uwp-windows-msvc`)
- **Visual Studio 2017** (v141 with ARM support) for `ruffleUWP` — VS2019/2022 have no ARM32 UWP libs
- Windows 10 SDK 15063 (min version 10586)
- **Visual Studio 2015** + VS2012/VS2013 toolsets + Windows Phone 8.0/8.1 SDKs for `ruffleWP8`
- Java (needed by Ruffle's `build_playerglobal`)

### Steps

1. Clone upstream Ruffle **next to** this repo's `ruffleUWP` folder (the crate uses `../../../ruffle/...` paths):
   ```
   git clone https://github.com/ruffle-rs/ruffle.git ruffle
   ```
   Tested with Ruffle 0.7.1.
2. Build the Rust static lib and the app package:
   ```powershell
   cd ruffleUWP
   .\scripts\package.ps1 -Platform ARM    # phone
   .\scripts\package.ps1 -Platform x64    # desktop
   ```
3. **Signing**: the W10M test certificate is included (`app/RuffleUWP/RuffleUWP_TemporaryKey.pfx`,
   password `ruffle`; install `RuffleUWP_TemporaryKey.cer` on the device to trust it). It is a
   self-signed *test* key only. For WP 8.1 create your own in Visual Studio
   (Package.appxmanifest → Packaging → Choose Certificate → Create test certificate) and save it as
   `ruffleWP8/wp81/RuffleWP81/RuffleWP81_TemporaryKey.pfx`.
4. WP 8.x: build `ruffleUWP` ARM first (produces `ruffleUWP/lib/`), then open
   `ruffleWP8/wp80/RuffleWP80.sln`, `wp81/RuffleWP81.sln` or `wp81sl/RuffleWP81SL.sln` in VS2015, `Release|ARM`.

### Installing on a phone

- W10M: enable Developer mode, then `WinAppDeployCmd install -file <appx> -ip 127.0.0.1` (USB) or Device Portal.
- WP 8.x: Application Deployment tool (`AppDeploy.exe`) from the Windows Phone SDK.

## Known issues

See [ISSUES.md](ISSUES.md).

## Acknowledgements

A huge **thank you to the [Ruffle](https://ruffle.rs/) team and all Ruffle contributors**.
All the hard work — the SWF parser, the AVM1/AVM2 virtual machines, rendering, audio and
everything that makes Flash content run — is theirs. This project only brings their work to
old Windows phones. If you enjoy it, please support and star the official project:

- Website: https://ruffle.rs/
- GitHub: https://github.com/ruffle-rs/ruffle

## License

This project is released under the [MIT License](LICENSE).

Ruffle is dual-licensed under MIT / Apache-2.0 by the Ruffle contributors.
Flash content (SWF files) belongs to their respective authors and is **not** included in this repository.
