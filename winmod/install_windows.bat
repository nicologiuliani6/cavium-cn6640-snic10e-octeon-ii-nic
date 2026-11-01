@echo off
REM SPDX-License-Identifier: GPL-2.0
REM install_windows.bat — Install and start the octnic Windows driver service
REM Usage (as Administrator):
REM   install_windows.bat [path\to\octnic.sys]
REM
REM If no path is given, it looks for octnic.sys in the same directory as this script.

setlocal enabledelayedexpansion

set SCRIPT_DIR=%~dp0
set DRIVER_PATH=%SCRIPT_DIR%octnic.sys

REM Override if path argument provided
if "%1" neq "" (
    set DRIVER_PATH=%1
)

echo ========================================
echo Octnic Windows Driver Installer
echo ========================================

REM Check admin rights
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo [FAIL] Must run as Administrator
    echo Right-click and select "Run as administrator"
    pause
    exit /b 1
)

REM Check driver file exists
if not exist "%DRIVER_PATH%" (
    echo [FAIL] Driver not found: %DRIVER_PATH%
    echo Place octnic.sys in the same directory or specify path.
    pause
    exit /b 1
)

echo [OK] Driver file: %DRIVER_PATH%

REM Check if service already exists
sc query octnic >nul 2>&1
if %errorLevel% equ 0 (
    echo [INFO] Service 'octnic' already exists
    sc stop octnic >nul 2>&1
    sc delete octnic >nul 2>&1
    echo [OK] Removed old service
)

REM Create service
echo [INFO] Creating service 'octnic'...
sc create octnic binPath= "%DRIVER_PATH%" type= kernel
if %errorLevel% neq 0 (
    echo [FAIL] Failed to create service
    pause
    exit /b 1
)
echo [OK] Service created

REM Start service
echo [INFO] Starting service 'octnic'...
sc start octnic
if %errorLevel% neq 0 (
    echo [WARN] Service failed to start
    echo This may require Test Signing mode or a valid code signature.
    echo To enable test signing: bcdedit /set testsigning on (requires reboot)
    pause
    exit /b 1
)

echo [OK] Service started successfully

REM Verify installation
echo.
echo ========================================
echo Verification
echo ========================================
Get-NetAdapter -Name 'oct*' 2>nul || echo Note: Run 'Get-NetAdapter -Name oct*' to verify adapters

echo.
echo ========================================
echo Installation Complete
echo ========================================
echo To uninstall:
echo   sc stop octnic
echo   sc delete octnic
echo.
echo To check status:
echo   sc query octnic
echo   Get-NetAdapter -Name 'oct*'
pause
