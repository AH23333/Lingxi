# Lingxi IME one-click uninstaller (elevated). ASCII only.
# Companion of uninstall.cmd (elevation wrapper / tray entry).
# Cleans: COM CLSID (x64+x86), CTF TIP trees (HKLM+HKCU), SortOrder
# assembly items, language list tips, autostart Run entry, running
# processes. User data (HKCU\Software\LingxiIme settings + %APPDATA%
# \LingxiIme learned dictionary) is PRESERVED by default: uninstalling
# must not throw away the user's dictionary. Pass -PurgeUserData to
# remove it as well. Verifies zero residue and exits with residue
# count (user-data checks flip: default expects KEPT, purge expects
# GONE).
#requires -Version 5.1
param([string]$Root = (Split-Path -Parent $PSScriptRoot),
      [switch]$PurgeUserData)

$ErrorActionPreference = 'Continue'
$tip  = '{ae967759-f054-4e02-ac5d-c172f1e9b99b}'   # CLSID_LingxiTextService
$fail = 0

function Step($m) { Write-Host "[uninstall] $m" }

Step "stop LingxiUI"
Stop-Process -Name LingxiUI -Force -ErrorAction SilentlyContinue

Step "strip Lingxi tips from the language list"
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
}

Step "unregister TIP x64"
$null = regsvr32 /u /s "$Root\bin\Debug\ImeCore.dll" 2>$null
Step "unregister TIP x86 (WOW64)"
if (Test-Path "$Root\bin\Debug32\ImeCore.dll") {
    & "$env:windir\SysWOW64\regsvr32.exe" /u /s "$Root\bin\Debug32\ImeCore.dll"
}

Step "remove leftover registry keys (belt and braces)"
$null = reg delete "HKLM\SOFTWARE\Classes\CLSID\$tip" /f 2>$null
$null = reg delete "HKLM\SOFTWARE\Classes\WOW6432Node\CLSID\$tip" /f 2>$null
$null = reg delete "HKLM\SOFTWARE\Microsoft\CTF\TIP\$tip" /f 2>$null
$null = reg delete "HKCU\Software\Microsoft\CTF\TIP\$tip" /f 2>$null
if ($PurgeUserData) { $null = reg delete "HKCU\Software\LingxiIme" /f 2>$null }

Step "remove autostart Run entry"
Remove-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' `
    -Name LingxiIme -ErrorAction SilentlyContinue

Step "remove SortOrder assembly items (keep other IMEs' items)"
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

Step "restart ctfmon (flush CTF caches)"
Stop-Process -Name ctfmon -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 800
if (-not (Get-Process -Name ctfmon -ErrorAction SilentlyContinue)) {
    Start-Process "$env:windir\System32\ctfmon.exe"
}

if ($PurgeUserData) {
    Step "purge user data (-PurgeUserData): APPDATA\LingxiIme"
    Remove-Item "$env:APPDATA\LingxiIme" -Recurse -Force -ErrorAction SilentlyContinue
} else {
    Step "keep user data (APPDATA\LingxiIme + HKCU settings); -PurgeUserData to remove"
}

# ---- zero-residue verification ----
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
$checks = @(
    @{ n = 'HKLM CLSID (x64)';    ok = -not (Test-Path "HKLM:\SOFTWARE\Classes\CLSID\$tip") },
    @{ n = 'HKLM CLSID (x86)';    ok = -not (Test-Path "HKLM:\SOFTWARE\Classes\WOW6432Node\CLSID\$tip") },
    @{ n = 'HKLM CTF TIP tree';   ok = -not (Test-Path "HKLM:\SOFTWARE\Microsoft\CTF\TIP\$tip") },
    @{ n = 'HKCU CTF TIP tree';   ok = -not (Test-Path "HKCU:\Software\Microsoft\CTF\TIP\$tip") },
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
if ($fail -eq 0) {
    Write-Host "[uninstall] SUCCESS: zero residue. The source tree can be deleted now."
} else {
    Write-Host "[uninstall] $fail residue item(s) remain, see report above."
}
exit $fail
