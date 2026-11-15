@echo off
set W=C:\Program Files (x86)\Microsoft Visual Studio 11.0\VC\WPSDK\WP80
set K=C:\Program Files (x86)\Windows Phone Kits\8.0
set PATH=%W%\bin\x86_arm;%W%\bin;C:\Program Files (x86)\Microsoft Visual Studio 11.0\Common7\IDE;%PATH%
cd gen
for %%f in (probe_*.c) do (
  lib /nologo /machine:ARM /def:%%~nf_imp.def /out:%%~nf_imp.lib >nul
  cl /nologo /c /O1 /GS- /DWINAPI_FAMILY=WINAPI_FAMILY_PHONE_APP %%f /Fo%%~nf.obj >nul
  link /nologo /DLL /NOENTRY /NODEFAULTLIB /MACHINE:ARM /OUT:%%~nf.dll %%~nf.obj %%~nf_imp.lib >nul || echo FAIL %%f
)
dir /b probe_*.dll
