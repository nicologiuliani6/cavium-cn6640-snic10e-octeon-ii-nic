# SPDX-License-Identifier: GPL-2.0
# install_windows.ps1 — full installer for the octnic Windows driver (2 ports, auto start).
# Run as Administrator (install_windows.bat does the elevation). Idempotent.
#   - trusts the bundled test certificate (no "install this driver?" popup)
#   - installs octnic.inf (port 0 = PCI function) and creates the port-1 companion device
#   - names the NICs oct0 / oct1
#   - installs octboot.ps1 + a boot-time scheduled task that boots the card automatically
#     (no-op if the card is already running, e.g. left up by Linux)
[CmdletBinding()]
param([string]$PackageDir = $PSScriptRoot)
$ErrorActionPreference = 'Stop'
function Ok($m)   { Write-Host "[ OK ] $m" -ForegroundColor Green }
function Info($m) { Write-Host "[..] $m" }
function Fail($m) { Write-Host "[FAIL] $m" -ForegroundColor Red; exit 1 }

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { Fail 'Run as Administrator' }
foreach ($f in 'octnic.sys', 'octnic.inf', 'octnic.cat', 'octnic-test.cer') { if (-not (Test-Path "$PackageDir\$f")) { Fail "missing $f in $PackageDir" } }

if (-not (Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'PCI\VEN_177D&DEV_0092*' })) {
    Fail 'Cavium CN6640 card not found on the PCI bus (VEN_177D DEV_0092). Check it is seated, then run again.'
}
Ok 'card found on the PCI bus'

# ---- trust the test certificate (so Windows installs without prompting) ----
$cer = "$PackageDir\octnic-test.cer"
Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
Ok 'test certificate trusted'

# ---- test-signing required for the test-signed driver ----
if ((bcdedit /enum '{current}' | Out-String) -notmatch 'testsigning\s+Yes') {
    bcdedit /set testsigning on | Out-Null
    if ($LASTEXITCODE) { Fail 'cannot enable test-signing: Secure Boot is ON. Disable Secure Boot in UEFI, then run again.' }
    Write-Host '[WARN] test-signing enabled: reboot Windows, then run this installer again.' -ForegroundColor Yellow
    exit 3010
}

# ---- driver + port-1 device ----
Info 'installing driver'
pnputil /add-driver "$PackageDir\octnic.inf" /install | Out-Null
& "$PackageDir\octnic-addport.ps1" -Inf "$PackageDir\octnic.inf"

# ---- files + boot task ----
$Dir = Join-Path $env:ProgramData 'octnic'
New-Item -ItemType Directory -Force $Dir | Out-Null
foreach ($f in 'octboot.ps1', 'octnic-start.ps1') { if (Test-Path "$PackageDir\$f") { Copy-Item "$PackageDir\$f" $Dir -Force } }
Get-ChildItem $PackageDir -Filter '*snic10e-initramfs-kernel.bin' -ErrorAction SilentlyContinue | Copy-Item -Destination $Dir -Force

$act = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-NoProfile -ExecutionPolicy Bypass -File `"$Dir\octnic-start.ps1`""
$trg = New-ScheduledTaskTrigger -AtStartup
$pri = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
$set = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Minutes 30)
Register-ScheduledTask -TaskName 'octnic-boot' -Action $act -Trigger $trg -Principal $pri -Settings $set -Force | Out-Null
Ok 'boot task "octnic-boot" registered (boots the card at every Windows start)'

# ---- names + first boot now ----
Start-Sleep -Seconds 6
Start-ScheduledTask -TaskName 'octnic-boot'
Ok 'card boot started in the background (~1-2 min); follow it in C:\ProgramData\octnic\start.log'
Get-NetAdapter -IncludeHidden | Where-Object InterfaceDescription -like '*SNIC10E*' | Format-Table Name, InterfaceDescription, Status -AutoSize
