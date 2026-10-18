@echo off

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul

cd /d "%~dp0"

cl /nologo /O2 /EHsc /std:c++17 test_scan.cpp cam_probe.cpp hwbp.cpp camobj.cpp camhook.cpp /Fobuild\ /Febuild\test_scan.exe user32.lib >nul || exit /b 9

build\test_scan.exe

exit /b %errorlevel%

