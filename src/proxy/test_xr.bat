@echo off

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul

cd /d "%~dp0"

cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /I..\third_party /I..\third_party\openxr\include test_xr.cpp xr.cpp osd.cpp hwbp.cpp /Fobuild\ /Febuild\test_xr.exe || exit /b 1

copy /y ..\third_party\openxr\x64\openxr_loader.dll build\ >nul

build\test_xr.exe %1

