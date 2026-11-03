# repro_coldstart_race.ps1 -- section 8-8 cold-start launch race: minimal repro + root-cause classifier
#
# Suspect chain (deep_test timeline): taskkill LingxiUI -> milliseconds later the IME DLL's
# EnsureUIRunning spawns a new LingxiUI; the dying instance may still hold
# "Local\LingxiImeSingleton" -> new instance sees ERROR_ALREADY_EXISTS at ui/main.cpp:500-508
# and exits SILENTLY (no log, no ready event). The DLL side latches "launched" per connect
# cycle (ime/pipeclient.cpp:95-107) and never respawns -> WaitUiReady 20s timeout.
#
# Competing hypothesis to discriminate: engine cold load exceeds the 20s ready budget
# (process ALIVE but Local\LingxiUIReady never signaled in time).
#
# Modes:
#   -Mode1  deterministic mechanism proof: hold the singleton mutex externally, spawn UI,
#           observe silent suicide + absent ready event; release; observe nobody respawns.
#   -Mode2  real-race sampler: loop K trials of [kill UI; spawn immediately; classify].
#           SUICIDE  = no LingxiUI process 2s after spawn (mutex window hit)
#           SLOWLOAD = process alive but ready event absent at +20s (cold load over budget)
#           OK       = ready within budget (records elapsed ms for distribution)
#
# ASCII only (PS5 encoding safety). Sandboxes APPDATA like regress does.
# Usage:  powershell -File tools\repro_coldstart_race.ps1 -Mode1
#         powershell -File tools\repro_coldstart_race.ps1 -Mode2 -Trials 10
param(
    [switch]$Mode1,
    [switch]$Mode2,
    [int]$Trials = 10
)

$ErrorActionPreference = "Continue"
$ROOT = Split-Path -Parent $PSScriptRoot
$UI   = Join-Path $ROOT "bin\Debug\LingxiUI.exe"
if (-not (Test-Path $UI)) { Write-Output "FATAL: $UI not found"; exit 1 }

# sandbox user dict like regress (never touch the real one)
$SB = Join-Path $ROOT "tmp\repro-cs-userdata"
if (-not (Test-Path $SB)) { New-Item -ItemType Directory -Path $SB | Out-Null }
$env:APPDATA = $SB
$env:LINGXI_SETTINGS_NS = "test"
$env:LINGXI_LOG = ""

$MtxName  = "Local\LingxiImeSingleton"
$ReadyName = "Local\LingxiUIReady"

function Test-UiAlive {
    $null -ne (Get-Process -Name LingxiUI -ErrorAction SilentlyContinue)
}
function Get-UiPid {
    $p = Get-Process -Name LingxiUI -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($p) { return $p.Id } else { return 0 }
}
function Test-ReadySignaled {
    # open existing event; returns $true only if signaled, $false if absent/unsignaled
    try {
        $h = [System.Threading.EventWaitHandle]::OpenExisting($ReadyName)
        $sig = $h.WaitOne(0)
        $h.Close()
        return $sig
    } catch { return $false }
}
function Kill-Ui {
    Get-Process -Name LingxiUI -ErrorAction SilentlyContinue | ForEach-Object {
        Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
    }
}

# ---------------------------------------------------------------- Mode 1
if ($Mode1) {
    Write-Output "=== MODE1: deterministic mutex-holder suicide proof ==="
    $createdNew = $false
    # initiallyOwned=$true: we own it when freshly created, so ReleaseMutex is legal
    $m = New-Object System.Threading.Mutex($true, $MtxName, [ref]$createdNew)
    if (-not $createdNew) {
        Write-Output "ABORT: mutex already held by a live UI -- kill it first (taskkill /f /im LingxiUI.exe)"
        $m.Close(); exit 2
    }
    Write-Output "[holder] mutex acquired externally (emulates dying instance mid-teardown)"
    Kill-Ui; Start-Sleep -Milliseconds 300

    Start-Process -FilePath $UI | Out-Null
    Start-Sleep -Seconds 2
    $alive2s = Test-UiAlive
    $ready2s = Test-ReadySignaled
    Write-Output ("[spawn-while-held] alive@2s={0} ready@2s={1}  (expect False/False = silent suicide)" -f $alive2s, $ready2s)

    [void]$m.ReleaseMutex(); $m.Close()
    Write-Output "[holder] mutex released (dying instance now fully torn down)"
    Start-Sleep -Seconds 4
    $aliveAfter = Test-UiAlive
    $readyAfter = Test-ReadySignaled
    Write-Output ("[after-release] alive={0} ready={1}  (expect False/False = nobody respawns)" -f $aliveAfter, $readyAfter)

    if ((-not $alive2s) -and (-not $ready2s) -and (-not $aliveAfter) -and (-not $readyAfter)) {
        Write-Output "MODE1 RESULT: MECHANISM CONFIRMED -- spawn during mutex-holder lifetime dies silently and is never retried"
        exit 0
    } else {
        Write-Output "MODE1 RESULT: UNEXPECTED -- re-inspect (did the spawn survive while held?)"
        Kill-Ui
        exit 1
    }
}

# ---------------------------------------------------------------- Mode 2
if ($Mode2) {
    Write-Output ("=== MODE2: real kill->spawn race sampler, {0} trials ===" -f $Trials)
    $suicide = 0; $slow = 0; $ok = 0; $readyMsList = @()
    for ($k = 1; $k -le $Trials; $k++) {
        Kill-Ui
        # wait for the process object to be gone (this is what taskkill's synchronous
        # return guarantees); spawn IMMEDIATELY after, mimicking the DLL's timing
        $t0 = Get-Date
        Start-Process -FilePath $UI | Out-Null
        Start-Sleep -Seconds 2
        if (-not (Test-UiAlive)) {
            $suicide++
            Write-Output ("trial {0}: SUICIDE (no process 2s after spawn -- mutex window hit)" -f $k)
            continue
        }
        # process alive: poll ready event up to 20s total, like WaitUiReady(100)
        $ready = $false
        $deadline = $t0.AddSeconds(20)
        while ((Get-Date) -lt $deadline) {
            if (Test-ReadySignaled) { $ready = $true; break }
            Start-Sleep -Milliseconds 200
        }
        if ($ready) {
            $ok++
            $ms = [int]((Get-Date) - $t0).TotalMilliseconds
            $readyMsList += $ms
            Write-Output ("trial {0}: OK ready@{1}ms" -f $k, $ms)
        } else {
            $slow++
            Write-Output ("trial {0}: SLOWLOAD (alive but no ready event within 20s)" -f $k)
        }
    }
    Kill-Ui
    Write-Output ("=== MODE2 SUMMARY: trials={0} OK={1} SUICIDE={2} SLOWLOAD={3}" -f $Trials, $ok, $suicide, $slow)
    if ($readyMsList.Count -gt 0) {
        $sorted = $readyMsList | Sort-Object
        Write-Output ("ready ms: min={0} median={1} max={2}" -f $sorted[0], $sorted[[int]($sorted.Count/2)], $sorted[-1])
    }
    if ($suicide -gt 0) { Write-Output "VERDICT: mutex-window suicide reproduced for real"; exit 0 }
    if ($slow -gt 0)    { Write-Output "VERDICT: cold-load-over-budget reproduced for real"; exit 0 }
    Write-Output "VERDICT: race not hit in this sample (see counts above)"
    exit 3
}

Write-Output "usage: -Mode1 | -Mode2 [-Trials N]"
exit 1
