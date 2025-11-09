@echo off
cd /d "%~dp0"
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" "%~dp0NextGenGraphics.Carbon.vcxproj" /p:Configuration=Release_NFSC /p:Platform=Win32 /t:Rebuild /v:minimal

