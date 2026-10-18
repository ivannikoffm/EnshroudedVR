@echo off

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul

cd /d "%~dp0"

cl /nologo /O2 /MT /EHsc /std:c++17 test_camhook.cpp camhook.cpp /Fobuild\ /Febuild\test_camhook.exe || exit /b 1

build\test_camhook.exe

