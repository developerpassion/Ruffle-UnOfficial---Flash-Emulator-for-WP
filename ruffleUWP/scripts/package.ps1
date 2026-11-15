# Build toàn bộ (Rust + app) và ký gói .appx bằng chứng chỉ test.
#   .\scripts\package.ps1 -Platform ARM      (Lumia / W10M)
#   .\scripts\package.ps1 -Platform x64      (test trên desktop)
param([ValidateSet("ARM", "x64")][string]$Platform = "ARM", [switch]$SkipRust)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$env:PATH = "$env:USERPROFILE\.cargo\bin;$env:PATH"

if (-not $SkipRust) {
    $target = if ($Platform -eq "ARM") { "thumbv7a-uwp-windows-msvc" } else { "x86_64-uwp-windows-msvc" }
    & "$PSScriptRoot\build-rust.ps1" -Target $target -Release
}

# Import lib ntdll tối giản (NtClose, NtQuerySystemTime...) cho UwpStubs.cpp; SDK không có ntdll.lib ARM.
# Đặt cạnh ruffle_uwp.lib (lib\<Platform>\release), vcxproj link "ntdll_uwp.lib".
$vcTools = "F:\Program Files\Microsoft Visual Studio\2017\Professional\VC\Tools\MSVC"
$libExe = (Get-ChildItem (Join-Path $vcTools "*\bin\HostX64\x64\lib.exe") | Select-Object -Last 1).FullName
$defFile = Join-Path $root "app\RuffleUWP\ntdll_uwp.def"
$ntdllOut = Join-Path $root "lib\$Platform\release\ntdll_uwp.lib"
& $libExe /nologo "/def:$defFile" "/machine:$Platform" "/out:$ntdllOut"
if ($LASTEXITCODE -ne 0) { throw "lib ntdll_uwp failed" }

# VS2017 (toolset v141) là bản duy nhất trên máy có thư viện ARM32 cho UWP
$msbuild = "F:\Program Files\Microsoft Visual Studio\2017\Professional\MSBuild\15.0\Bin\MSBuild.exe"
& $msbuild "$root\RuffleUWP.sln" /p:Configuration=Release /p:Platform=$Platform /p:AppxPackageSigningEnabled=false /m /v:minimal /nologo
if ($LASTEXITCODE -ne 0) { throw "MSBuild failed" }

$appx = "$root\AppPackages\RuffleUWP\RuffleUWP_0.1.1.0_${Platform}_Test\RuffleUWP_0.1.1.0_$Platform.appx"
$signtool = (Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" | Select-Object -Last 1).FullName
# Chứng chỉ TEST (mật khẩu "ruffle") — không dùng cho phát hành
& $signtool sign /fd SHA256 /f "$root\app\RuffleUWP\RuffleUWP_TemporaryKey.pfx" /p ruffle $appx
if ($LASTEXITCODE -ne 0) { throw "sign failed" }
# Chép chứng chỉ (.cer, chỉ có public key) cạnh .appx để cài lên máy trước khi cài app
$cer = Join-Path (Split-Path $appx) "RuffleUWP_0.1.1.0_$Platform.cer"
Copy-Item "$root\app\RuffleUWP\RuffleUWP_TemporaryKey.cer" $cer -Force
Write-Host "OK -> $appx"
Write-Host "Cert -> $cer"
Write-Host "Cài lên điện thoại (Developer mode + Device discovery):"
Write-Host "  WinAppDeployCmd install -file `"$appx`" -ip <IP_DIEN_THOAI> -pin <PIN>"
