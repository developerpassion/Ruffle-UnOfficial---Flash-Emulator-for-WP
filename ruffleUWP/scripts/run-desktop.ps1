# Chạy app UWP (x64, đã register) với một SWF, chờ, rồi lưu frame dump + log.
# Tạo LocalState\debug.flag để bật log/autoload; xoá khi xong (tham số -KeepDebug để giữ).
#   .\scripts\run-desktop.ps1 -Swf path\test.swf [-Seconds 6] [-Out frame.png]
# App tự nạp LocalState\autoload.swf và ghi frame.bmp sau ~2s chơi.
param([Parameter(Mandatory)][string]$Swf, [int]$Seconds = 6, [string]$Out = "", [switch]$KeepDebug)
$ErrorActionPreference = "Stop"
$pkg = "RuffleUWP.Port_0zchs43c1ject"
$ls = "$env:LOCALAPPDATA\Packages\$pkg\LocalState"
Get-Process RuffleUWP -EA SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
if (Test-Path "$ls\frame.bmp") { Remove-Item -LiteralPath "$ls\frame.bmp" -Force }
Copy-Item $Swf "$ls\autoload.swf" -Force
Set-Content "$ls\debug.flag" "1"
Start-Process "shell:AppsFolder\$pkg!App"
Start-Sleep $Seconds
$fs = [IO.File]::Open("$ls\log.txt", 'Open', 'Read', 'ReadWrite'); $r = New-Object IO.StreamReader -ArgumentList $fs
$r.ReadToEnd(); $fs.Close()
Get-Process RuffleUWP -EA SilentlyContinue | Stop-Process -Force
if (-not $KeepDebug) {
    foreach ($f in "debug.flag", "autoload.swf") {
        if (Test-Path "$ls\$f") { Remove-Item -LiteralPath "$ls\$f" -Force }
    }
}
if ($Out -and (Test-Path "$ls\frame.bmp")) {
    Add-Type -AssemblyName System.Drawing
    $b = [Drawing.Image]::FromFile("$ls\frame.bmp"); $b.Save($Out, [Drawing.Imaging.ImageFormat]::Png); $b.Dispose()
    "frame -> $Out"
}
