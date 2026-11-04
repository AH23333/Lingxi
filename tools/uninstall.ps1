# Lingxi IME one-click uninstaller (elevated). ASCII only.
# Companion of uninstall.cmd (elevation wrapper / tray entry).
# Cleans: COM CLSID (x64+x86, HKLM **and** HKCU per-user fallback view),
# CTF TIP trees (HKLM+HKCU), SortOrder assembly items, language list tips,
# autostart Run entry, running processes. User data (HKCU\Software\LingxiIme
# settings + %APPDATA%\LingxiIme learned dictionary) is PRESERVED by default:
# uninstalling must not throw away the user's dictionary. Pass -PurgeUserData
# to remove it as well. Verifies zero residue and exits with residue+failure
# count (user-data checks flip: default expects KEPT, purge expects GONE).
#
# -DryRun: print every planned action and run the verification READ-ONLY;
#          touches nothing (no process stop, no regsvr32, no deletes).
#          Exit code = residue count (nonzero while anything remains).
#
# P1-1 fix (review-2026-11-04-system-integration): cleanup list and residue
# checklist share ONE source ($RegKeyTargets) so the two can never drift
# apart again; HKCU per-user CLSID keys (the non-admin registration
# fallback written by ime/dllmain.cpp RegisterServer) are now covered;
# regsvr32 exit codes are no longer swallowed; ghost CLSID (key present
# but DLL file gone) is detected and reported with a restore action.
#requires -Version 5.1
param([string]$Root = (Split-Path -Parent $PSScriptRoot),
      [switch]$PurgeUserData,
      [switch]$DryRun)

$ErrorActionPreference = 'Continue'
$tip  = '{ae967759-f054-4e02-ac5d-c172f1e9b99b}'   # CLSID_LingxiTextService
$fail = 0

function Step($m) { Write-Host "[uninstall] $m" }
function Plan($m) { if ($DryRun) { Write-Host "  [DryRun] would: $m" } }

# ---- shared registry key list: cleanup AND verification both iterate this ----
# Exact keys only. Never recurse a parent key (we must not touch other CLSIDs).
$RegKeyTargets = @(
    @{ hive='HKLM'; path="SOFTWARE\Classes\CLSID\$tip";              label='HKLM CLSID (x64)' },
    @{ hive='HKLM'; path="SOFTWARE\Classes\WOW6432Node\CLSID\$tip";  label='HKLM CLSID (x86)' },
    @{ hive='HKCU'; path="SOFTWARE\Classes\CLSID\$tip";              label='HKCU CLSID (per-user x64)' },
    @{ hive='HKCU'; path="SOFTWARE\Classes\WOW6432Node\CLSID\$tip";  label='HKCU CLSID (per-user x86)' },
    @{ hive='HKLM'; path="SOFTWARE\Microsoft\CTF\TIP\$tip";          label='HKLM CTF TIP tree' },
    @{ hive='HKCU'; path="Software\Microsoft\CTF\TIP\$tip";          label='HKCU CTF TIP tree' }
)

# Read the InprocServer32 default value of a present CLSID key (ghost check).
function Get-ClsidServerPath($hive, $path) {
    try {
        $v = (Get-ItemProperty -Path "Registry::$hive\$path\InprocServer32" `
              -ErrorAction Stop).'(default)'
        if ($null -eq $v) {
            $v = (Get-Item "Registry::$hive\$path\InprocServer32" -ErrorAction Stop).GetValue('')
        }
        if ($v) { return ($v -replace '^"|"$', '') }
    } catch { }
    return $null
}

if ($DryRun) { Step "DRY RUN - no changes will be made" }

Step "stop LingxiUI"
Plan "Stop-Process -Name LingxiUI -Force"
if (-not $DryRun) { Stop-Process -Name LingxiUI -Force -ErrorAction SilentlyContinue }

Step "strip Lingxi tips from the language list"
Plan "remove $tip from Get-WinUserLanguageList (fall back to default keyboard)"
if (-not $DryRun) {
try {
    $list = Get-WinUserLanguageList
    foreach ($el in $list) {
        $keep = @($el.InputMethodTips | Where-Object { $_ -notlike "*$tip*" })
        if ($keep.Count -eq 0) {
            # never leave a language without an input method: fall back
            # to its default keyboard layout
            $hex = '{0:x4}' -f ([System.Globalization.CultureInfo]::new($el.LanguageTag).LCID)
            $keep = @("$hex" + ":0000" + "$hex")
        }
        $el.InputMethodTips.Clear()
        foreach ($k in $keep) { [void]$el.InputMethodTips.Add($k) }
    }
    Set-WinUserLanguageList $list -Force
} catch {
    Write-Host "[uninstall] WARN language list: $($_.Exception.Message)"
    $fail++
}
}

Step "unregister TIP x64"
Plan "regsvr32 /u /s $Root\bin\Debug\ImeCore.dll"
if (-not $DryRun) {
    if (Test-Path "$Root\bin\Debug\ImeCore.dll") {
        regsvr32 /u /s "$Root\bin\Debug\ImeCore.dll" 2>$null
        if ($LASTEXITCODE -ne 0) {
            Write-Host "[uninstall] WARN regsvr32 /u x64 exit=$LASTEXITCODE (belt-and-braces cleanup below still runs)"
            $fail++
        }
    } else {
        Write-Host "[uninstall] NOTE x64 DLL already missing; registry cleanup below handles ghost keys"
    }
    Step "unregister TIP x86 (WOW64)"
    Plan "SysWOW64 regsvr32 /u /s $Root\bin\Debug32\ImeCore.dll"
    if (Test-Path "$Root\bin\Debug32\ImeCore.dll") {
        & "$env:windir\SysWOW64\regsvr32.exe" /u /s "$Root\bin\Debug32\ImeCore.dll"
        if ($LASTEXITCODE -ne 0) {
            Write-Host "[uninstall] WARN regsvr32 /u x86 exit=$LASTEXITCODE (belt-and-braces cleanup below still runs)"
            $fail++
        }
    } else {
        Write-Host "[uninstall] NOTE x86 DLL already missing; registry cleanup below handles ghost keys"
    }
} else {
    Step "unregister TIP x86 (WOW64)"
    Plan "SysWOW64 regsvr32 /u /s $Root\bin\Debug32\ImeCore.dll"
}

Step "remove leftover registry keys (shared list, exact keys only)"
foreach ($k in $RegKeyTargets) {
    Plan "reg delete $($k.hive)\$($k.path) /f   [$($k.label)]"
    if (-not $DryRun) {
        reg delete "$($k.hive)\$($k.path)" /f 2>$null | Out-Null
    }
}
if ($PurgeUserData) {
    Plan "reg delete HKCU\Software\LingxiIme /f   [user settings, purge only]"
    if (-not $DryRun) { reg delete "HKCU\Software\LingxiIme" /f 2>$null | Out-Null }
}

Step "remove autostart Run entry"
Plan "Remove-ItemProperty HKCU:...\Run LingxiIme"
if (-not $DryRun) {
    Remove-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' `
        -Name LingxiIme -ErrorAction SilentlyContinue
}

Step "remove SortOrder assembly items (keep other IMEs' items)"
Plan "delete AssemblyItem entries whose CLSID == $tip only"
if (-not $DryRun) {
Get-ChildItem 'HKCU:\Software\Microsoft\CTF\SortOrder\AssemblyItem' -ErrorAction SilentlyContinue |
    ForEach-Object { $lang = $_
        Get-ChildItem $lang.PSPath -ErrorAction SilentlyContinue |
            ForEach-Object { $cat = $_
                Get-ChildItem $cat.PSPath -ErrorAction SilentlyContinue |
                    ForEach-Object { $item = $_
                        $clsid = (Get-ItemProperty $item.PSPath -Name CLSID -ErrorAction SilentlyContinue).CLSID
                        if ($clsid -and $clsid -ieq $tip) {
                            Remove-Item $item.PSPath -Recurse -Force
                        }
                    }
            }
    }
}

Step "restart ctfmon (flush CTF caches)"
Plan "Stop-Process ctfmon; start ctfmon if not running"
if (-not $DryRun) {
    Stop-Process -Name ctfmon -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 800
    if (-not (Get-Process -Name ctfmon -ErrorAction SilentlyContinue)) {
        Start-Process "$env:windir\System32\ctfmon.exe"
    }
}

if ($PurgeUserData) {
    Step "purge user data (-PurgeUserData): APPDATA\LingxiIme"
    Plan "Remove-Item $env:APPDATA\LingxiIme -Recurse -Force"
    if (-not $DryRun) {
        Remove-Item "$env:APPDATA\LingxiIme" -Recurse -Force -ErrorAction SilentlyContinue
    }
} else {
    Step "keep user data (APPDATA\LingxiIme + HKCU settings); -PurgeUserData to remove"
}

# ---- zero-residue verification (read-only; runs in DryRun too) ----
Step "verify zero residue"
function Test-SortOrderResidue {
    $found = $null -ne (Get-ChildItem 'HKCU:\Software\Microsoft\CTF\SortOrder\AssemblyItem' -ErrorAction SilentlyContinue |
        ForEach-Object { $l = $_
            Get-ChildItem $l.PSPath -ErrorAction SilentlyContinue |
                ForEach-Object { $c = $_
                    Get-ChildItem $c.PSPath -ErrorAction SilentlyContinue |
                        ForEach-Object { $i = $_
                            $cl = (Get-ItemProperty $i.PSPath -Name CLSID -ErrorAction SilentlyContinue).CLSID
                            if ($cl -and $cl -ieq $tip) { $i }
                        }
                }
        })
    return $found
}
function Test-LangListResidue {
    $tips = @(Get-WinUserLanguageList | ForEach-Object { $_.InputMethodTips } |
        Where-Object { $_ -like "*$tip*" })
    return $tips.Count -gt 0
}
$udKept = -not $PurgeUserData   # default: user data must SURVIVE the uninstall
if ($udKept) { Write-Host "  (user-data checks expect KEPT; -PurgeUserData flips them to expect GONE)" }

$checks = @()
# Registry keys: same shared list as cleanup (P1-1: single source, no drift).
foreach ($k in $RegKeyTargets) {
    $checks += @{ n = $k.label; ok = -not (Test-Path "Registry::$($k.hive)\$($k.path)") }
}
$checks += @(
    @{ n = 'HKCU settings key (user data)'; ok = ($udKept -eq [bool](Test-Path "HKCU:\Software\LingxiIme")) },
    @{ n = 'SortOrder items';     ok = -not (Test-SortOrderResidue) },
    @{ n = 'language list tips';  ok = -not (Test-LangListResidue) },
    @{ n = 'APPDATA user data (user data)'; ok = ($udKept -eq [bool](Test-Path "$env:APPDATA\LingxiIme")) },
    @{ n = 'autostart Run entry'; ok = $null -eq (Get-ItemProperty `
        'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' `
        -Name LingxiIme -ErrorAction SilentlyContinue) }
)
foreach ($c in $checks) {
    if ($c.ok) { Write-Host "  [OK]      $($c.n)" }
    else { Write-Host "  [RESIDUE] $($c.n)" -ForegroundColor Red; $fail++ }
}

# Ghost CLSID detail: key present but the DLL it points to is gone.
# Report the restore action instead of a bare red line.
foreach ($k in $RegKeyTargets[0..3]) {
    if (Test-Path "Registry::$($k.hive)\$($k.path)") {
        $srv = Get-ClsidServerPath $k.hive $k.path
        if ($srv -and -not (Test-Path $srv)) {
            Write-Host "  [GHOST]   $($k.label): key present but DLL missing -> $srv" -ForegroundColor Yellow
            Write-Host "            restore: re-run this uninstaller (key deletion above covers it),"
            Write-Host "            or delete manually: reg delete $($k.hive)\$($k.path) /f"
        }
    }
}

if ($DryRun) {
    Write-Host "[uninstall] DRY RUN done. $fail item(s) currently present/unclean (nothing was changed)."
    exit $fail
}
if ($fail -eq 0) {
    Write-Host "[uninstall] SUCCESS: zero residue. Sign out and back in, then the source tree can be deleted."
    Write-Host "            (Hosts that already loaded the IME keep the old image mapped until"
    Write-Host "            sign-out; locked DLL files can be deleted after that.)"
} else {
    Write-Host "[uninstall] $fail residue/failure item(s) remain, see report above."
}
exit $fail
