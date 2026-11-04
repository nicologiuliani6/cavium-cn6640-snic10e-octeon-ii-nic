# SPDX-License-Identifier: GPL-2.0
# octnic-start.ps1 — runs at every Windows boot (scheduled task "octnic-boot", SYSTEM).
# 1. waits for the octnic driver / control device
# 2. names the two NICs oct0 / oct1
# 3. boots the card unless its OS is already up (e.g. a Linux session left it running)
#    -> never fights Linux: octboot.ps1 is a no-op when the card is alive.
$ErrorActionPreference = 'Continue'
$Dir = Join-Path $env:ProgramData 'octnic'
New-Item -ItemType Directory -Force $Dir | Out-Null
$Log = Join-Path $Dir 'start.log'
function Log($m) { Add-Content $Log "$(Get-Date -Format s) $m" }

function Rename-OctNics {
    foreach ($pair in @(@('*10GbE Port 0*', 'oct0'), @('*10GbE Port 1*', 'oct1'))) {
        $a = Get-NetAdapter -IncludeHidden -ErrorAction SilentlyContinue | Where-Object { $_.InterfaceDescription -like $pair[0] -and $_.Name -ne $pair[1] -and $_.Status -ne 'Not Present' } | Select-Object -First 1
        if ($a) { try { Rename-NetAdapter -Name $a.Name -NewName $pair[1] -ErrorAction Stop; Log "renamed $($a.Name) -> $($pair[1])" } catch { Log "rename failed: $_" } }
    }
}

# wait (up to 2 min) for both adapters
for ($i = 0; $i -lt 60; $i++) {
    $n = @(Get-NetAdapter -IncludeHidden -ErrorAction SilentlyContinue | Where-Object { $_.InterfaceDescription -like '*SNIC10E 10GbE Port*' -and $_.Status -ne 'Not Present' }).Count
    if ($n -ge 2) { break }
    Start-Sleep -Seconds 2
}
Log "adapters present: $n"
Rename-OctNics

Log 'running octboot.ps1'
& (Join-Path $Dir 'octboot.ps1') -Download -BootTimeoutSeconds 300 *>> $Log
Log "octboot exit code $LASTEXITCODE"
Rename-OctNics
