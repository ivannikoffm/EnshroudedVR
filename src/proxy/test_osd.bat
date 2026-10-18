@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
cl /nologo /O2 /MT /EHsc /std:c++17 /utf-8 test_osd.cpp osd.cpp /Fobuild\ /Febuild\test_osd.exe user32.lib gdi32.lib || exit /b 1
build\test_osd.exe
