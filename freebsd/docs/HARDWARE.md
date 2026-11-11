# Hardware

## The card

| | |
|---|---|
| **Model** (back sticker) | `CN6640-SNIC10E-1.1-G` |
| **Board id** (serial console) | `SNIC10E rev major:4 minor:1`, serial `4.1G1634-GB003886` |
| **SoC** | Cavium Octeon II **CN6640** -- 8-core MIPS64 @ 800 MHz (pass 1.2) |
| **PCI ID** | `177d:0092`, subsystem `177d:0001` |
| **RAM** | 2 GiB DDR3 (667 MHz / 1334 DDR) |
| **Storage** | 8 MiB NOR flash (u-boot + env) + 1 GiB NAND |
| **PHY** | Vitesse VSC8488 (dual 10 G) |
| **Ports** | 2x SFP+ -- XAUI interface 0 (`xaui0`) and interface 1 (`xaui1`) |
| **PCIe** | Gen2 x4, endpoint `177d:0092` (BDF is slot-dependent, e.g. `02:00.0`) |

The card is bus-powered. There is no independent power; the only way to fully power-cycle
the Octeon is a **host reboot** (a PCIe Secondary Bus Reset resets the PEM link but not the
SoC).

## FreeBSD-specific requirements

- **FreeBSD 14.x** (14.0+). The driver uses `ifnet` APIs available from FreeBSD 14 onward
  (`if_settransmitfn`, `if_getsoftc`, `if_input`, etc.).
- **FreeBSD source tree** at `/usr/src` for generated headers (`objops.h`, etc.).
  Check out with:
  ```sh
  sudo svn checkout https://svn.freebsd.org/base/release/14.2.0 /usr/src
  ```
- After checking out the source, generate required headers:
  ```sh
  cd /usr/src && make objops.h
  ```
  Place generated headers at `/home/nico/fbsd-inc` (or adjust `-I` paths in the Makefile).

## PCIe BARs (host view)

After the card has booted its NIC image and programmed its PEM inbound windows:

| BAR | host phys (example) | size | use |
|---|---|---|---|
| BAR0 | `0xc8000000` | 16 KiB | SLI control CSRs (indirect window) |
| BAR2 | `0xc4000000` | 64 MiB | card DRAM window -- the shared-memory NIC rings |

The FreeBSD driver (`if_octnic`) discovers BAR2 from PCI config space automatically; these
addresses are never hard-coded. On FreeBSD, BAR access uses `bus_space` (via `rman_get_bustag`
and `rman_get_bushandle`) rather than `ioremap`.

> **BAR0 indirect-window freeze hazard.** The SLI window registers (BAR0 `0x00-0x40`) can
> read/write arbitrary card NCB addresses, but `SLI_WINDOW_CTL` (BAR0 `0x2E0`) defaults to
> **0 = infinite wait**: a window read to a space that does not respond stalls the PCIe bus and
> **hard-freezes the host**. The vendor driver writes `0x200000` there first. On this board
> the window answers only for some devices -- DPI yes; DRAM, PEM and SLI-self all hang.
> Set `SLI_WINDOW_CTL` before any window access.

## Serial console (one-time provisioning only)

Normal operation is serial-free. You need the console **once** to persist the u-boot boot
environment (see [FLASHING](../docs/FLASHING.md)), and as a recovery path.

Header **J1** on the card, FT232 3.3 V USB-serial, **115200 8N1**:

| FT232 | Card J1 |
|---|---|
| GND | GND (bracket / J1 GND) |
| RX  | J1 pin 5 (card TX) |
| TX  | J1 pin 4 (card RX) |

```sh
# picocom at 115200; example with picocom
picocom -b 115200 /dev/cuaU0
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

The card-side `octcarrier` module un-gates each XAUI port's TX. The `ipd_port` values are:

| port | interface | `ipd_port` |
|---|---|---|
| `xaui0` | 0 | `0` |
| `xaui1` | 1 | `16` |

(`dev=xaui0,xaui1 ipd_port=0,16`). `16` = interface-1 port-0 on CN66xx XAUI; `oct1` TX passes
traffic only with it un-gated.

## Compatibility

No known FreeBSD-specific incompatibilities. The driver uses standard FreeBSD kernel APIs
(`bus_space`, `ifnet`, `sysctl`, `callout`, `taskqueue`) that have been stable since
FreeBSD 13. The shared-memory protocol is byte-identical to the Linux driver and does not
depend on any OS-specific card behavior beyond what `octboot` establishes.
