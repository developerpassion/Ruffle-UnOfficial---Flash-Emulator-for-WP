@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio 12.0\VC\bin\x86_arm\vcvarsx86_arm.bat" >nul
link /nologo /DLL /NOENTRY /MACHINE:ARM /OUT:spike.dll  /INCLUDE:ruffle_create /INCLUDE:ruffle_destroy /INCLUDE:ruffle_tick /INCLUDE:ruffle_render /INCLUDE:ruffle_resize /INCLUDE:ruffle_get_framebuffer /INCLUDE:ruffle_mouse /INCLUDE:ruffle_mouse_button /INCLUDE:ruffle_key /INCLUDE:ruffle_char /INCLUDE:ruffle_audio_fill /INCLUDE:ruffle_set_storage /INCLUDE:ruffle_set_quality /LIBPATH:"%VCINSTALLDIR%lib\store\arm" /LIBPATH:"C:\Program Files (x86)\Windows Phone Kits\8.1\lib\arm" WpStubs.obj E:\Projects\Ruffle\ruffleUWP\lib\ARM\release\ruffle_uwp.lib E:\Projects\Ruffle\ruffleUWP\lib\ARM\release\ntdll_uwp.lib msvcrt.lib vccorlib.lib WindowsPhoneCore.lib runtimeobject.lib ws2_32.lib > link.txt 2>&1
echo exit=%ERRORLEVEL%
dumpbin /nologo /imports spike.dll > imports.txt
