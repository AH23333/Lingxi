@echo off
rem Unregister Lingxi IME (both bitnesses) and optionally purge user data.
rem Usage: unregister.cmd [Debug or Release] [--purge]
rem   --purge  ALSO delete user settings (HKCU\Software\LingxiIme) and the
rem            learned user dictionary (APPDATA\LingxiIme). Default: keep.
rem ASCII only (PITFALLS #12). Unregistering HKLM needs Administrator.
setlocal
set CONFIG=Debug
set PURGE=0
for %%A in (%*) do (
    if /i "%%A"=="--purge" set PURGE=1
    if /i not "%%A"=="--purge" set CONFIG=%%A
)
set "DLL=%~dp0..\bin\%CONFIG%\ImeCore.dll"
set "DLL32=%~dp0..\bin\%CONFIG%32\ImeCore.dll"
set "CLSID={AE967759-F054-4E02-AC5D-C172F1E9B99B}"

rem Stop the UI process first (project-owned process)
taskkill /IM LingxiUI.exe /f >nul 2>&1

if exist "%DLL%" regsvr32 /u /s "%DLL%"
if exist "%DLL32%" regsvr32 /u /s "%DLL32%"
if not exist "%DLL%" if not exist "%DLL32%" echo [unregister] No ImeCore.dll under bin\%CONFIG%, nothing to unregister

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
reg query "HKLM\SOFTWARE\WOW6432Node\Microsoft\CTF\TIP\%CLSID%" >nul 2>&1
if not errorlevel 1 (
    echo [unregister] WARNING: WOW6432Node CTF TIP still present. Run as Administrator and retry.
    set LEFTOVER=1
)

if "%PURGE%"=="1" (
    reg delete "HKCU\Software\LingxiIme" /f >nul 2>&1
    if exist "%APPDATA%\LingxiIme" rmdir /s /q "%APPDATA%\LingxiIme"
    del /q "%TEMP%\lingxi-ime.log" "%TEMP%\lingxi_sent.txt" >nul 2>&1
    echo [unregister] Purged: HKCU settings + user dictionary + temp logs.
)

if "%LEFTOVER%"=="1" exit /b 2

echo [unregister] Done. If the IME still appears in the input list, sign out and back in.
