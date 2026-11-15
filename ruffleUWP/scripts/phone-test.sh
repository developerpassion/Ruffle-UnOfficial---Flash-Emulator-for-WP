#!/bin/sh
# Build ARM với các cờ chẩn đoán, ký, cài lên điện thoại cắm USB (Device Portal bật), mở app và theo dõi tiến trình.
#   sh scripts/phone-test.sh [/p:NoGpu=true] [/p:NoXAudio=true] [/p:SafeMode=true]
cd "$(dirname "$0")/.."
"/f/Program Files/Microsoft Visual Studio/2017/Professional/MSBuild/15.0/Bin/MSBuild.exe" RuffleUWP.sln //p:Configuration=Release //p:Platform=ARM //p:AppxPackageSigningEnabled=false //t:Rebuild //m //v:minimal //nologo "$@" 2>&1 | grep -E " error " | head -5
ST=$(ls "/c/Program Files (x86)/Windows Kits/10/bin/"*/x64/signtool.exe | tail -1)
A=AppPackages/RuffleUWP/RuffleUWP_0.1.1.0_ARM_Test/RuffleUWP_0.1.1.0_ARM.appx
"$ST" sign //fd SHA256 //f app/RuffleUWP/RuffleUWP_TemporaryKey.pfx //p ruffle "$(cygpath -w $A)" 2>&1 | tail -1
cp app/RuffleUWP/RuffleUWP_TemporaryKey.cer "$(dirname $A)/RuffleUWP_0.1.1.0_ARM.cer"  # Rebuild xoá thư mục gói -> chép lại cert
W="/c/Program Files (x86)/Windows Kits/10/bin/10.0.15063.0/x86/WinAppDeployCmd.exe"
timeout 300 "$W" uninstall -package RuffleUWP.Port_0.1.1.0_arm__0zchs43c1ject -ip 127.0.0.1 2>&1 | tail -1
timeout 300 "$W" install -file "$(cygpath -w $A)" -ip 127.0.0.1 2>&1 | tail -1
B=http://127.0.0.1:10080
APPID=$(printf "%s" "RuffleUWP.Port_0zchs43c1ject!App" | base64 -w0); PKG=$(printf "%s" "RuffleUWP.Port_0.1.1.0_arm__0zchs43c1ject" | base64 -w0)
( curl -s -m 40 -X POST -d "" "$B/api/taskmanager/app?appid=$APPID&package=$PKG"; echo " <- launch" ) &
for i in $(seq 1 25); do r=$(curl -s -m 3 "$B/api/resourcemanager/processes" | grep -o '"ImageName" : "RuffleUWP[^"]*"' | head -1); echo "${r:-none}"; sleep 0.3; done | uniq -c
wait
