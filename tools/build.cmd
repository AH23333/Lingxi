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
rem writes fresh files. Target name carries a random stamp: a fixed .prev can
rem itself be locked (mapped by a live process) and undeletable, which would
rem make the rename fail and LNK1168 return. Stamped files are cleaned on some
rem later run when unlocked.
set "OUTDIR=%ROOT%\bin\%CONFIG%"
set "STAMP=%RANDOM%%RANDOM%"
if exist "%OUTDIR%" (
    del /q "%OUTDIR%\*.prev" 2>nul
    for %%F in (ImeCore.dll LingxiUI.exe) do (
        if exist "%OUTDIR%\%%F" ren "%OUTDIR%\%%F" "%%F.%STAMP%.prev" 2>nul
    )
)

msbuild "%ROOT%\LingxiIme.sln" /m /nologo /v:m /p:Configuration=%CONFIG% /p:Platform=x64
set MSBUILD_RC=%errorlevel%

rem Stage llama.cpp runtime + model next to the exe (copy only when newer/missing)
if exist "%ROOT%\deps\llama\bin" (
    robocopy "%ROOT%\deps\llama\bin" "%OUTDIR%\llama" /XO /NJH /NJS /NDL >nul
)
if exist "%ROOT%\deps\models" (
    robocopy "%ROOT%\deps\models" "%OUTDIR%\models" /XO /NJH /NJS /NDL >nul
)

endlocal & exit /b %MSBUILD_RC%
