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

rem 32-bit TIP for WOW64 hosts (optional, coexists with x64 registration).
rem The 32-bit DLL lives in bin\<CONFIG>32\ (tools\build32.cmd). Register with
rem SysWOW64 regsvr32 so CLSID/CTF land in the WOW6432Node registry view.
set "DLL32=%~dp0..\bin\%CONFIG%32\ImeCore.dll"
if exist "%DLL32%" (
    %SystemRoot%\SysWOW64\regsvr32.exe /s "%DLL32%"
    if errorlevel 1 (
        echo [register] WARNING: 32-bit registration FAILED ^(needs Administrator^).
    ) else (
        echo [register] 32-bit TIP registered ^(WOW64 hosts now supported^).
    )
) else (
    echo [register] 32-bit ImeCore.dll not found - skipping WOW64 registration.
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0imecheck.ps1"
