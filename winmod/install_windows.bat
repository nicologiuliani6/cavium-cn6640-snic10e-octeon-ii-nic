@echo off
REM SPDX-License-Identifier: GPL-2.0
REM install_windows.bat - one-click installer for the octnic Windows driver (2 ports, auto start).
REM Double-click it. It will, by itself:
REM   1. ask for Administrator rights (UAC)
REM   2. download the latest release zip if octnic.sys is not next to it
REM   3. run install_windows.ps1: trust the test cert, enable test-signing (one reboot,
REM      then it resumes alone), install the driver + the second port, register the
REM      boot task that starts the card automatically at every Windows boot
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
    powershell -NoProfile -Command "$ErrorActionPreference='Stop'; [Net.ServicePointManager]::SecurityProtocol='Tls12'; $d=\"$env:TEMP\octnic-pkg\"; Remove-Item $d -Recurse -Force -ErrorAction SilentlyContinue; New-Item -ItemType Directory $d | Out-Null; $r=Invoke-RestMethod 'https://api.github.com/repos/%REPO%/releases/latest'; $a=$r.assets | Where-Object name -eq 'octnic-windows-x64.zip'; Invoke-WebRequest $a.browser_download_url -OutFile \"$d\pkg.zip\"; Expand-Archive \"$d\pkg.zip\" -DestinationPath $d -Force"
    if errorlevel 1 (echo [FAIL] Download failed. Get octnic-windows-x64.zip from the GitHub release and unzip it next to this file. & pause & exit /b 1)
    cd /d "%TEMP%\octnic-pkg"
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%CD%\install_windows.ps1" -PackageDir "%CD%"
if "%errorlevel%"=="3010" (
    reg add HKLM\Software\Microsoft\Windows\CurrentVersion\RunOnce /v octnic /d "\"%SELF%\"" /f >nul
    echo [OK] Test-signing enabled. Reboot required; installation continues automatically after login.
    choice /m "Reboot now"
    if not errorlevel 2 shutdown /r /t 5
    exit /b 0
)
echo.
echo Uninstall: pnputil /enum-drivers  then  pnputil /delete-driver oemNN.inf /uninstall ; Unregister-ScheduledTask octnic-boot
pause
