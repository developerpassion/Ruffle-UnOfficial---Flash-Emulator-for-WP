@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio 12.0\VC\bin\x86_arm\vcvarsx86_arm.bat" >nul
set KI=C:\Program Files (x86)\Windows Phone Kits\8.1\Include
set PW=C:\Program Files (x86)\Microsoft SDKs\WindowsPhoneApp\v8.1\ExtensionSDKs\Microsoft.VCLibs\12.0\References\CommonConfiguration\neutral
set WD=C:\Program Files (x86)\Windows Phone Kits\8.1\References\CommonConfiguration\Neutral
cl /nologo /c /O2 /MD /ZW /EHsc /DWINAPI_FAMILY=WINAPI_FAMILY_PHONE_APP /I"%VCINSTALLDIR%include" /I"%KI%" /I"%KI%\minwin" /I"%KI%\mincore" /I"%KI%\abi" /AI"%PW%" /AI"%WD%" ..\shim\WpStubs.cpp /FoWpStubs.obj
