# octnic — Windows NDIS Miniport Driver

Windows kernel-mode driver for the **Cavium CN6640-SNIC10E** SmartNIC, exposing two independent 10 GbE interfaces (`oct0`, `oct1`) over the card's PCIe BAR2 shared-memory datapath.

## Architecture

Same BAR2 shared-memory layout as the Linux driver:

```
HOST (Windows)                    PCIe                        CARD (OpenWrt from RAM)
┌────────────────┐                               ┌───────────────────────────┐
│ octnic.sys     │   BAR2 window (64 MiB)        │ octshm_card (card kmod)   │
│ NDIS miniport  │◄───── shared-memory rings ────►│  per-port rings           │
│ oct0 / oct1    │                               │  DPI RX engine            │
│                │   TX: host PIO into BAR window│  XAUI tap (packet_type)   │
│                │   RX: card DPI DMA into host RAM│                         │
└────────────────┘                               └─────────────┬─────────────┘
                                                               │ XAUI + DAC
                                                               ▼  peer 10 GbE
```

## Shared-Memory Layout (per port, 4MB stride)

| Offset | Name | Size | Description |
|--------|------|------|-------------|
| `0x000000` | Control Page | 256 B | Magic, version, ready flags, ring indices, DMA bases, temps |
| `0x001000` | TX Descriptor Ring | 1 KB | 128 entries × 8 bytes `{len, phase}` |
| `0x002000` | RX Descriptor Ring | 1 KB | 128 entries × 8 bytes `{len, flags}` |
| `0x100000` | TX Buffer | ~4 MB | Host PIO writes frames here (write-combining) |
| `0x280000` | RX Buffer | ~3.5 MB | Card DPI DMA writes frames here |

**Per-port stride:** 4 MB (`0x400000`). With `ports=2`, each port gets its own 4 MB region and its own BAR1 index on the card side.

## Datapath

### TX (Host → Card) — PIO Fill
Host writes frames directly into the BAR2 TX buffer using write-combining memory. Slots are claimed via CAS (compare-and-swap) for parallel TX queues. The card drains by phase bit, supporting zero-copy PKO gather (`zc=1`).

### RX (Card → Host) — DPI DMA + Busy-Poll
The card's DPI engine DMA-writes received frames into host RAM. In `hrx=1` mode, an 8-byte `{len, phase}` header is DPI'd ahead of each frame, so the host reads descriptors from local RAM (no per-frame PCIe MMIO). Multiple busy-poll threads drain the ring in parallel, stride-partitioned by thread ID.

## Build Requirements

- **Windows 10/11 x64** (version 2004+)
- **Windows Driver Kit (WDK)** — latest version recommended
- **Visual Studio 2019/2022** with C++ build tools
- Code signing certificate for production deployment

## Build Steps

### Option 1: Visual Studio
1. Open `octnic.vcxproj` in Visual Studio
2. Select `x64` + `Release` configuration
3. Build (`Ctrl+Shift+B`)
4. Output: `bin\x64\Release\octnic.sys`

### Option 2: Command Line
```bash
# Source WDK environment
call "C:\Program Files (x86)\Windows Kits\10\build\windowsdriver.props"

# Build
msbuild octnic.vcxproj /p:Configuration=Release /p:Platform=x64
```

### Option 3: Batch Script
```bash
.\build_release.bat
```

## Installation

> Status: **tested on real hardware** (Windows 11 24H2, test-signed). Two NDIS adapters `oct0` / `oct1`, DHCP, ping and internet verified through the card. Port link follows the card's SFP+ link bit, so a port with no peer stays "Disconnected".

### How two NICs come from one PCI function
One PCI function can bind only one NDIS adapter. So **port 0** is the PCI device (`PCI\VEN_177D&DEV_0092`, owns the BAR2 mapping and the host DMA memory) and **port 1** is a root-enumerated companion device (`ROOT\OCTNIC_PORT1`, created by `octnic-addport.ps1`) that shares them. Both are served by the same `octnic.sys`; the INF writes `OctPort` per device section. Both show up in Device Manager and under *Network Connections*.

### Easy install (prebuilt, test-signed)

1. Download `install_windows.bat` (and `octnic-windows-x64.zip`) from the [latest release](https://github.com/nicologiuliani6/cavium-cn6640-snic10e-octeon-ii-nic/releases/latest) and double-click it. It asks for Administrator rights and then:
   - trusts the bundled test certificate (no "install this driver?" popup),
   - enables test-signing if off (`bcdedit /set testsigning on`; reboots once and **resumes by itself**),
   - installs the driver, creates the second port, names the NICs `oct0` / `oct1`,
   - registers the boot task **`octnic-boot`** (SYSTEM, at every Windows start) that boots the card — see below.
2. Verify: `Get-NetAdapter oct0, oct1`.

Requirements: Windows 10/11 x64, **Secure Boot off** (test-signing cannot be enabled otherwise).

### Card boot from Windows (automatic)
The card's OpenWrt runs from RAM and must be pushed by the host after power-on. `octboot.ps1` does what Linux `octboot` does, through the driver's control device `\\.\octnic` (Administrators/SYSTEM only): wait for the BAR2 window, push the image at `+0x10000` with verify + word-fix, write `GO!!` at `+0x100`, wait for the heartbeat; it falls back to a secondary bus reset (SBR) + retry. **It is a no-op when the card OS is already up**, so it never fights Linux (dual boot / warm reboot). The task `octnic-boot` runs `octnic-start.ps1` at boot: waits for the adapters, names them, runs `octboot.ps1 -Download` (image is fetched once from the release into `C:\ProgramData\octnic`). Logs: `C:\ProgramData\octnic\start.log`, `octboot.log`; driver log: `C:\Windows\octnic.log`.

Manual: `.\octboot.ps1 [-ImagePath file.bin] [-Force]` (as Administrator).

### Build from source
`build_local.bat` (VS Build Tools + WDK 10.0.26100, plain `cl`/`link`, no MSBuild WDK integration) → `out\octnic.sys`; `package.ps1` → test-signed `dist\` (INF + catalog + scripts). `octnic.vcxproj` is used by CI.

Uninstall: `pnputil /enum-drivers`, `pnputil /delete-driver oemNN.inf /uninstall`, `Unregister-ScheduledTask octnic-boot`.

### Production signing (no test-signing)

Needs a Microsoft-signed driver: EV code-signing certificate + Windows Hardware Dev Center (Partner Center) account, attestation signing.
`.\make-submission.ps1 -Zip octnic-windows-x64.zip -Thumbprint <EV cert sha1>` builds and signs `out\octnic.cab` for upload. Untested.

### Releases / CI

`.github/workflows/build-windows.yml` builds on every push touching `winmod/`, and on every published release it attaches `octnic-windows-x64.zip` and `install_windows.bat`.

## Usage

```powershell
Get-NetAdapter oct0, oct1                    # both NICs
New-NetIPAddress -InterfaceAlias oct0 -IPAddress 10.9.9.1 -PrefixLength 24
Set-NetIPInterface -InterfaceAlias oct1 -Dhcp Enabled
```
## Module Parameters (Registry)

Parameters are configured via Windows Registry under:
`HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\octnic\Parameters`

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `RxThreads` | DWORD | 2 | RX drain threads per port (power of 2, 1..8) |
| `PollUs` | DWORD | 20 | RX poll interval in microseconds |
| `Hrx` | DWORD | 1 | Host-RAM RX descriptors |
| `RxThreads` | DWORD | 8 | Parallel RX drain threads |
| `NTxQ` | DWORD | 8 | TX queues for multi-queue |
| `Lockfree` | DWORD | 1 | Lock-free ring mode |
| `Ztx` | DWORD | 0 | Zero-copy TX (off by default) |
| `DmaMode` | DWORD | 1 | Use DMA instead of PIO |

## Health Monitoring

The driver includes a watchdog that monitors:
- **Heartbeat**: Card liveness via counter in control page
- **Temperature**: Board and Octeon die temps from card sensors
- **PCIe Config**: Memory decode status
- **Link State**: SFP+ carrier detection

Alerts are logged to Windows Event Log under `Application and Services Logs\octnic`.

## Performance

Expected performance (same as Linux, hardware-limited):

| Direction | Speed | Notes |
|-----------|-------|-------|
| TX | ~10 GbE line-rate | Zero-copy PKO gather on card side |
| RX | ~8.1–8.8 GbE | DPI DMA + multi-thread drain |
| FWD (card→host via BAR2) | ~3.1 GbE* | *Known limitation: needs inbound-DPI/`ztx` |

See [docs/PERFORMANCE.md](../docs/PERFORMANCE.md) for detailed benchmarks.

## File Structure

```
winmod/
├── octnic.h              # Header: constants, structures, prototypes
├── octnic_main.c         # DriverEntry, NDIS registration
├── octnic_init.c         # PCI discovery, BAR mapping, port init
├── octnic_tx.c           # TX path: SendPackets → CAS → PIO write
├── octnic_rx.c           # RX path: poll threads → drain → indicate
├── octnic_oid.c          # OID handlers: MAC, stats, link state
├── octnic_watchdog.c     # Card health monitoring
├── octnic_utils.c        # Utilities: NBL pool, cloning, barriers
├── octnic.rc             # Windows resource file (version info)
├── octnic.vcxproj        # Visual Studio project file
├── Makefile              # WDK nmake build (alternative to VS)
├── build_release.bat     # Automated build script
├── octboot.ps1           # Windows bootloader (PowerShell)
└── README.md             # This file
```

## Differences from Linux Driver

| Feature | Linux (`linuxmod/octnic.c`) | Windows (`winmod/`) |
|---------|---------------------------|-------------------|
| Framework | Kernel module + netdev | NDIS 6.x miniport + KMDF |
| TX | `ndo_start_xmit` + CAS | `SendPackets` handler + InterlockedCompareExchange |
| RX | kthread + `local_bh_disable` | Win32 thread + `NdisMIndicateReceiveNetBufferLists` |
| Memory | `ioremap_wc()` | `MmMapIoSpace()` (NonCached) |
| Scheduling | `kthread_run()` + `schedule_delayed_work` | KDPC timer + IoAllocateWorkItem |
| HWMON | Linux hwmon subsystem | Windows Event Log |
| Multi-queue | `alloc_etherdev_mq()` | NDIS 802.3 multi-queue OID support |

## Troubleshooting

### Driver won't load
- Ensure **Test Signing** is enabled: `bcdedit /set testsigning on` (for development)
- Check Event Viewer for driver errors
- Verify code signing: `signtool verify /pa .\bin\x64\Release\octnic.sys`

### No link after boot
- Verify card is provisioned (one-time u-boot env via serial)
- Run `octboot.ps1` and check for heartbeat message
- Check DAC cable connection

### Low RX throughput
- Increase `RxThreads` in registry (max 8)
- Ensure `Hrx=1` is set (host-RAM descriptors)
- Check CPU affinity — poll threads should spread across cores

## License

GPL-2.0 — same as the Linux driver and card modules.

## See Also

- [Main README](../README.md) — Project overview, quick start
- [docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md) — Detailed architecture
- [docs/DMA-DESIGN.md](../docs/DMA-DESIGN.md) — DMA design rationale
- [docs/FLASHING.md](../docs/FLASHING.md) — Card flashing & booting
- [linuxmod/](../linuxmod/) — Linux kernel driver
