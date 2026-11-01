# SPDX-License-Identifier: GPL-2.0
# make-submission.ps1 — build the .cab to upload to Microsoft Partner Center (attestation signing).
# Usage (PowerShell, WDK installed):
#   .\make-submission.ps1 -Zip octnic-windows-x64.zip -Thumbprint <EV cert sha1>
#   .\make-submission.ps1 -Sys x64\Release\octnic.sys            # no -Thumbprint: cab left unsigned
# Then upload out\octnic.cab at https://partner.microsoft.com/dashboard/hardware (Attestation signing).
param(
    [string]$Zip,
    [string]$Sys,
    [string]$Thumbprint,
    [string]$Timestamp = "http://timestamp.digicert.com"
)
$ErrorActionPreference = "Stop"
$bin = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin\10.*" -Directory | Sort-Object Name | Select-Object -Last 1
$work = Join-Path $PSScriptRoot "submission"
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $work, "$PSScriptRoot\out" | Out-Null

if ($Zip) { Expand-Archive $Zip $work -Force
            Remove-Item "$work\octnic-test.cer","$work\octnic.cat","$work\install_windows.bat" -ErrorAction SilentlyContinue }
elseif ($Sys) { Copy-Item $Sys $work; Copy-Item "$PSScriptRoot\octnic.inf" $work }
else { throw "pass -Zip or -Sys" }

# Microsoft re-signs the .sys; the test signature from CI is harmless but we ship the catalog fresh.
& "$($bin.FullName)\x86\Inf2Cat.exe" /driver:$work /os:10_X64 /uselocaltime
if ($LASTEXITCODE) { throw "Inf2Cat failed" }

@"
.OPTION EXPLICIT
.Set CabinetNameTemplate=octnic.cab
.Set DiskDirectoryTemplate=$PSScriptRoot\out
.Set CompressionType=MSZIP
.Set Cabinet=on
.Set Compress=on
.Set MaxCabinetSize=0
.Set MaxDiskFileCount=0
.Set MaxDiskSize=0
$work\octnic.sys
$work\octnic.inf
$work\octnic.cat
"@ | Set-Content "$work\octnic.ddf" -Encoding ASCII
makecab /f "$work\octnic.ddf"
if ($LASTEXITCODE) { throw "makecab failed" }

$cab = "$PSScriptRoot\out\octnic.cab"
if ($Thumbprint) {
    & "$($bin.FullName)\x64\signtool.exe" sign /sha1 $Thumbprint /fd SHA256 /tr $Timestamp /td SHA256 $cab
    if ($LASTEXITCODE) { throw "signtool failed (EV token plugged in / unlocked?)" }
} else { Write-Warning "No -Thumbprint: cab is UNSIGNED, Partner Center will reject it." }
Write-Host "Ready: $cab"
