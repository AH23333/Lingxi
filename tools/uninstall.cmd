@echo off
rem Lingxi IME one-click uninstaller (elevation wrapper).
rem ASCII only; cmd.exe reads batch files in ANSI codepage.
rem Tray menu launches this via ShellExecute "runas" (already elevated);
rem manual double-click self-elevates via UAC below.
setlocal

net session >nul 2>&1
if errorlevel 1 (
    echo [uninstall] elevation required, relaunching via UAC...
    powershell -NoProfile -Command "Start-Process -FilePath 'cmd.exe' -ArgumentList '/c','\"%~f0\"' -Verb RunAs"
    exit /b 0
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1"
set RC=%ERRORLEVEL%
echo.
if "%RC%"=="0" (
    echo [uninstall] done. Press any key to close.
) else (
    echo [uninstall] finished with residue - see report above. Press any key to close.
)
pause >nul
exit /b %RC%
