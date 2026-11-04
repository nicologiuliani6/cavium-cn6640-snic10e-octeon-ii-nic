# SPDX-License-Identifier: GPL-2.0
# octboot.ps1 â€” Windows host bootloader for the Cavium CN6640 SNIC10E (no serial).
# Same flow as the Linux `octboot` fast path, through the octnic driver's control
# device \\.\octnic instead of /sys/.../resource2:
#   1. if the card OS is already up (e.g. booted by Linux / earlier run) -> do nothing
#   2. wait for the BAR2 window (u-boot waiting), push the OpenWrt image at +0x10000
#      with verify + word-fix, write "GO!!" at +0x100, wait for the card heartbeat.
# Windows cannot issue a secondary bus reset: if the window never comes live, power-cycle
# the PC (cold boot) and run again.
#
#   .\octboot.ps1 [-ImagePath file.bin] [-Force] [-Download]
[CmdletBinding()]
param(
    [string]$ImagePath = '',
    [switch]$Force,                 # reload even if the card OS is already up
    [switch]$Download,              # fetch the image from the latest GitHub release if missing
    [switch]$Probe,                 # print STATE=osready|os|live|dead and exit (no image needed)
    [switch]$SbrOnly,               # secondary bus reset + restore card config, then exit
    [int]$WindowTimeoutSeconds = 90,
    [int]$BootTimeoutSeconds = 240,
    [string]$Repo = 'nicologiuliani6/cavium-cn6640-snic10e-octeon-ii-nic'
)
$ErrorActionPreference = 'Stop'
$Magic = 0x4f435348
$Off   = 0x10000
$ImgName = 'openwrt-octeon-generic-snic10e-initramfs-kernel.bin'
$DataDir = Join-Path $env:ProgramData 'octnic'
$LogFile = Join-Path $DataDir 'octboot.log'
New-Item -ItemType Directory -Force $DataDir | Out-Null
function Log($m, $c = 'Gray') { $l = "$(Get-Date -Format s) $m"; Write-Host $l -ForegroundColor $c; Add-Content $LogFile $l }
function Fail($m) { Log "[FAIL] $m" Red; exit 1 }

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { Fail 'Run as Administrator' }

Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices; using Microsoft.Win32.SafeHandles;
public static class OctCtl {
  [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)] static extern SafeFileHandle CreateFileW(string n, uint acc, uint share, IntPtr sa, uint disp, uint fl, IntPtr t);
  [DllImport("kernel32.dll", SetLastError=true)] static extern bool DeviceIoControl(SafeFileHandle h, uint code, byte[] inb, int inl, byte[] outb, int outl, out int ret, IntPtr ov);
  static SafeFileHandle h; public static int LastErr;
  public static bool Open() { h = CreateFileW(@"\\.\octnic", 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero); return !h.IsInvalid; }
  public static uint[] Status() {
    byte[] o = new byte[48]; int r;
    if (!DeviceIoControl(h, 0x8F002000, null, 0, o, o.Length, out r, IntPtr.Zero)) { LastErr = Marshal.GetLastWin32Error(); return null; }
    uint[] v = new uint[12]; Buffer.BlockCopy(o, 0, v, 0, 48); return v; }
  // returns words still bad after verify/fix, or -1 on error
  public static int Write(uint off, byte[] data, int n) {
    byte[] b = new byte[8 + n]; BitConverter.GetBytes(off).CopyTo(b, 0); BitConverter.GetBytes((uint)n).CopyTo(b, 4);
    Buffer.BlockCopy(data, 0, b, 8, n); int r;
    if (!DeviceIoControl(h, 0x8F002004, b, b.Length, b, b.Length, out r, IntPtr.Zero)) return -1;
    return BitConverter.ToInt32(b, 0); }
  public static int Sbr(uint bb, uint bs, uint cb, uint cs) {
    // SBR is requested through IOCTL_OCT_WRITE at the special offset 0xFFFFFFFC; payload = 4 x uint32
    byte[] b = new byte[24]; BitConverter.GetBytes(0xFFFFFFFCu).CopyTo(b,0); BitConverter.GetBytes(16u).CopyTo(b,4);
    BitConverter.GetBytes(bb).CopyTo(b,8); BitConverter.GetBytes(bs).CopyTo(b,12);
    BitConverter.GetBytes(cb).CopyTo(b,16); BitConverter.GetBytes(cs).CopyTo(b,20); int r;
    if (!DeviceIoControl(h, 0x8F002004, b, b.Length, b, b.Length, out r, IntPtr.Zero)) return Marshal.GetLastWin32Error();
    SbrDiag = new uint[4]; Buffer.BlockCopy(b, 0, SbrDiag, 0, 16);
    return SbrDiag[3] == 0 ? 0 : -1; }
  public static uint[] SbrDiag;
  public static byte[] Read(uint off, int n) {
    byte[] b = new byte[Math.Max(8, n)]; BitConverter.GetBytes(off).CopyTo(b, 0); BitConverter.GetBytes((uint)n).CopyTo(b, 4); int r;
    if (!DeviceIoControl(h, 0x8F002008, b, 8, b, b.Length, out r, IntPtr.Zero)) return null;
    byte[] o = new byte[n]; Buffer.BlockCopy(b, 0, o, 0, n); return o; }
}
'@

if (-not [OctCtl]::Open()) { Fail 'cannot open \\.\octnic (octnic driver not loaded / not running as Administrator)' }
$s = [OctCtl]::Status()
if (-not $s) { Fail "status ioctl failed (win32 error $([OctCtl]::LastErr); 21=device not ready: PCI device not started / BAR2 not mapped)" }
function CardUp($s) { return ($s[3] -eq $Magic -and $s[4] -ne 0) }

if ((CardUp $s) -and -not $Force -and -not $Probe -and -not $SbrOnly) { Log '[ OK ] card OS already up and ready - nothing to do' Green; exit 0 }

# ---- find / fetch the card image ----
if (-not ($Probe -or $SbrOnly)) {
if (-not $ImagePath) {
    $cands = @("$PSScriptRoot\$ImgName", "$PSScriptRoot\*snic10e-initramfs-kernel.bin", "$PSScriptRoot\..\$ImgName", "$DataDir\$ImgName")
    foreach ($c in $cands) { $f = Get-ChildItem $c -ErrorAction SilentlyContinue | Select-Object -First 1; if ($f) { $ImagePath = $f.FullName; break } }
}
if ((-not $ImagePath -or -not (Test-Path $ImagePath)) -and $Download) {
    Log "downloading $ImgName from GitHub release..."
    [Net.ServicePointManager]::SecurityProtocol = 'Tls12'
    $rel = Invoke-RestMethod "https://api.github.com/repos/$Repo/releases/latest"
    $as = $rel.assets | Where-Object name -eq $ImgName
    if (-not $as) { Fail "asset $ImgName not in latest release" }
    $ImagePath = Join-Path $DataDir $ImgName
    Invoke-WebRequest $as.browser_download_url -OutFile $ImagePath
}
if (-not $ImagePath -or -not (Test-Path $ImagePath)) { Fail "card image not found (use -ImagePath or -Download)" }
$img = [IO.File]::ReadAllBytes($ImagePath)
Log "[ OK ] image $ImagePath ($($img.Length) bytes)" Green
}

# ---- PCI location of the card + upstream bridge (for the secondary bus reset) ----
function PciLoc($id) {
    $b = (Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_BusNumber).Data
    $a = (Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_Address).Data
    return @{ Bus = [uint32]$b; Slot = [uint32](((($a -shr 16) -band 0x1f)) -bor (($a -band 7) -shl 5)) }
}
function Invoke-Sbr {
    $card = Get-PnpDevice -PresentOnly | Where-Object InstanceId -like 'PCI\VEN_177D&DEV_0092*' | Select-Object -First 1
    $par = (Get-PnpDeviceProperty -InstanceId $card.InstanceId -KeyName DEVPKEY_Device_Parent).Data
    $c = PciLoc $card.InstanceId; $p = PciLoc $par
    Log "secondary bus reset: card bus $($c.Bus) slot $($c.Slot), bridge bus $($p.Bus) slot $($p.Slot)"
    $e = [OctCtl]::Sbr($p.Bus, $p.Slot, $c.Bus, $c.Slot)
    if ($e -eq -1) { Fail ("SBR refused by driver: card ids {0:X8} bridge hdr {1} secondary bus {2} (expected 0092177D / 1 / {3})" -f [OctCtl]::SbrDiag[0], [OctCtl]::SbrDiag[1], [OctCtl]::SbrDiag[2], $c.Bus) }
    if ($e) { Fail "SBR ioctl failed (win32 $e)" }
    Log '[ OK ] SBR done, card config restored' Green
}
function Wait-Window {
    $mark = [Text.Encoding]::ASCII.GetBytes('NSRLIVE0')
    $deadline = (Get-Date).AddSeconds($WindowTimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        if ([OctCtl]::Write($Off, $mark, 8) -eq 0) {
            $rb = [OctCtl]::Read($Off, 8)
            if ($rb -and [Text.Encoding]::ASCII.GetString($rb) -eq 'NSRLIVE0') { return $true }
        }
        Start-Sleep -Milliseconds 500
    }
    return $false
}
function Push-Image {
    $chunk = 1MB; $pos = 0
    $pad = $img
    if ($img.Length % 4) { $pad = New-Object byte[] ($img.Length + 4 - ($img.Length % 4)); [Array]::Copy($img, $pad, $img.Length) }
    while ($pos -lt $pad.Length) {
        $n = [Math]::Min($chunk, $pad.Length - $pos)
        $buf = New-Object byte[] $n; [Array]::Copy($pad, $pos, $buf, 0, $n)
        $bad = [OctCtl]::Write([uint32]($Off + $pos), $buf, $n)
        if ($bad -ne 0) { Fail "DRAM write failed at +$pos (bad words: $bad)" }
        $pos += $n
    }
    Log "[ OK ] image uploaded ($($pad.Length) bytes, verified)" Green
    # handshake: u-boot waits for GO!! at DRAM 0x20000100 (= BAR2+0x100)
    $go = [Text.Encoding]::ASCII.GetBytes('GO!!')
    for ($i = 0; $i -lt 20; $i++) {
        [void][OctCtl]::Write(0x100, $go, 4)
        $r = [OctCtl]::Read(0x100, 4)
        if ($r -and [Text.Encoding]::ASCII.GetString($r) -eq 'GO!!') { break }
    }
    Log '[ OK ] booting (u-boot flush -> bootoctlinux)...' Green
}
function Wait-Card([int]$secs) {
    $deadline = (Get-Date).AddSeconds($secs)
    while ((Get-Date) -lt $deadline) {
        $s = [OctCtl]::Status()
        if ($s -and (CardUp $s)) { return $true }
        Start-Sleep -Seconds 1
    }
    return $false
}

if ($SbrOnly) { Invoke-Sbr; exit 0 }
if ($Probe) {
    $WindowTimeoutSeconds = 3
    $st = if (CardUp $s) { 'osready' } elseif ($s[3] -eq $Magic) { 'os' } elseif (Wait-Window) { 'live' } else { 'dead' }
    Write-Output "STATE=$st"; exit 0
}
# attempt 1: fast path (u-boot waiting with a live window); attempt 2: after a secondary bus reset
if ($Force -and (CardUp $s)) { Invoke-Sbr }
if (-not (Wait-Window)) { Log 'window not live -> SBR' Yellow; Invoke-Sbr; if (-not (Wait-Window)) { Fail 'BAR2 window never writable even after SBR. Power-cycle the PC, then run again.' } }
Log '[ OK ] BAR2 window live' Green
Push-Image
if (Wait-Card 75) { Log '[ OK ] card up - oct0/oct1 come up within ~2 s' Green; exit 0 }
Log 'no heartbeat -> SBR and retry' Yellow
Invoke-Sbr
if (-not (Wait-Window)) { Fail 'BAR2 window never writable after SBR' }
Push-Image
if (Wait-Card $BootTimeoutSeconds) { Log '[ OK ] card up - oct0/oct1 come up within ~2 s' Green; exit 0 }
Log "[WARN] no heartbeat after ${BootTimeoutSeconds}s; card may still be booting" Yellow
exit 2