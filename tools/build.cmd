@echo off
rem Lingxi IME build script (ASCII only; cmd.exe reads batch files in ANSI codepage)
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Debug

set "ROOT=%~dp0.."
set "VSVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

if not exist "%VSVARS%" (
    echo [build] vcvars64.bat not found, check VS2022 install path
    exit /b 1
)

call "%VSVARS%" >nul
if errorlevel 1 (
    echo [build] vcvars64 init failed
    exit /b 1
)

rem Pre-empt file locks: a TIP DLL loaded by live apps (CTF) or a running UI
rem cannot be overwritten. Rename previous outputs aside so the linker always
rem writes fresh files; locked .prev files are cleaned on some later run.
set "OUTDIR=%ROOT%\bin\%CONFIG%"
if exist "%OUTDIR%" (
    del /q "%OUTDIR%\*.prev" 2>nul
    for %%F in (ImeCore.dll LingxiUI.exe) do (
        if exist "%OUTDIR%\%%F" ren "%OUTDIR%\%%F" "%%F.prev" 2>nul
    )
)

msbuild "%ROOT%\LingxiIme.sln" /m /nologo /v:m /p:Configuration=%CONFIG% /p:Platform=x64
endlocal
exit /b %errorlevel%
