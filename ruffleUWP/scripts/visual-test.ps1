# Chạy software renderer trên các test có output.expected.png của Ruffle và in % pixel lệch.
#   .\scripts\visual-test.ps1 [-Filter "avm1/*"] [-Max 100]
param([string]$Filter = "*", [int]$Max = 200)
$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
$crate = Join-Path $root "rust\ruffle_uwp"
$swfs = Join-Path $root "..\ruffle\tests\tests\swfs"
$out = Join-Path $root "test-out"
New-Item -ItemType Directory -Force $out | Out-Null
& cargo build --release --example render_png --manifest-path "$crate\Cargo.toml" 2>&1 | Out-Null
$exe = "$crate\target\release\examples\render_png.exe"
Add-Type -AssemblyName System.Drawing
$results = @()
Get-ChildItem $swfs -Recurse -Filter output.expected.png | Where-Object {
    $rel = $_.Directory.FullName.Substring((Resolve-Path $swfs).Path.Length + 1) -replace "\\", "/"
    $rel -like $Filter
} | Select-Object -First $Max | ForEach-Object {
    $dir = $_.Directory.FullName
    $rel = $dir.Substring((Resolve-Path $swfs).Path.Length + 1) -replace "\\", "/"
    $frames = 1
    $toml = Join-Path $dir "test.toml"
    if (Test-Path $toml) {
        $m = Select-String -Path $toml -Pattern "^num_frames\s*=\s*(\d+)" | Select -First 1
        if ($m) { $frames = [int]$m.Matches[0].Groups[1].Value }
    }
    $img = [System.Drawing.Image]::FromFile($_.FullName); $w = $img.Width; $h = $img.Height; $img.Dispose()
    $png = Join-Path $out (($rel -replace "/", "_") + ".png")
    $line = & $exe "$dir\test.swf" $png $frames $w $h $_.FullName 2>&1 | Select-String "^DIFF" | Select -First 1
    $diff = if ($line) { ($line.ToString() -split " ")[1] } else { "CRASH" }
    $results += [pscustomobject]@{ Test = $rel; Diff = $diff }
}
$results | Sort-Object { if ($_.Diff -match "^[\d.]+%$") { [double]($_.Diff -replace "%", "") } else { 999 } } -Descending | Format-Table -AutoSize
$ok = ($results | Where-Object { $_.Diff -match "^[\d.]+%$" -and [double]($_.Diff -replace "%", "") -lt 1.0 }).Count
Write-Host "Pass (<1% pixel lệch): $ok / $($results.Count)"
