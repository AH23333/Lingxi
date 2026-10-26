@echo off
rem Unregister Lingxi IME
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Debug
set "DLL=%~dp0..\bin\%CONFIG%\ImeCore.dll"

if not exist "%DLL%" (
    echo [unregister] %DLL% not found
    exit /b 1
)
regsvr32 /u /s "%DLL%"
if errorlevel 1 (
    echo [unregister] Failed. Try running as Administrator.
    exit /b 1
)
echo [unregister] Done.
