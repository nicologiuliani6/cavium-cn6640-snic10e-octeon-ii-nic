@echo off
REM SPDX-License-Identifier: GPL-2.0
REM install_windows.bat — install the prebuilt octnic driver (run as Administrator).
REM Package layout: octnic.sys, octnic.inf, octnic.cat, octnic-test.cer, this script.
setlocal
cd /d "%~dp0"

net session >nul 2>&1 || (echo [FAIL] Run as Administrator & pause & exit /b 1)

REM Test-signed build: trust the bundled certificate, and testsigning must be on.
if exist octnic-test.cer (
    certutil -addstore -f Root octnic-test.cer >nul
    certutil -addstore -f TrustedPublisher octnic-test.cer >nul
)
bcdedit /enum {current} | findstr /i "testsigning.*Yes" >nul
if errorlevel 1 (
    echo [WARN] Test signing is OFF. Run: bcdedit /set testsigning on  ^(then reboot, then re-run this script^)
    pause & exit /b 1
)

pnputil /add-driver octnic.inf /install
if errorlevel 1 (echo [FAIL] pnputil failed & pause & exit /b 1)
echo [OK] Installed. Check: Get-NetAdapter
echo Uninstall: pnputil /enum-drivers  then  pnputil /delete-driver oemNN.inf /uninstall
pause
