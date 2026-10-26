@echo off
rem Build ImeCore.vcxproj only (ASCII only)
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
msbuild "%~dp0..\ime\ImeCore.vcxproj" /p:Configuration=Debug /p:Platform=x64 /nologo /v:m
