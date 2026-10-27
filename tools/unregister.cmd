@echo off
rem Unregister Lingxi IME
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Debug
set "DLL=%~dp0..\bin\%CONFIG%\ImeCore.dll"
set "CLSID={AE967759-F054-4E02-AC5D-C172F1E9B99B}"

if not exist "%DLL%" (
    echo [unregister] %DLL% not found
    exit /b 1
)
regsvr32 /u /s "%DLL%"
if errorlevel 1 (
    echo [unregister] Failed. Try running as Administrator.
    exit /b 1
)

rem Self-check: CTF unregister only writes HKLM and fails silently without
rem admin; leftover TIP keys with no CLSID create a "ghost" input method.
set LEFTOVER=0
reg query "HKLM\SOFTWARE\Classes\CLSID\%CLSID%" >nul 2>&1
if not errorlevel 1 (
    echo [unregister] WARNING: HKLM CLSID still present. Run as Administrator and retry.
    set LEFTOVER=1
)
reg query "HKLM\SOFTWARE\Microsoft\CTF\TIP\%CLSID%" >nul 2>&1
if not errorlevel 1 (
    echo [unregister] WARNING: HKLM CTF TIP still present. Run as Administrator and retry.
    set LEFTOVER=1
)
if "%LEFTOVER%"=="1" exit /b 2

echo [unregister] Done. If the IME still appears in the input list, sign out and back in.
