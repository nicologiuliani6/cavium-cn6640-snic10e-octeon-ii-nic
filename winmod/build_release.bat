@echo off
REM SPDX-License-Identifier: GPL-2.0
REM build_release.bat — Build octnic Windows driver in release mode
REM Requires: Windows Driver Kit (WDK) installed
REM Usage: Run from Developer Command Prompt for VS or WDK

setlocal enabledelayedexpansion

echo ========================================
echo Building octnic Windows driver
echo ========================================

REM Find WDK installation
set WDK_PATH=
if exist "C:\Program Files (x86)\Windows Kits\10\build\windowsdriver.props" (
    for /f "tokens=*" %%i in ('dir "C:\Program Files (x86)\Windows Kits\10\build" /b /o:n 2^>nul') do set WDK_VER=%%i
    set WDK_PATH=C:\Program Files (x86)\Windows Kits\10\build\!WDK_VER!
)

if "!WDK_PATH!"=="" (
    echo ERROR: WDK not found. Please install Windows Driver Kit.
    echo Download from: https://docs.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk
    exit /b 1
)

echo Found WDK at !WDK_PATH!

REM Set build environment
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" 2>nul || ^
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" 2>nul || ^
echo WARNING: Could not find Visual Studio vcvars64.bat

REM Build with msbuild
set SOLUTION_FILE=%~dp0octnic.vcxproj
set CONFIGURATION=Release
set PLATFORM=x64

echo Building %CONFIGURATION% %PLATFORM%...
msbuild "%SOLUTION_FILE%" /p:Configuration=%CONFIGURATION% /p:Platform=%PLATFORM% /m /v:minimal

if %errorlevel% neq 0 (
    echo ERROR: Build failed with error level !errorlevel!
    exit /b !errorlevel!
)

REM Check for output
if exist "%~dp0bin\%PLATFORM%\%CONFIGURATION%\octnic.sys" (
    echo ========================================
    echo Build successful!
    echo Output: %~dp0bin\%PLATFORM%\%CONFIGURATION%\octnic.sys
    echo ========================================
) else (
    echo WARNING: Could not find octnic.sys in expected location
    dir /s "%~dp0*.sys" 2>nul
)

endlocal
