@echo off
cd /d "%~dp0"
if not exist build mkdir build
C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe /nologo /optimize+ /target:winexe /out:build\EnshroudedVR-Config.exe /r:System.Windows.Forms.dll /r:System.Drawing.dll EnshroudedVRConfig.cs
