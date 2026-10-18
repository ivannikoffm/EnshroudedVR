@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build_public mkdir build_public
cl /nologo /O2 /DEVR_PUBLIC /MT /EHsc /std:c++17 /W3 /I..\third_party\vulkan\.. /I..\third_party /I..\third_party\openxr\include ^
   dinput8_proxy.cpp cam_probe.cpp hwbp.cpp camobj.cpp camhook.cpp xr.cpp osd.cpp /LD /Fobuild_public\ /Febuild_public\dinput8.dll ^
   /link /DEF:dinput8.def user32.lib kernel32.lib psapi.lib
exit /b %errorlevel%
