@echo off
rem Deep regression: engine_test once + e2e up to 3 attempts (stop at first pass).
rem Logs archived to tmp\regress-<random>\. Exit 0 = all pass, 1 = failure.
rem ASCII only. Tip: close heavy apps first; e2e is timing-sensitive (PITFALLS #19).
setlocal
set "ROOT=%~dp0.."
set "OUT=%ROOT%\tmp\regress-%RANDOM%%RANDOM%"
mkdir "%OUT%" 2>nul

rem Environment noise guards (LINGXI_LOG=1 destroys latency + spams file IO)
set LINGXI_LOG=
set LINGXI_PINYIN=

set FAIL=0

echo [regress] engine_test...
"%ROOT%\bin\enginetest\engine_test.exe" > "%OUT%\engine_test.txt" 2>&1
if errorlevel 1 (
    echo [regress] engine_test FAILED - see engine_test.txt
    set FAIL=1
) else (
    echo [regress] engine_test PASS
)

rem goto-loop: %VAR% re-parses each pass, so "stop at first pass" works
set E2E_OK=0
set ATTEMPT=1
:e2e_loop
if "%E2E_OK%"=="1" goto :e2e_done
if "%ATTEMPT%"=="4" goto :e2e_done
echo [regress] e2e attempt %ATTEMPT%...
"%ROOT%\bin\Debug\e2e.exe" > "%OUT%\e2e-%ATTEMPT%.txt" 2>&1
if errorlevel 1 (
    echo [regress] e2e attempt %ATTEMPT% FAILED
) else (
    echo [regress] e2e attempt %ATTEMPT% PASS
    set E2E_OK=1
    goto :e2e_done
)
set /a ATTEMPT+=1
goto :e2e_loop
:e2e_done

if not "%E2E_OK%"=="1" (
    echo [regress] e2e FAILED after 3 attempts
    set FAIL=1
)

if "%FAIL%"=="1" (
    echo [regress] RESULT: FAILED - logs in %OUT%
    exit /b 1
)
echo [regress] RESULT: ALL PASS - logs in %OUT%
endlocal
