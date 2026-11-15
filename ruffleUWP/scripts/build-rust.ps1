# Build ruffle_uwp staticlib cho target UWP (tier 3 -> nightly + build-std).
# Ví dụ:  .\scripts\build-rust.ps1 -Target thumbv7a-uwp-windows-msvc -Release
# -TargetCpu (tuỳ chọn, vd. kryo = Snapdragon 820): tối ưu riêng một CPU, có thể KHÔNG chạy trên máy cũ
#   (Lumia 650 = Cortex-A7). Mặc định để trống: bản chung chạy mọi máy W10M ARM.
param(
    [ValidateSet("thumbv7a-uwp-windows-msvc", "x86_64-uwp-windows-msvc", "i686-uwp-windows-msvc", "aarch64-uwp-windows-msvc")]
    [string]$Target = "x86_64-uwp-windows-msvc",
    [switch]$Release,
    [string]$Toolchain = "nightly",
    [string]$TargetCpu = ""
)
$ErrorActionPreference = "Stop"
$env:PATH = "$env:USERPROFILE\.cargo\bin;$env:PATH"
$root = Split-Path -Parent $PSScriptRoot
$crate = Join-Path $root "rust\ruffle_uwp"

$cargoArgs = @("+$Toolchain", "build", "--manifest-path", "$crate\Cargo.toml",
          "--target", $Target, "-Z", "build-std=std,panic_abort")
if ($Release) { $cargoArgs += "--release" }
# Thư mục target riêng cho mỗi CPU để không phải build lại khi đổi qua lại
$targetDir = if ($TargetCpu) { Join-Path $crate "target\cpu-$TargetCpu" } else { Join-Path $crate "target" }
$cargoArgs += @("--target-dir", $targetDir)
$env:RUSTFLAGS = if ($TargetCpu) { "-C target-cpu=$TargetCpu" } else { "" }

Write-Host "cargo $($cargoArgs -join ' ')"
& cargo @cargoArgs
if ($LASTEXITCODE -ne 0) { throw "cargo build failed" }

$buildProfile = if ($Release) { "release" } else { "debug" }
$platform = switch ($Target) {
    "thumbv7a-uwp-windows-msvc" { "ARM" }
    "x86_64-uwp-windows-msvc"   { "x64" }
    "i686-uwp-windows-msvc"     { "Win32" }
    "aarch64-uwp-windows-msvc"  { "ARM64" }
}
$out = Join-Path $root "lib\$platform\$buildProfile"
New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item (Join-Path $targetDir "$Target\$buildProfile\ruffle_uwp.lib") $out -Force
Write-Host "-> $out\ruffle_uwp.lib"
