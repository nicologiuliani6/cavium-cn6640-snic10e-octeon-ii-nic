# SPDX-License-Identifier: GPL-2.0
# package.ps1 — assemble + test-sign the driver package from winmod\out\octnic.sys.
#   .\package.ps1 [-Out dir]     (run build_local.bat first)
param([string]$Out = (Join-Path $PSScriptRoot 'dist'))
$ErrorActionPreference = 'Stop'
$bin = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0'
Remove-Item $Out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $Out | Out-Null
Copy-Item "$PSScriptRoot\out\octnic.sys","$PSScriptRoot\octnic.inf","$PSScriptRoot\octnic-addport.ps1","$PSScriptRoot\octnic-start.ps1","$PSScriptRoot\octboot.ps1","$PSScriptRoot\octnic-provision.ps1","$PSScriptRoot\install_windows.ps1","$PSScriptRoot\install_windows.bat" $Out
# unique, monotonically increasing DriverVer per build so Windows always prefers the new package
$build = [int]((Get-Date) - [datetime]'2026-11-01').TotalMinutes
$txt = (Get-Content "$Out\octnic.inf" -Raw) -replace 'DriverVer\s*=\s*[^\r\n]+', ("DriverVer   = {0},1.1.{1}.{2}" -f (Get-Date -Format 'MM/dd/yyyy'), [int][math]::Floor($build / 65535), ($build % 65535))
Set-Content "$Out\octnic.inf" $txt -Encoding ascii -NoNewline
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object Subject -eq 'CN=octnic test signing' | Select-Object -First 1
if (-not $cert) { $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=octnic test signing' -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(5) }
Export-Certificate -Cert $cert -FilePath "$Out\octnic-test.cer" | Out-Null
& "$bin\x86\Inf2Cat.exe" /driver:$Out /os:10_X64 /uselocaltime | Out-Null
foreach ($f in 'octnic.sys','octnic.cat') { & "$bin\x64\signtool.exe" sign /sha1 $cert.Thumbprint /fd SHA256 "$Out\$f" | Out-Null; if ($LASTEXITCODE) { throw "sign $f failed" } }
Get-ChildItem $Out | Select-Object Name, Length
