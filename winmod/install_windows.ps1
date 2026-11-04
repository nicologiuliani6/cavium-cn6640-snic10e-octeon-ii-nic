# SPDX-License-Identifier: GPL-2.0
# install_windows.ps1 — Windows side of the global installer (see README "Install").
# Run as Administrator (install.bat / install_windows.bat do the elevation). Idempotent.
#
# Flow (asks for confirmation before every step that changes something; -Yes answers yes):
#   1. card present on the PCI bus?
#   2. install the driver (needed to read the card): test cert, test-signing, octnic, port 1, NIC names
#   3. is the card already flashed (u-boot env provisioned for host boot)?
#        yes -> go on to 5
#        no  -> 4
#   4. look for the USB-serial adapter, test it, and (after confirmation) flash the u-boot env
#   5. register the boot task (card boots automatically at every Windows start), boot the card now
[CmdletBinding()]
param([string]$PackageDir = $PSScriptRoot, [switch]$Yes)
$ErrorActionPreference = 'Stop'
function Ok($m)   { Write-Host "[ OK ] $m" -ForegroundColor Green }
function Info($m) { Write-Host "[..] $m" }
function Warn($m) { Write-Host "[!] $m" -ForegroundColor Yellow }
function Fail($m) { Write-Host "[FAIL] $m" -ForegroundColor Red; exit 1 }
function Ask([string]$q) {
    if ($Yes) { Write-Host "[?] $q -> yes (-Yes)"; return $true }
    if (-not [Environment]::UserInteractive) { Warn "$q -> no (not interactive; use -Yes)"; return $false }
    return ((Read-Host "[?] $q [y/N]") -match '^[yY]')
}

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { Fail 'Run as Administrator' }
foreach ($f in 'octnic.sys', 'octnic.inf', 'octnic.cat', 'octnic-test.cer', 'octboot.ps1', 'octnic-addport.ps1', 'octnic-start.ps1', 'octnic-provision.ps1') {
    if (-not (Test-Path "$PackageDir\$f")) { Fail "missing $f in $PackageDir" }
}
$Dir = Join-Path $env:ProgramData 'octnic'
New-Item -ItemType Directory -Force $Dir | Out-Null
foreach ($f in 'octboot.ps1', 'octnic-start.ps1', 'octnic-provision.ps1') { Copy-Item "$PackageDir\$f" $Dir -Force }

# ---------- 1. card present? ----------
function CardPresent { [bool](Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'PCI\VEN_177D&DEV_0092*' }) }
if (CardPresent) { Ok 'Cavium CN6640 found on the PCI bus' }
else {
    Warn 'Cavium CN6640 (177d:0092) not found on the PCI bus - check it is seated in a PCIe slot.'
    if (-not (Ask 'Install the driver anyway?')) { Write-Host 'Cancelled: no card.'; exit 1 }
}

# ---------- 2. driver ----------
if (-not (Ask 'Install the octnic driver (trust the test certificate, install, create the second NIC)?')) { Write-Host 'Cancelled by user.'; exit 1 }
$cer = "$PackageDir\octnic-test.cer"
Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
Ok 'test certificate trusted'
if ((bcdedit /enum '{current}' | Out-String) -notmatch 'testsigning\s+Yes') {
    if (-not (Ask 'Windows test-signing is off and the driver is test-signed. Enable it (needs one reboot)?')) { Fail 'test-signing required' }
    bcdedit /set testsigning on | Out-Null
    if ($LASTEXITCODE) { Fail 'cannot enable test-signing: Secure Boot is ON. Disable Secure Boot in UEFI, then run again.' }
    Warn 'test-signing enabled: reboot Windows, then run this installer again.'
    exit 3010
}
Info 'installing driver'
pnputil /add-driver "$PackageDir\octnic.inf" /install | Out-Null
& "$PackageDir\octnic-addport.ps1" -Inf "$PackageDir\octnic.inf"
for ($i = 0; $i -lt 30; $i++) { if (Test-Path '\\.\octnic') { break }; Start-Sleep 1 }
function Rename-OctNics {
    foreach ($pair in @(@('*10GbE Port 0*', 'oct0'), @('*10GbE Port 1*', 'oct1'))) {
        $a = Get-NetAdapter -IncludeHidden -ErrorAction SilentlyContinue | Where-Object { $_.InterfaceDescription -like $pair[0] -and $_.Name -ne $pair[1] -and $_.Status -ne 'Not Present' } | Select-Object -First 1
        if ($a) { Rename-NetAdapter -Name $a.Name -NewName $pair[1] -ErrorAction SilentlyContinue }
    }
}
Start-Sleep 4; Rename-OctNics
Ok 'driver installed (oct0 / oct1)'

# ---------- 3. already flashed? ----------
function Get-CardState { $o = & "$Dir\octboot.ps1" -Probe 2>$null; ($o | Where-Object { $_ -match '^STATE=' } | Select-Object -Last 1) -replace '^STATE=', '' }
$state = Get-CardState
Info "card state: $state"
if ($state -eq 'dead') {
    Warn 'The card does not answer: its u-boot is not provisioned for host boot, or it is stuck.'
    if (Ask 'Try a secondary bus reset of the card and check again?') {
        & "$Dir\octboot.ps1" -SbrOnly | Out-Null
        Start-Sleep 3; $state = Get-CardState; Info "card state after reset: $state"
    }
}
if ($state -in 'osready', 'os', 'live') {
    Ok 'card already flashed (u-boot env provisioned): no serial needed'
} else {
    # ---------- 4. serial -> flash ----------
    Warn 'Card not flashed (or not reachable). Looking for the USB-serial adapter...'
    $port = & "$Dir\octnic-provision.ps1" -Detect
    if (-not $port) {
        Warn 'No USB-serial adapter found. Plug the FT232 adapter into the card console header and into this PC,'
        Warn 'then run the installer again. The driver is installed; nothing was written to the card.'
        exit 0
    }
    Ok "serial adapter found: $port"
    if (-not (Ask "Test the serial link on $port (sends a few carriage returns)?")) { Write-Host 'Stopped before touching the card.'; exit 0 }
    $chk = (& "$Dir\octnic-provision.ps1" -Check -Port $port | Where-Object { $_ -match '^SERIAL=' } | Select-Object -Last 1) -replace '^SERIAL=', ''
    Info "serial link: $chk"
    if ($chk -eq 'silent') {
        if (Ask 'Silent. Reset the card and test again (it prints the u-boot banner at reset)?') {
            $chk = (& "$Dir\octnic-provision.ps1" -Check -Sbr -Port $port | Where-Object { $_ -match '^SERIAL=' } | Select-Object -Last 1) -replace '^SERIAL=', ''
            Info "serial link after reset: $chk"
        }
    }
    if ($chk -notin 'uboot', 'console') { Warn 'The serial link does not work (wiring, port, or baud 115200 8N1). Fix it and run the installer again.'; exit 0 }
    Ok 'serial link works'
    if (-not (Ask 'Flash the card now? This writes the u-boot environment to its NAND (persistent, reversible with restore-bootapp.sh).')) { Write-Host 'Stopped before flashing.'; exit 0 }
    & "$Dir\octnic-provision.ps1" -Port $port -Yes
    if ($LASTEXITCODE -ne 0) { Fail 'flashing failed - see C:\ProgramData\octnic\provision.log' }
    Ok 'card flashed'
}

# ---------- 5. automation + first boot ----------
if (Ask 'Register the boot task "octnic-boot" (boots the card automatically at every Windows start; no-op if the card is already up)?') {
    $act = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-NoProfile -ExecutionPolicy Bypass -File `"$Dir\octnic-start.ps1`""
    $trg = New-ScheduledTaskTrigger -AtStartup
    $pri = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
    $set = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Minutes 30)
    Register-ScheduledTask -TaskName 'octnic-boot' -Action $act -Trigger $trg -Principal $pri -Settings $set -Force | Out-Null
    Ok 'boot task "octnic-boot" registered'
}
Get-ChildItem $PackageDir -Filter '*snic10e-initramfs-kernel.bin' -ErrorAction SilentlyContinue | Copy-Item -Destination $Dir -Force
if (Ask 'Boot the card now (pushes the OpenWrt image over PCIe, ~1-2 min)?') {
    & "$Dir\octboot.ps1" -Download
    Start-Sleep 4; Rename-OctNics
}
Get-NetAdapter -IncludeHidden | Where-Object InterfaceDescription -like '*SNIC10E*' | Format-Table Name, InterfaceDescription, Status -AutoSize
Ok 'done'
