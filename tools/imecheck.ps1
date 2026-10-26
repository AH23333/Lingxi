# Lingxi IME registration self-check (ASCII only: PowerShell 5.1 reads .ps1 in ANSI codepage)
$clsid = "{AE967759-F054-4E02-AC5D-C172F1E9B99B}"
$ok = $true

Write-Host "=== Lingxi IME registration self-check ==="

# 1. COM CLSID
$hklmClsid = Test-Path "HKLM:\SOFTWARE\Classes\CLSID\$clsid"
$hkcuClsid = Test-Path "HKCU:\SOFTWARE\Classes\CLSID\$clsid"
Write-Host ("CLSID registered: HKLM={0} HKCU={1}" -f $hklmClsid, $hkcuClsid)
if (-not ($hklmClsid -or $hkcuClsid)) { $ok = $false }

# 2. CTF TIP (written by ITfInputProcessorProfiles::Register)
$hklmTip = Test-Path "HKLM:\SOFTWARE\Microsoft\CTF\TIP\$clsid"
$hkcuTip = Test-Path "HKCU:\SOFTWARE\Microsoft\CTF\TIP\$clsid"
Write-Host ("CTF TIP registered: HKLM={0} HKCU={1}" -f $hklmTip, $hkcuTip)
if (-not ($hklmTip -or $hkcuTip)) {
    Write-Host "  [!] TIP missing -- profile/category registration failed. Check log."
    $ok = $false
}

# 3. Language profiles
$profiles = @()
foreach ($root in @("HKLM:\SOFTWARE\Microsoft\CTF\TIP\$clsid", "HKCU:\SOFTWARE\Microsoft\CTF\TIP\$clsid")) {
    if (Test-Path $root) {
        $profiles += Get-ChildItem "$root\LanguageProfile" -ErrorAction SilentlyContinue |
            ForEach-Object { $_.PSChildName }
    }
}
if ($profiles) { Write-Host ("Language profiles: " + ($profiles -join ", ")) }
else { Write-Host "  [!] No language profile found"; $ok = $false }

# 4. Binaries
$dll = Join-Path $PSScriptRoot "..\bin\Debug\ImeCore.dll"
$exe = Join-Path $PSScriptRoot "..\bin\Debug\LingxiUI.exe"
Write-Host ("ImeCore.dll exists: {0}  LingxiUI.exe exists: {1}" -f (Test-Path $dll), (Test-Path $exe))

if ($ok) { Write-Host "=== Self-check PASSED ===" } else { Write-Host "=== Self-check FAILED ===" }
exit $(if ($ok) { 0 } else { 1 })
