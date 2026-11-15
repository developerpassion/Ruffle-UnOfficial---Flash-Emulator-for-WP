# Tạo icon/splash cho app WP 8.1 (scale-100 và scale-240) từ icon của ruffleUWP.
Add-Type -AssemblyName System.Drawing
$root = Split-Path -Parent $PSScriptRoot
$src = Join-Path $root "..\ruffleUWP\app\RuffleUWP\Assets"
$dst = Join-Path $root "wp81\RuffleWP81\Assets"
New-Item -ItemType Directory -Force $dst | Out-Null

function Make($in, $out, $w, $h, $splash) {
    $img = [System.Drawing.Image]::FromFile((Resolve-Path (Join-Path $src $in)).Path)
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = "HighQualityBicubic"
    if ($splash) {
        $g.Clear([System.Drawing.ColorTranslator]::FromHtml("#37528C"))
        $s = [Math]::Min($w, $h) * 0.6
        $g.DrawImage($img, [int](($w - $s) / 2), [int](($h - $s) / 2), [int]$s, [int]$s)
    } else {
        $g.DrawImage($img, 0, 0, $w, $h)
    }
    $bmp.Save((Join-Path $dst $out), [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose(); $img.Dispose()
}

Make "Square150x150Logo.scale-200.png" "Square150x150Logo.scale-100.png" 150 150 $false
Make "Square150x150Logo.scale-200.png" "Square150x150Logo.scale-240.png" 360 360 $false
Make "Square150x150Logo.scale-200.png" "Square71x71Logo.scale-100.png" 71 71 $false
Make "Square150x150Logo.scale-200.png" "Square71x71Logo.scale-240.png" 170 170 $false
Make "Square44x44Logo.scale-200.png" "Square44x44Logo.scale-100.png" 44 44 $false
Make "Square44x44Logo.scale-200.png" "Square44x44Logo.scale-240.png" 106 106 $false
Make "StoreLogo.scale-200.png" "StoreLogo.scale-100.png" 50 50 $false
Make "StoreLogo.scale-200.png" "StoreLogo.scale-240.png" 120 120 $false
Make "Wide310x150Logo.scale-200.png" "Wide310x150Logo.scale-100.png" 310 150 $false
Make "Wide310x150Logo.scale-200.png" "Wide310x150Logo.scale-240.png" 744 360 $false
Make "Square150x150Logo.scale-200.png" "SplashScreen.scale-100.png" 480 800 $true
Make "Square150x150Logo.scale-200.png" "SplashScreen.scale-240.png" 1152 1920 $true
Get-ChildItem $dst | Select-Object Name, Length

# ---- WP 8.0 (Silverlight): ApplicationIcon 100, tile Flip 159/336/691x336, splash 768x1280 (jpg)
$dst = Join-Path $root "wp80\RuffleWP80\Assets"
New-Item -ItemType Directory -Force (Join-Path $dst "Tiles") | Out-Null
Make "Square150x150Logo.scale-200.png" "ApplicationIcon.png" 100 100 $false
Make "Square150x150Logo.scale-200.png" "Tiles\FlipCycleTileSmall.png" 159 159 $false
Make "Square150x150Logo.scale-200.png" "Tiles\FlipCycleTileMedium.png" 336 336 $false
Make "Wide310x150Logo.scale-200.png" "Tiles\FlipCycleTileLarge.png" 691 336 $false
Make "Square150x150Logo.scale-200.png" "FileSwf33.png" 33 33 $false
Make "Square150x150Logo.scale-200.png" "FileSwf69.png" 69 69 $false
Make "Square150x150Logo.scale-200.png" "FileSwf176.png" 176 176 $false
Make "Square150x150Logo.scale-200.png" "..\SplashScreenImage.png" 768 1280 $true
$png = Join-Path $root "wp80\RuffleWP80\SplashScreenImage.png"
$img = [System.Drawing.Image]::FromFile($png)
$img.Save((Join-Path $root "wp80\RuffleWP80\SplashScreenImage.jpg"), [System.Drawing.Imaging.ImageFormat]::Jpeg)
$img.Dispose(); Remove-Item $png
Get-ChildItem -Recurse $dst | Select-Object Name, Length

# ---- Silverlight 8.1: trình đóng gói .xap không đọc file liên kết -> chép thật ảnh 8.0 + logo appx (scale-240)
$sl = Join-Path $root "wp81sl\RuffleWP81SL"
New-Item -ItemType Directory -Force (Join-Path $sl "Assets\Tiles") | Out-Null
Copy-Item (Join-Path $root "wp80\RuffleWP80\Assets\*.png") (Join-Path $sl "Assets") -Force
Copy-Item (Join-Path $root "wp80\RuffleWP80\Assets\Tiles\*.png") (Join-Path $sl "Assets\Tiles") -Force
Copy-Item (Join-Path $root "wp80\RuffleWP80\SplashScreenImage.jpg") $sl -Force
foreach ($n in "Square150x150Logo", "Square71x71Logo", "Square44x44Logo", "StoreLogo", "Wide310x150Logo") {
    Copy-Item (Join-Path $root "wp81\RuffleWP81\Assets\$n.scale-240.png") (Join-Path $sl "Assets") -Force
}
