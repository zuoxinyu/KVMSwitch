@echo off
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cl /nologo /EHsc /O2 /utf-8 /Fe:ddc_timing_test.exe ddc_timing_test.cpp user32.lib dxva2.lib
