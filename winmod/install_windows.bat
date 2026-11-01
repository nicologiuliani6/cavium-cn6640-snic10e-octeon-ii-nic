@echo off
REM SPDX-License-Identifier: GPL-2.0
REM install_windows.bat — one-click installer for the octnic Windows driver.
REM Double-click it (or run it from anywhere). It will, by itself:
REM   1. ask for Administrator rights (UAC)
REM   2. download the latest release zip if octnic.sys is not next to it
REM   3. turn on Windows test-signing if needed (reboots once, then continues alone)
REM   4. trust the bundled test certificate and install the driver
setlocal
title octnic installer
set "SELF=%~f0"
set "REPO=nicologiuliani6/cavium-cn6640-snic10e-octeon-ii-nic"

net session >nul 2>&1
if errorlevel 1 (
    powershell -NoProfile -Command "Start-Process -FilePath '%SELF%' -Verb RunAs"
    exit /b
)
cd /d "%~dp0"

if not exist octnic.sys (
    echo [..] Downloading latest octnic-windows-x64.zip ...
    set "DEST=%TEMP%\octnic-pkg"
    powershell -NoProfile -Command "$ErrorActionPreference='Stop'; [Net.ServicePointManager]::SecurityProtocol='Tls12'; $d=\"$env:TEMP\octnic-pkg\"; Remove-Item $d -Recurse -Force -ErrorAction SilentlyContinue; New-Item -ItemType Directory $d | Out-Null; $r=Invoke-RestMethod 'https://api.github.com/repos/%REPO%/releases/latest'; $a=$r.assets | Where-Object name -eq 'octnic-windows-x64.zip'; Invoke-WebRequest $a.browser_download_url -OutFile \"$d\pkg.zip\"; Expand-Archive \"$d\pkg.zip\" -DestinationPath $d -Force"
    if errorlevel 1 (echo [FAIL] Download failed. Get octnic-windows-x64.zip from the GitHub release and unzip it next to this file. & pause & exit /b 1)
    cd /d "%TEMP%\octnic-pkg"
)

REM Preflight: is the card on the PCI bus at all?
powershell -NoProfile -Command "if (Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_177D&DEV_0092*' }) { exit 0 } else { exit 1 }"
if errorlevel 1 (
    echo [FAIL] Cavium CN6640 card not found on the PCI bus ^(VEN_177D DEV_0092^).
    echo        Check it is seated and the PC saw it at boot, then run this file again.
    pause & exit /b 1
)
echo [OK] Card found on PCI bus

echo [..] Trusting test certificate
certutil -addstore -f Root octnic-test.cer >nul
certutil -addstore -f TrustedPublisher octnic-test.cer >nul

bcdedit /enum {current} | findstr /i /r "testsigning.*Yes" >nul
if errorlevel 1 (
    echo [..] Enabling test-signing
    bcdedit /set testsigning on >nul 2>&1
    if errorlevel 1 (
        echo [FAIL] Cannot enable test-signing: Secure Boot is ON.
        echo        Disable Secure Boot in the BIOS/UEFI, then run this file again.
        pause & exit /b 1
    )
    reg add HKLM\Software\Microsoft\Windows\CurrentVersion\RunOnce /v octnic /d "\"%SELF%\"" /f >nul
    echo [OK] Test-signing enabled. A reboot is required; installation continues automatically after login.
    choice /m "Reboot now"
    if not errorlevel 2 shutdown /r /t 5
    exit /b 0
)

echo [..] Installing driver
pnputil /add-driver octnic.inf /install
if errorlevel 1 (echo [FAIL] pnputil failed & pause & exit /b 1)

echo [..] Waiting for the NIC ^(the driver only starts if the card is already booted^)...
powershell -NoProfile -Command "$ok=$false; 1..15 | ForEach-Object { if (Get-NetAdapter -ErrorAction SilentlyContinue | Where-Object InterfaceDescription -like '*CN6640*') { $ok=$true; return }; Start-Sleep 1 }; if ($ok) { exit 0 } else { exit 1 }"
if errorlevel 1 (
    echo [WARN] Driver installed but no NIC appeared: the card is probably not booted yet.
    echo        Boot it with octboot.ps1, then run:  pnputil /restart-device "PCI\VEN_177D&DEV_0092*"
    echo        ^(or Device Manager: disable/enable the Cavium device^)
) else (
    echo [OK] NIC is up:
    powershell -NoProfile -Command "Get-NetAdapter | Where-Object InterfaceDescription -like '*CN6640*' | Format-Table Name,Status,LinkSpeed -AutoSize"
)
echo Uninstall: pnputil /enum-drivers  then  pnputil /delete-driver oemNN.inf /uninstall
pause
