# Batch 6 e2e drill: local fake release server (127.0.0.1 only) + update_test --e2e.
# NEVER touches bin\Debug / bin\Debug32. Exit 0 only when E2E_RC=0 AND DRILL=ALLPASS.
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot)   # repo root (script lives in tools\update_test)
$exe = Join-Path $root 'bin\updatetest\update_test.exe'
if (-not (Test-Path $exe)) { Write-Host 'DRILL=EXE_MISSING'; exit 1 }
New-Item -ItemType Directory -Force (Join-Path $root 'tmp') | Out-Null
$srvOut = Join-Path $root 'tmp\drill_srv.log'
$srvErr = Join-Path $root 'tmp\drill_srv_err.log'
$port = 8765
$srv = Start-Process -FilePath 'python' -ArgumentList @("`"$root\tools\update_test\fake_release_server.py`"", '--port', "$port", '--exe', "`"$exe`"") -RedirectStandardOutput $srvOut -RedirectStandardError $srvErr -PassThru -WindowStyle Hidden
$deadline = (Get-Date).AddSeconds(20)
$ready = $false
while ((Get-Date) -lt $deadline) {
    if ((Test-Path $srvOut) -and ((Get-Content $srvOut -Raw -ErrorAction SilentlyContinue) -match 'READY')) { $ready = $true; break }
    if ($srv.HasExited) { break }
    Start-Sleep -Milliseconds 200
}
if (-not $ready) {
    Write-Host 'DRILL=SERVER_NOT_READY'
    if (Test-Path $srvErr) { Get-Content $srvErr }
    Stop-Process -Id $srv.Id -Force -ErrorAction SilentlyContinue
    exit 1
}
$env:LINGXI_UPDATE_TEST_BASE = "http://127.0.0.1:$port"
$out = Join-Path $root 'tmp\drill_e2e_out.txt'
$p = Start-Process -FilePath $exe -ArgumentList '--e2e' -RedirectStandardOutput $out -NoNewWindow -Wait -PassThru
Stop-Process -Id $srv.Id -Force -ErrorAction SilentlyContinue
Write-Host "E2E_RC=$($p.ExitCode)"
Get-Content $out -Encoding Unicode | Select-String -Pattern '\[FAIL\]|E2E'
if ($p.ExitCode -ne 0) { Write-Host 'DRILL=FAIL'; exit 1 }
Write-Host 'DRILL=ALLPASS'
exit 0
