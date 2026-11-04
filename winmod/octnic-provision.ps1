# SPDX-License-Identifier: GPL-2.0
# octnic-provision.ps1 — Windows port of scripts/card-prep-hostboot.sh: ONE-TIME serial
# provisioning of a fresh card's u-boot environment, so it can be booted from the host
# with no serial cable ever again (octboot.ps1). Needs a USB-serial adapter (FT232) wired
# to the card's console and the octnic driver loaded (BAR addresses + SBR come from it).
#
#   .\octnic-provision.ps1 -Detect                 # print the serial port to use (or nothing)
#   .\octnic-provision.ps1 -Check [-Port COM5] [-Sbr]
#                                                  # does the serial link work? (prints SERIAL=...)
#   .\octnic-provision.ps1 [-Port COM5] [-Yes]     # provision: asks before writing the NAND env
#
# The env is saved to the card's NAND (`saveenv`); scripts/restore-bootapp.sh (Linux) puts the
# stock env back. UNTESTED on real hardware from Windows (the reference card was already
# provisioned from Linux): it is a line-for-line port of the Linux script.
[CmdletBinding()]
param(
    [string]$Port = '',
    [switch]$Detect,
    [switch]$Check,
    [switch]$Sbr,                # with -Check: reset the card first so u-boot prints its banner
    [switch]$Yes,                # do not ask before writing the env
    [int]$Baud = 115200,
    [int]$Sleep = 120,           # fallback u-boot pre-boot wait if the GO!! handshake is unavailable
    [string]$Addr = '0x20000000',
    [string]$Off = '0x10000',
    [int]$Rgo = 3
)
$ErrorActionPreference = 'Stop'
$DataDir = Join-Path $env:ProgramData 'octnic'
New-Item -ItemType Directory -Force $DataDir | Out-Null
$LogFile = Join-Path $DataDir 'provision.log'
function Log($m, $c = 'Gray') { $l = "$(Get-Date -Format s) $m"; Write-Host $l -ForegroundColor $c; Add-Content $LogFile $l }
function Fail($m) { Log "[FAIL] $m" Red; exit 1 }

# ---- serial port discovery: a USB serial adapter, never the motherboard COM1 ----
function Find-SerialPort {
    $c = @()
    $c += Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue | Where-Object { $_.PNPDeviceID -match '^(USB|FTDIBUS)' } | ForEach-Object { $_.DeviceID }
    $c += Get-PnpDevice -Class Ports -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match '^(USB|FTDIBUS)' -and $_.FriendlyName -match '\((COM\d+)\)' } | ForEach-Object { $Matches[1] }
    return ($c | Where-Object { $_ } | Select-Object -Unique | Select-Object -First 1)
}
if ($Detect) { $p = Find-SerialPort; if ($p) { Write-Output $p }; exit 0 }

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { Fail 'Run as Administrator' }
if (-not $Port) { $Port = Find-SerialPort }
if (-not $Port) { Fail 'no USB-serial adapter found (FT232 plugged in? use -Port COMx)' }

function Open-Serial {
    $sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
    $sp.ReadTimeout = 200; $sp.WriteTimeout = 2000; $sp.NewLine = "`r"
    $sp.Open(); $sp.DiscardInBuffer(); return $sp
}
$script:buf = New-Object System.Text.StringBuilder
function Pump($sp) { try { $r = $sp.ReadExisting(); if ($r) { [void]$script:buf.Append($r) } } catch {} }
function Send($sp, $line, [double]$wait = 1.2) { $sp.Write($line + "`r`n"); Start-Sleep -Milliseconds ([int]($wait * 1000)); Pump $sp }
function CrFlood($sp, [int]$n) { for ($i = 0; $i -lt $n; $i++) { $sp.Write("`r"); Start-Sleep -Milliseconds 30; Pump $sp } Start-Sleep 1; Pump $sp }
$BannerRx = 'Clearing DRAM|Octeon snic10e#|U-Boot'
$octboot = Join-Path $PSScriptRoot 'octboot.ps1'
if (-not (Test-Path $octboot)) { $octboot = Join-Path $DataDir 'octboot.ps1' }

# ---- -Check: does the serial link work? ----
if ($Check) {
    $sp = Open-Serial
    try {
        if ($Sbr) { & $octboot -SbrOnly | Out-Null; Start-Sleep 1; CrFlood $sp 700 } else { CrFlood $sp 40 }
        $t = $script:buf.ToString()
        if ($t -match $BannerRx) { Write-Output 'SERIAL=uboot' }
        elseif ($t.Trim().Length -gt 0) { Write-Output 'SERIAL=console' }
        else { Write-Output 'SERIAL=silent' }
    } finally { $sp.Close() }
    exit 0
}

# ---- provisioning ----
$card = Get-PnpDevice -PresentOnly | Where-Object InstanceId -like 'PCI\VEN_177D&DEV_0092*' | Select-Object -First 1
if (-not $card) { Fail 'Cavium 177d:0092 not on the PCI bus' }
$res = Get-CimInstance Win32_PnPAllocatedResource | Where-Object { ($_.Dependent.DeviceID -replace '\\\\', '\') -eq $card.InstanceId } | ForEach-Object { $_.Antecedent } | Where-Object { $_.StartingAddress }
$bar0 = ($res | Where-Object { ($_.EndingAddress - $_.StartingAddress + 1) -le 0x100000 } | Sort-Object StartingAddress | Select-Object -First 1).StartingAddress
$bar2 = ($res | Where-Object { ($_.EndingAddress - $_.StartingAddress + 1) -ge 0x2000000 } | Sort-Object StartingAddress | Select-Object -First 1).StartingAddress
if (-not $bar0 -or -not $bar2) { Fail 'cannot read the card BAR0/BAR2 from Windows (is the octnic driver loaded?)' }
$A = [Convert]::ToUInt64($Addr, 16); $O = [Convert]::ToUInt64($Off, 16)
$P2N_BAR0 = '0x{0:X16}' -f [uint64]$bar0
$P2N_BAR1 = '0x{0:X16}' -f [uint64]$bar2
$RBASE = [int]($A -shr 22)
$BADDR = '0x{0:X}' -f ($A + $O)
Log "card BAR0=$P2N_BAR0 BAR2=$P2N_BAR1 entry=$BADDR"
if (-not $Yes) {
    Write-Host "About to write a PERSISTENT u-boot environment (saveenv -> NAND) on the card over $Port." -ForegroundColor Yellow
    Write-Host 'It is reversible (scripts/restore-bootapp.sh) but it changes how the card boots.' -ForegroundColor Yellow
    if ((Read-Host 'Continue? [y/N]') -notmatch '^[yY]') { Log 'cancelled by user'; exit 2 }
}

$sp = Open-Serial
try {
    Log 'SBR + CR flood to catch the u-boot prompt...'
    & $octboot -SbrOnly | Out-Null
    Start-Sleep 1
    CrFlood $sp 700
    if ($script:buf.ToString() -notmatch $BannerRx) {
        Fail 'no u-boot banner/prompt - card wedged or not answering the SBR. Power-cycle the PC (full shutdown), then run again.'
    }
    Log 'at u-boot prompt, writing the self-programming env...'
    function Idx([int]$i) { 'write64 0x{0:X11} 0x{1:X}' -f (0x11800C00000A8 + $i * 8), ((($RBASE + $i) -shl 4) -bor 0xB) }
    Send $sp "setenv wa write64 0x00011800C0000080 $P2N_BAR0 \; write64 0x00011800C0000088 $P2N_BAR1"
    Send $sp ("setenv wb {0} \; {1} \; {2}" -f (Idx 0), (Idx 1), (Idx 2))
    Send $sp ("setenv wc {0} \; {1} \; {2}" -f (Idx 3), (Idx 4), (Idx 5))
    Send $sp ("setenv wd {0} \; {1} \; flush_l2c \; flush_dcache" -f (Idx 6), (Idx 7))
    Send $sp 'setenv bootdelay 1'
    $FLAG = '0x20000100'; $GO = '0x474f2121'
    Send $sp "setenv e itest.l *$FLAG == $GO"
    Send $sp "setenv n itest.l *$FLAG != $GO"
    Send $sp 'setenv s sleep 1 \; run wa \; run wb \; run wc \; run wd'
    Send $sp 'setenv w1 run e s'
    function Rep([string]$name, [int]$n) { ((1..$n | ForEach-Object { "run $name" }) -join ' \; ') }
    Send $sp ("setenv w10 " + (Rep 'w1' 10))
    Send $sp ("setenv w100 " + (Rep 'w10' 10))
    Send $sp ("setenv wg " + (Rep 'w100' 6))
    Send $sp "setenv bo flush_l2c \; flush_dcache \; bootoctlinux $BADDR numcores=8 endbootargs console=ttyS0,115200 octeon-ethernet.receive_group_order=$Rgo"
    # probe the handshake semantics on the live prompt (same as the Linux script)
    Send $sp 'setenv pa echo PROBE_HIT'
    Send $sp "mw.l $FLAG $GO 1"
    Send $sp 'run n pa'
    Send $sp "mw.l $FLAG 0 1"
    Send $sp 'run n pa'
    $hits = ([regex]::Matches($script:buf.ToString(), '(?m)^PROBE_HIT')).Count
    if ($hits -eq 2) { $BOOT = 'run wg \; run n bo'; Log "handshake wait OK (flag $FLAG)" }
    else { $BOOT = "sleep $Sleep \; run bo"; Log "[WARN] handshake probe failed -> fallback to sleep $Sleep" Yellow }
    Send $sp "setenv bootcmd mw.l $FLAG 0 1 \; run wa \; run wb \; run wc \; run wd \; $BOOT"
    Send $sp 'saveenv' 5
    foreach ($v in 'bootcmd', 'bo', 'wg', 'wa', 'bootdelay') { Send $sp "printenv $v" }
} finally { $sp.Close() }

Set-Content (Join-Path $DataDir 'provision-serial.log') $script:buf.ToString()
$out = $script:buf.ToString()
$ok = $out.Contains("bootoctlinux $BADDR") -and $out.Contains("0x00011800C0000088 $P2N_BAR1")
if ($ok) {
    Log '[ OK ] env provisioned + saved. The card now boots with no serial cable.' Green
    Log '       Boot it now:  .\octboot.ps1   (the boot task octnic-boot does it at every Windows start)' Green
    exit 0
}
Fail "could not confirm the env readback - see $DataDir\provision-serial.log"
