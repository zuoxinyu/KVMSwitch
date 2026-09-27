@echo off
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
msbuild windows\KVMSwitch.sln /p:Configuration=Release /p:Platform=x64 /m /v:m /nologo
if %ERRORLEVEL% equ 0 (
    copy /y "windows\x64\Release\KVMSwitch.exe" "KVMSwitch.exe" >nul
    echo Build successful: KVMSwitch.exe
)
