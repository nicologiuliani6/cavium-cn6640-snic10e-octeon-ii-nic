# AGENTS.md

This file provides essential context for working with this Cavium CN6640 SNIC10E NIC project.

## Project Overview

This is an out-of-tree Linux driver stack and boot tooling for the Cavium CN6640-SNIC10E (Octeon II CN6640) SmartNIC, exposing two independent 10 GbE host interfaces (`oct0`, `oct1`) over a reverse-engineered PCIe BAR2 shared-memory datapath. No vendor NDA firmware is required.

## Key Components

- `octboot` (host bash): Host bootloader that SBRs the card, restores BARs, pushes OpenWrt image to card DRAM over BAR2
- `octnic` (host kernel module): Registers `oct0`/`oct1` over the card's BAR2 window
- `octshm_card` (card kernel module): Card end of shared-memory datapath
- `octcarrier` (card kernel module): Un-gates XAUI TX on QLogic DAC
- `scripts/cavium-up.sh` / `scripts/nic-up.sh`: Orchestrate full bring-up

## Installation & Setup

### Prerequisites
- Linux host with Secure Boot disabled (kernel lockdown blocks setpci/BAR access)
- Modern Linux kernel (developed on 6.14, dkms recommended)
- USB-serial adapter for one-time u-boot provisioning only

### Install Process
```bash
# Download card OS image
curl -LO https://github.com/nicologiuliani6/cavium-cn6640-snic10e-octeon-ii-nic/releases/latest/download/openwrt-octeon-generic-snic10e-initramfs-kernel.bin

# Install driver and enable service (does not boot card)
sudo ./install.sh

# For first-time setup, persist card u-boot env
sudo ./scripts/card-prep-hostboot.sh

# Boot card and bring up NICs
sudo systemctl start cavium-nic
```

## Key Commands

- `sudo modprobe octnic ports=2` - Load host driver
- `sudo systemctl start cavium-nic` - Boot card + bring up NICs (no serial)
- `ip -br addr show oct0` - Check NIC status
- `sudo ./octboot` - Manual card boot (no serial)

## Important Notes

1. **Secure Boot**: Must be disabled as kernel lockdown blocks BAR access needed for driver operation.

2. **BAR Access**: Requires root privileges and direct PCIe register access via setpci.

3. **DKMS Support**: The driver supports DKMS for automatic rebuilding on kernel upgrades.

4. **Card Reset Handling**: Card resets/hangs are detected by the octnic module, which logs alerts and drops carrier.

5. **Module Parameters**: The `octnic` module has parameters like `temp_warn_mc` and `temp_crit_mc` that control temperature monitoring.

6. **Build Process**: Host module is built against running kernel headers using standard Makefile approach.

7. **Service Integration**: The systemd service (`cavium-nic.service`) runs at boot time to automatically bring up the card.

8. **One-time Setup**: Card requires one-time u-boot environment provisioning via serial cable, after which operation is serial-free.