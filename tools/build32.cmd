@echo off
rem Build the 32-bit (Win32) ImeCore.dll for WOW64 hosts -> bin\<CONFIG>32\.
rem Only ImeCore needs a 32-bit build (LingxiUI/engine/e2e stay x64);
rem common is pulled in as a ProjectReference with Platform=Win32.
rem Register it with:  C:\Windows\SysWOW64\regsvr32.exe bin\<CONFIG>32\ImeCore.dll
rem ASCII only.
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Debug

set "ROOT=%~dp0.."
set "VSVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

if not exist "%VSVARS%" (
    echo [build32] vcvars64.bat not found, check VS2022 install path
    exit /b 1
)
call "%VSVARS%" >nul
if errorlevel 1 (
    echo [build32] vcvars64 init failed
    exit /b 1
)

rem Win32 outputs carry a "32" suffix (bin\Debug32\), set in the vcxproj via
rem MSBuildThisFileDirectory - no SolutionDir dependency, direct vcxproj build is fine.
msbuild "%ROOT%\ime\ImeCore.vcxproj" /m /nologo /v:m /p:Configuration=%CONFIG% /p:Platform=Win32
exit /b %errorlevel%
