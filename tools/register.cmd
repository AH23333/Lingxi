@echo off
rem Register Lingxi IME (ImeCore.dll). Machine-level registration needs admin;
rem the DLL falls back to per-user (HKCU) registration automatically.
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Debug
set "DLL=%~dp0..\bin\%CONFIG%\ImeCore.dll"

if not exist "%DLL%" (
    echo [register] %DLL% not found. Run build.cmd first.
    exit /b 1
)

regsvr32 /s "%DLL%"
if errorlevel 1 (
    echo [register] Registration FAILED.
    echo   Try: run this script as Administrator, or set LINGXI_LOG=1 and
    echo   check %%TEMP%%\lingxi-ime.log
    exit /b 1
)
echo [register] OK. If the IME does not appear in the input list, sign out and back in.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0imecheck.ps1"
