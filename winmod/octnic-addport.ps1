# SPDX-License-Identifier: GPL-2.0
# octnic-addport.ps1 — create the root-enumerated companion device ROOT\OCTNIC_PORT1
# (the second NIC, oct1) and bind it to octnic.inf. Idempotent. Run as Administrator.
#   .\octnic-addport.ps1 [-Inf C:\path\octnic.inf]
param([string]$Inf = (Join-Path $PSScriptRoot 'octnic.inf'))
$ErrorActionPreference = 'Stop'
$hwid = 'ROOT\OCTNIC_PORT1'

if (Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains $hwid }) {
    Write-Host "[ OK ] $hwid already present"; exit 0
}

Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices; using System.ComponentModel;
public static class OctSetup {
  [StructLayout(LayoutKind.Sequential)] public struct SP_DEVINFO_DATA { public int cbSize; public Guid ClassGuid; public int DevInst; public IntPtr Reserved; }
  [DllImport("setupapi.dll", SetLastError=true)] static extern IntPtr SetupDiCreateDeviceInfoList(ref Guid g, IntPtr h);
  [DllImport("setupapi.dll", SetLastError=true, CharSet=CharSet.Unicode)] static extern bool SetupDiCreateDeviceInfoW(IntPtr s, string name, ref Guid g, string desc, IntPtr h, int flags, ref SP_DEVINFO_DATA d);
  [DllImport("setupapi.dll", SetLastError=true, CharSet=CharSet.Unicode)] static extern bool SetupDiSetDeviceRegistryPropertyW(IntPtr s, ref SP_DEVINFO_DATA d, int prop, byte[] buf, int len);
  [DllImport("setupapi.dll", SetLastError=true)] static extern bool SetupDiCallClassInstaller(int fn, IntPtr s, ref SP_DEVINFO_DATA d);
  [DllImport("setupapi.dll")] static extern bool SetupDiDestroyDeviceInfoList(IntPtr s);
  [DllImport("newdev.dll", SetLastError=true, CharSet=CharSet.Unicode)] static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr h, string hwid, string inf, int flags, out bool reboot);
  public static void Create(string hwid, string inf) {
    Guid net = new Guid("4d36e972-e325-11ce-bfc1-08002be10318");
    IntPtr s = SetupDiCreateDeviceInfoList(ref net, IntPtr.Zero);
    if (s == (IntPtr)(-1)) throw new Win32Exception();
    try {
      SP_DEVINFO_DATA d = new SP_DEVINFO_DATA(); d.cbSize = Marshal.SizeOf(d);
      if (!SetupDiCreateDeviceInfoW(s, "NET", ref net, null, IntPtr.Zero, 1 /*DICD_GENERATE_ID*/, ref d)) throw new Win32Exception();
      byte[] b = System.Text.Encoding.Unicode.GetBytes(hwid + "\0\0");
      if (!SetupDiSetDeviceRegistryPropertyW(s, ref d, 1 /*SPDRP_HARDWAREID*/, b, b.Length)) throw new Win32Exception();
      if (!SetupDiCallClassInstaller(0x19 /*DIF_REGISTERDEVICE*/, s, ref d)) throw new Win32Exception();
    } finally { SetupDiDestroyDeviceInfoList(s); }
    bool reboot;
    if (!UpdateDriverForPlugAndPlayDevicesW(IntPtr.Zero, hwid, inf, 1 /*INSTALLFLAG_FORCE*/, out reboot)) throw new Win32Exception();
  }
}
'@
[OctSetup]::Create($hwid, (Resolve-Path $Inf).Path)
Write-Host "[ OK ] created $hwid"
