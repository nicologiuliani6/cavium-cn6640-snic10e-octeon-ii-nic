# Hardware

## The card

| | |
|---|---|
| **Model** (back sticker) | `CN6640-SNIC10E-1.1-G` |
| **Board id** (serial console) | `SNIC10E rev major:4 minor:1`, serial `4.1G1634-GB003886` |
| **SoC** | Cavium Octeon II **CN6640** — 8-core MIPS64 @ 800 MHz (pass 1.2) |
| **PCI ID** | `177d:0092`, subsystem `177d:0001` |
| **RAM** | 2 GiB DDR3 (667 MHz / 1334 DDR) |
| **Storage** | 8 MiB NOR flash (u-boot + env) + 1 GiB NAND |
| **PHY** | Vitesse VSC8488 (dual 10 G) |
| **Ports** | 2× SFP+ — XAUI interface 0 (`xaui0`) and interface 1 (`xaui1`) |
| **PCIe** | Gen2 x4, endpoint `177d:0092` (BDF is slot-dependent, e.g. `02:00.0`) |

The card is bus-powered. There is no independent power; the only way to fully power-cycle
the Octeon is a **host reboot** (a PCIe Secondary Bus Reset resets the PEM link but not the
SoC).

## BIOS

- **Secure Boot must be off.** With Secure Boot on the kernel runs in `lockdown=integrity`:
  `setpci` and the BAR `mmap`s this stack needs return `EPERM`, and unsigned out-of-tree
  modules are refused ("Key was rejected by service").
- **"Above 4G decoding" is *not* required.** Both BARs are 64-bit prefetchable but small
  enough (16 KiB + 64 MiB) to be assigned below 4 GiB, and that is where this machine's BIOS
  puts them with the option **off** (`0xc8000000` / `0xc4000000`). Enable it only if the BIOS
  runs out of 32-bit MMIO space and leaves BAR2 unassigned — `lspci -s <BDF> -v` then shows
  `[virtual]` or a zero-size Region 2 and `octnic` won't find the card.
- Leave the card's slot at Gen2; it trains at 5 GT/s x4.

## PCIe BARs (host view)

After the card has booted its NIC image and programmed its PEM inbound windows:

| BAR | host phys (example) | size | use |
|---|---|---|---|
| BAR0 | `0xc8000000` | 16 KiB | SLI control CSRs (indirect window) |
| BAR2 | `0xc4000000` | 64 MiB | card DRAM window — the shared-memory NIC rings |

`octnic` auto-discovers BAR2 from the PCI device; these addresses are never hard-coded.

> ⚠️ **BAR0 indirect-window freeze hazard.** The SLI window registers (BAR0 `0x00–0x40`) can
> read/write arbitrary card NCB addresses, but `SLI_WINDOW_CTL` (BAR0 `0x2E0`) defaults to
> **0 = infinite wait**: a window read to a space that doesn't respond stalls the PCIe bus and
> **hard-freezes the host** (three freezes during the zero-serial experiments). The vendor
> driver writes `0x200000` there first ("avoid host hang when reads invalid register"). On this
> board the window answers only for some devices — DPI yes; DRAM, PEM and SLI-self all hang.
> Set `SLI_WINDOW_CTL` before any window access.

## Serial console (one-time provisioning only)

Normal operation is serial-free. You need the console **once** to persist the u-boot boot
environment (see [FLASHING](FLASHING.md)), and as a recovery path.

Header **J1** on the card, FT232 3.3 V USB-serial, **115200 8N1**:

| FT232 | Card J1 |
|---|---|
| GND | GND (bracket / J1 GND) |
| RX  | J1 pin 5 (card TX) |
| TX  | J1 pin 4 (card RX) |

```bash
# minicom / picocom at 115200; example with picocom
picocom -b 115200 /dev/ttyUSB0
```

Do **not** cross RX/TX the other way; pin 4 = card RX, pin 5 = card TX.

## Cabling for two ports

Each SFP+ port pairs 1:1 with a peer port over a DAC:

```
card xaui0  <--DAC-->  peer port A   (=> host oct0)
card xaui1  <--DAC-->  peer port B   (=> host oct1)
```

Both ports are independent: one cabled port is enough to use the card.

## XAUI `ipd_port` mapping

`octcarrier` un-gates each XAUI port's TX. The card-side `ipd_port` values are:

| port | interface | `ipd_port` |
|---|---|---|
| `xaui0` | 0 | `0` |
| `xaui1` | 1 | `16` |

(`dev=xaui0,xaui1 ipd_port=0,16`). `16` = interface-1 port-0 on CN66xx XAUI; `oct1` TX passes
traffic only with it un-gated.
