@echo off
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
msbuild KVMSwitch.sln /p:Configuration=Release /p:Platform=x64 /m /v:m /nologo
