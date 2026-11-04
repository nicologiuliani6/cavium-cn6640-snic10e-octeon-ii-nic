# SPDX-License-Identifier: GPL-2.0
# octnic-diag.ps1 — dump card/driver state (needs Administrator). Output: C:\ProgramData\octnic\diag.txt
$out = Join-Path $env:ProgramData 'octnic\diag.txt'
$src = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'octboot.ps1'))
$i = $src.IndexOf('Add-Type'); $j = $src.IndexOf("'@", $i + 20)
Invoke-Expression ($src.Substring($i, $j + 2 - $i))
$o = @()
if (-not [OctCtl]::Open()) { $o += 'cannot open \\.\octnic'; $o | Set-Content $out; exit 1 }
$s = [OctCtl]::Status()
$n = 'ready','nCand','candLen','w0','w8','w100','w10000','hb','armed0','armed1','link0','link1'
for ($k = 0; $k -lt 12; $k++) { $o += ('{0,-8} {1:X8}' -f $n[$k], $s[$k]) }
foreach ($p in 0, 1) {
    $b = [OctCtl]::Read([uint32]($p * 0x400000), 0x44)
    $d = 0..16 | ForEach-Object { [BitConverter]::ToUInt32($b, $_ * 4) }
    $o += ('port{0}: magic={1:X8} ver={2} card_ready={3} host_ready={4} txprod={5} txcons={6} rxprod={7} rxcons={8} hb={9:X} dma_en={10} temp0={11} temp1={12} C_LINK={13:X8}' -f $p, $d[0], $d[1], $d[2], $d[3], $d[4], $d[5], $d[6], $d[7], $d[8], $d[9], $d[14], $d[15], $d[16])
}
Start-Sleep 2
$s2 = [OctCtl]::Status(); $o += ('hb {0:X} -> {1:X}' -f $s[7], $s2[7])
$o += (Get-NetAdapter | Where-Object InterfaceDescription -like '*SNIC10E*' | Format-Table Name, Status, LinkSpeed -Auto | Out-String)
$o | Set-Content $out
