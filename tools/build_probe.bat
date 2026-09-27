@echo off
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cl /nologo /EHsc /O2 /utf-8 /Fe:tray_probe.exe tray_probe.cpp shell32.lib user32.lib
