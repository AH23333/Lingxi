@echo off
rem One-time: enable Windows Error Reporting local dumps for LingxiUI.exe.
rem MUST run as Administrator (writes HKLM). After any LingxiUI crash, a full
rem dump appears in %TEMP%\lingxi-dumps for offline analysis.
rem ASCII only.
setlocal
net session >nul 2>&1
if errorlevel 1 (
    echo [setup_dumps] ERROR: run as Administrator ^(right-click - Run as administrator^).
    exit /b 1
)
set "KEY=HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\LingxiUI.exe"
reg add "%KEY%" /v DumpFolder /t REG_EXPAND_SZ /d "%%TEMP%%\lingxi-dumps" /f >nul
reg add "%KEY%" /v DumpType /t REG_DWORD /d 2 /f >nul
reg add "%KEY%" /v DumpCount /t REG_DWORD /d 5 /f >nul
if errorlevel 1 (
    echo [setup_dumps] reg add FAILED.
    exit /b 1
)
echo [setup_dumps] OK - WER LocalDumps configured for LingxiUI.exe
echo [setup_dumps]   DumpFolder=%%TEMP%%\lingxi-dumps  DumpType=2 (full)  DumpCount=5
reg query "%KEY%"
