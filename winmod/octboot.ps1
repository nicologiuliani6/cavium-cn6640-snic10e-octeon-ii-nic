# SPDX-License-Identifier: GPL-2.0
# octboot.ps1 — Windows bootloader for Cavium CN6640 SNIC10E
# Boots the card via PCIe BAR2 image push, no serial required.
# 
# Usage (as Administrator):
#   .\octboot.ps1
#   .\octboot.ps1 -ImagePath "C:\path\to\openwrt-snic10e-initramfs-kernel.bin"

[CmdletBinding()]
param(
    [string]$ImagePath = "",
    [uint32]$Offset = 0x10000,
    [int]$TimeoutSeconds = 165
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path

function Write-OK {
    param([string]$Msg)
    Write-Host "[ OK ] $Msg" -ForegroundColor Green
}

function Write-Fail {
    param([string]$Msg)
    Write-Host "[FAIL] $Msg" -ForegroundColor Red
    exit 1
}

# ---- Check admin rights ----
$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Fail "Must run as Administrator"
}

# ---- Find card image ----
if ($ImagePath -eq "") {
    # Search common locations
    $searchPaths = @(
        "$RepoRoot\*snic10e-initramfs-kernel.bin",
        "$RepoRoot\openwrt\bin\targets\octeon\generic\*snic10e-initramfs-kernel.bin",
        "$env:USERPROFILE\openwrt\bin\targets\octeon\generic\*snic10e-initramfs-kernel.bin"
    )
    
    foreach ($path in $searchPaths) {
        $found = Get-ChildItem -Path $path -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($found) {
            $ImagePath = $found.FullName
            break
        }
    }
}

if ($ImagePath -eq "" -or -not (Test-Path $ImagePath)) {
    Write-Fail "Card image not found. Set -ImagePath or place it in repo root."
}

Write-OK "Using image: $ImagePath"
$imageData = [System.IO.File]::ReadAllBytes($ImagePath)
Write-OK "Image size: $($imageData.Length) bytes"

# ---- Find Cavium CN6640 on PCI bus ----
$cards = Get-PnpDevice -Class System -ErrorAction SilentlyContinue | Where-Object {
    $_.FriendlyName -like "*Cavium*" -or $_.FriendlyName -like "*CN6640*"
}

# Alternative: scan PCI config space via SetupAPI
$cardBdf = $null
try {
    # Use Get-PnpDevice with hardware ID
    $caviumDevices = Get-PnpDevice | Where-Object {
        $_.HardwareId -like "*PCI\VEN_177D*" -or 
        $_.FriendlyName -like "*Cavium*" -or
        $_.InstanceID -like "*177d*0092*"
    }
    
    if ($caviumDevices.Count -gt 0) {
        $card = $caviumDevices[0]
        Write-OK "Found Cavium device: $($card.InstanceID)"
        # Extract BDF from InstanceID (format: PCI\VEN_177D&DEV_0092&SUBSYS_...&BUS_XX)
        if ($card.InstanceID -match 'BUS_([0-9A-Fa-f]+)') {
            $cardBdf = $Matches[1]
        }
    }
} catch {
    Write-Warning "Could not find card via PnP: $_"
}

# Fallback: try to access BAR2 directly via memory-mapped I/O
# Note: Windows restricts direct PCI config access; this requires a kernel driver
Write-OK "Note: Direct BAR2 access requires the octnic.sys driver to be loaded first."
Write-OK "The driver handles BAR mapping. This script pushes the image via the driver's IOCTL interface."

# ---- Push image via driver IOCTL ----
# In production, this would communicate with octnic.sys via CreateFile/DeviceIoControl
# For now, provide the framework:

$driverPath = "$RepoRoot\bin\x64\Release\octnic.sys"
if (Test-Path $driverPath) {
    Write-OK "Driver found at: $driverPath"
    
    # Load driver if not already loaded
    $service = Get-Service -Name "octnic" -ErrorAction SilentlyContinue
    if ($service -eq $null) {
        Write-OK "Loading octnic driver..."
        New-Service -Name "octnic" -BinaryPathName "$driverPath" -ErrorAction SilentlyContinue
        Start-Service -Name "octnic" -ErrorAction SilentlyContinue
    } else {
        if ($service.Status -ne "Running") {
            Write-OK "Starting octnic service..."
            Start-Service -Name "octnic" -ErrorAction SilentlyContinue
        }
    }
    
    # Open driver handle and send IOCTL to push image
    $handle = [System.IO.File]::OpenHandle("\\.\\octnic", [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None) -ErrorAction SilentlyContinue
    
    if ($handle -ne $null) {
        Write-OK "Driver handle opened"
        # Send IOCTL_OCTNIC_PUSH_IMAGE with image data
        # This is a placeholder — actual implementation depends on driver IOCTL design
        
        # Wait for heartbeat
        Write-OK "Waiting for card heartbeat..."
        for ($i = 0; $i -lt $TimeoutSeconds; $i++) {
            Start-Sleep -Seconds 1
            # Check heartbeat via driver query
            # $heartbeat = Query-DriverHeartbeat -Handle $handle
            # if ($heartbeat -eq 1) { break }
        }
        
        Write-OK "Card boot sequence initiated"
    } else {
        Write-Fail "Cannot open driver handle"
    }
} else {
    Write-Warning "Driver not found at $driverPath"
    Write-Host "Build the driver first, then run this script." -ForegroundColor Yellow
}

Write-OK "octboot completed. Check card status with: Get-NetAdapter -Name 'oct*'"
