@echo off

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul

cd /d "%~dp0"

cl /nologo /O1 /MT /EHsc /std:c++17 test_hwbp.cpp hwbp.cpp /Fobuild\ /Febuild\test_hwbp.exe /link psapi.lib >nul || exit /b 1

build\test_hwbp.exe

