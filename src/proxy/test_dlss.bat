@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /O2 /MT /EHsc /std:c++17 test_dlss.cpp camhook.cpp /Fobuild\ /Febuild\test_dlss.exe /link psapi.lib || exit /b 1
build\test_dlss.exe
exit /b %errorlevel%
