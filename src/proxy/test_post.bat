@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
lib /nologo /def:vulkan-1.def /machine:x64 /out:build\vulkan-1.lib >nul || exit /b 1
cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /I..\third_party test_post.cpp /Fobuild\ /Febuild\test_post.exe /link /LIBPATH:build || exit /b 1
build\test_post.exe
exit /b %errorlevel%
