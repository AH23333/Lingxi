@echo off
rem Deep regression: engine_test once + deep_test and e2e up to 3 attempts each
rem (stop at first pass). Logs archived to tmp\regress-<random>\. Exit 0 = all pass.
rem ASCII only. Tip: close heavy apps first; e2e/deep_test are timing-sensitive (PITFALLS #19).
rem
rem Isolation rules (learned 2026-10-28):
rem  - Stale LingxiUI instance from a previous run crosses state into the next
rem    (candidate/rerank leftovers) -> kill our own UI before each suite run.
rem  - suites Learn() into %APPDATA%\LingxiIme\userdict.txt -> sandbox APPDATA
rem    into tmp\regress-userdata so tests never touch the user's real dictionary.
setlocal
set "ROOT=%~dp0.."
set "OUT=%ROOT%\tmp\regress-%RANDOM%%RANDOM%"
mkdir "%OUT%" 2>nul

rem Environment noise guards (LINGXI_LOG=1 destroys latency + spams file IO)
set LINGXI_LOG=
set LINGXI_SETTINGS_NS=test
set LINGXI_PINYIN=

reg delete "HKCU\Software\LingxiIme_test" /f >nul 2>nul
rem sandboxed user dict for the whole regress run
set "APPDATA=%OUT%\userdata"
mkdir "%APPDATA%" 2>nul

set FAIL=0

echo [regress] engine_test...
"%ROOT%\bin\enginetest\engine_test.exe" > "%OUT%\engine_test.txt" 2>&1
if errorlevel 1 (
    echo [regress] engine_test FAILED - see engine_test.txt
    set FAIL=1
) else (
    echo [regress] engine_test PASS
)

rem ---- polyphonic-reading completeness gate (data class-level regression) ----
rem fix_readings --check exits 1 when single-char entries are missing readings
rem that are well-evidenced elsewhere in the dictionary itself. Best-effort:
rem only runs when python is available (build pipeline dependency).
where python >nul 2>nul
if not errorlevel 1 (
    echo [regress] fix_readings --check...
    python "%ROOT%\tools\fix_readings.py" --check > "%OUT%\fix_readings.txt" 2>&1
    if errorlevel 1 (
        echo [regress] fix_readings CHECK FAILED - see fix_readings.txt
        set FAIL=1
    ) else (
        echo [regress] fix_readings check PASS
    )
) else (
    echo [regress] python not found - skipping readings gate
)

rem ---- deep_test (pinyin composition pipeline; has internal 3-attempt shell) ----
rem fresh UI owned by this run: kill leftovers so state does not cross suites
taskkill /f /im LingxiUI.exe >nul 2>&1
rem note: taskkill /f on our own LingxiUI.exe is allowed (HANDOFF rule: only
rem project-owned processes may be restarted; never touch user apps)
echo [regress] deep_test...
"%ROOT%\tools\deep_test\bin\Debug\deep_test.exe" > "%OUT%\deep_test.txt" 2>&1
if errorlevel 1 (
    echo [regress] deep_test FAILED - see deep_test.txt
    set FAIL=1
) else (
    echo [regress] deep_test PASS
)

rem ---- e2e ----
taskkill /f /im LingxiUI.exe >nul 2>&1
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
