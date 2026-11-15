#!/bin/sh
# Tạo lại icon UWP từ ../logo.svg:  sh scripts/make-icons.sh   (chạy trong ruffleUWP/)
set -e
cargo +nightly build --release -q --manifest-path tools/svg2png/Cargo.toml
X=tools/svg2png/target/release/svg2png.exe
A=app/RuffleUWP/Assets
while read n w h pad; do "$X" ../logo.svg "$A/$n.png" $w $h $pad; done <<'LIST'
Square44x44Logo.scale-100 44 44 0.04
Square44x44Logo.scale-200 88 88 0.04
Square44x44Logo.scale-400 176 176 0.04
Square44x44Logo.targetsize-24_altform-unplated 24 24 0.02
Square44x44Logo.targetsize-44_altform-unplated 44 44 0.04
Square44x44Logo.targetsize-48_altform-unplated 48 48 0.04
Square150x150Logo.scale-100 150 150 0.08
Square150x150Logo.scale-200 300 300 0.08
Square150x150Logo.scale-400 600 600 0.08
Wide310x150Logo.scale-100 310 150 0.1
Wide310x150Logo.scale-200 620 300 0.1
Wide310x150Logo.scale-400 1240 600 0.1
StoreLogo.scale-100 50 50 0.04
StoreLogo.scale-200 100 100 0.04
StoreLogo.scale-400 200 200 0.04
SplashScreen.scale-100 620 300 0.12
SplashScreen.scale-200 1240 600 0.12
LIST
echo "OK -> $A"
