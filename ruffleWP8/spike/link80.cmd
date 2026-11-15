@echo off
set W=C:\Program Files (x86)\Microsoft Visual Studio 11.0\VC\WPSDK\WP80
set K=C:\Program Files (x86)\Windows Phone Kits\8.0
set PATH=%W%\bin\x86_arm;%W%\bin;C:\Program Files (x86)\Microsoft Visual Studio 11.0\Common7\IDE;%PATH%
cl /nologo /c /O2 /MD /ZW /EHsc /DWINAPI_FAMILY=WINAPI_FAMILY_PHONE_APP /DRUFFLE_WP80 /I"%W%\include" /I"%K%\Include" /I"%K%\Include\minwin" /I"%K%\Include\mincore" /I"%K%\Include\abi" /AI"%W%\lib\arm" /AI"%K%\Windows Metadata" ..\shim\WpStubs.cpp /FoWpStubs80.obj > cl80.txt 2>&1
cl /nologo /c /O2 /MD /EHsc /DWINAPI_FAMILY=WINAPI_FAMILY_PHONE_APP /DRUFFLE_WP80 /I"%W%\include" /I"%K%\Include" /I"%K%\Include\minwin" /I"%K%\Include\mincore" /I"%K%\Include\abi" /AI"%W%\lib\arm" /AI"%K%\Windows Metadata" ..\shim\WpCompat80.cpp /FoWpCompat80.obj >> cl80.txt 2>&1
echo cl=%ERRORLEVEL%
link /nologo /DLL /NOENTRY /MACHINE:ARM /OUT:spike80.dll /INCLUDE:ruffle_create /INCLUDE:ruffle_destroy /INCLUDE:ruffle_tick /INCLUDE:ruffle_render /INCLUDE:ruffle_resize /INCLUDE:ruffle_get_framebuffer /INCLUDE:ruffle_mouse /INCLUDE:ruffle_mouse_button /INCLUDE:ruffle_key /INCLUDE:ruffle_char /INCLUDE:ruffle_audio_fill /INCLUDE:ruffle_set_storage /INCLUDE:ruffle_set_quality /LIBPATH:"%W%\lib\arm" /LIBPATH:"%K%\lib\arm" WpStubs80.obj WpCompat80.obj E:\Projects\Ruffle\ruffleUWP\lib\ARM\release\ruffle_uwp.lib E:\Projects\Ruffle\ruffleUWP\lib\ARM\release\ntdll_uwp.lib msvcrt.lib vccorlib.lib WindowsPhoneCore.lib runtimeobject.lib ws2_32.lib > link80.txt 2>&1
echo link=%ERRORLEVEL%
dumpbin /nologo /imports spike80.dll > imports80.txt 2>&1
