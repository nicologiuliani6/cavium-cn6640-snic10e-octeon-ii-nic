# Usage

## Hands-off (systemd)

`system/cavium-nic.service` runs `scripts/cavium-up.sh` at boot: it boots the card with
`octboot`, then `scripts/nic-up.sh` loads `octnic ports=2` and brings both ports up.

The one-shot installer does all of the below in one go — it registers `octnic` with DKMS
when that is available (so the module survives kernel upgrades) and otherwise builds it in
place, drops the two host configs, and installs the service:

```bash
sudo ./install.sh            # add --start to also boot the card + bring NICs up now
sudo systemctl start cavium-nic
```

Or by hand — install the service:

```bash
sudo cp system/cavium-nic.service /etc/systemd/system/
sudo systemctl enable --now cavium-nic
```

Two host requirements (shipped as files in `system/`, installed by `install.sh`, or by hand once):

```bash
# 1) keep the stock liquidio driver off the card (it hangs the host probing this OEM board)
sudo cp system/blacklist-liquidio.conf /etc/modprobe.d/
sudo rmmod liquidio 2>/dev/null

# 2) stop NetworkManager from flushing the oct* IPs
sudo cp system/99-octnic-unmanaged.conf /etc/NetworkManager/conf.d/
sudo systemctl reload NetworkManager
```

After `systemctl start cavium-nic` you get `oct0` and `oct1` as ordinary 10 GbE interfaces.

## Manual bring-up

```bash
sudo ./octboot                       # boot the card (waits for heartbeat)
sudo modprobe octnic ports=2         # oct0 + oct1
sudo ip addr add 10.9.9.1/24 dev oct0
sudo ip link set oct0 mtu 9000 up
# ... same for oct1, or bridge them, or hand to your app
```

`octnic` parameters (all optional; sensible defaults):

| param | default | meaning |
|---|---|---|
| `ports` | `1` | number of host netdevs (`1`=oct0, `2`=oct0+oct1) |
| `base` | `0` | BAR2 phys; `0` = auto-discover via PCI `177d:0092` |
| `dma` | `0` | `1` = enable DMA RX path |
| `hrx` | `0` | host-RAM RX descriptors (DPI writes an 8-byte header) |
| `rxthreads` | `1` | parallel RX drain threads (1/2/4/8) |
| `ntxq` | `1` | TX queues (multi-core xmit) |
| `poll_us` | `200` | RX poll interval |
| `rxbatch` | `1` | deliver each drain batch via `netif_receive_skb_list` |
| `lockfree` | `0` | read the per-slot phase bit instead of the shared index (match card `lockfree=1`) |
| `ztx` | `0` | zero-copy TX via inbound DPI — read-latency-bound, loses to PIO fill; off |
| `p_base_mw` | `18000` | power model baseline, mW (see [below](#card-temperature-and-power)) |
| `p_gbps_mw` | `700` | power model traffic term, mW per Gbit/s |

The autostart uses `ports=2 dma=1 hrx=1 rxthreads=8 ntxq=8 poll_us=20`.

## Benchmarking

Both ports are ordinary netdevs, so `iperf3` over the DAC to whatever the card is cabled to
is the whole method. `scripts/nic-up.sh` puts `10.9.9.1/24` on `oct0` and `10.9.10.1/24` on
`oct1` (override with `IP0=` / `IP1=`); give the peer an address in the same subnet and:

```bash
# on the peer
iperf3 -s
# on this host, per port
sudo iperf3 -c <peer-ip> -B 10.9.9.1 -P8 -t10        # add -R for the reverse direction
```

MTU is set to 9000 on `oct0`/`oct1`; set the same on the peer. Benchmark on a **freshly
booted** card — see [PERFORMANCE](PERFORMANCE.md) for the numbers and the caveats.

## Card temperature and power

The card feeds its board + die temperature to the host over the BAR2 control page;
the baked `rc.local` daemon feeds it, and `octnic` exposes it as hwmon, so plain `sensors`
shows it (no serial cable involved).

```
cavium_card-pci-0200
board:            +36.4 C
octeon-die:       +44.9 C
card (estimate):   19.50 W
```

**`power1` is a model, not a measurement.** The card has no power sensor: it is PCIe
bus-powered and its two i2c buses carry only the tmp421, the SFP/TLV EEPROMs and a pca9554
(see `snic10e.dts`). `octnic` estimates the draw as `baseline + per-Gbit/s of traffic` from
the `oct0`/`oct1` counters, and labels the channel `card (estimate)`. Calibrate the two
coefficients against a wall meter (writable at runtime, mW):

```bash
echo 18000 | sudo tee /sys/module/octnic/parameters/p_base_mw   # card as installed, idle
echo   700 | sudo tee /sys/module/octnic/parameters/p_gbps_mw   # extra per Gbit/s TX+RX
```

`hostmod/power-model-check.py` re-runs the same integer expression as the driver, so a
units or overflow slip fails there instead of in `sensors` (`python3 hostmod/power-model-check.py`).

Method: read the wall meter with the host idle, card removed vs. installed (modules
plugged as you run them) → `p_base_mw`; then run `iperf3` at a known rate and divide the
increase by the Gbit/s → `p_gbps_mw`.

There is **no per-port term**: the host `oct0`/`oct1` netdevs are always admin-up and the
card does not publish its real `xaui0`/`xaui1` carrier over the ctrl page, so a per-link term
would bill ports that have no cable. Module and PHY power is part of `p_base_mw`.

## Uninstall

The card OS lives in RAM, so removing the host side is the whole rollback (the card's u-boot
env is the one permanent change — see the end of this section):

```bash
sudo systemctl disable --now cavium-nic
sudo rm -f /etc/systemd/system/cavium-nic.service && sudo systemctl daemon-reload
sudo rmmod octnic
sudo dkms remove -m octnic -v "$(awk -F'"' '/^PACKAGE_VERSION=/{print $2}' hostmod/dkms.conf)" --all
sudo rm -f /lib/modules/$(uname -r)/extra/octnic.ko && sudo depmod -a   # non-dkms install
sudo rm -f /etc/modprobe.d/blacklist-liquidio.conf \
           /etc/NetworkManager/conf.d/99-octnic-unmanaged.conf
```

The card keeps only the one-time u-boot env; `scripts/restore-bootapp.sh` puts it back on its
stock OEM autoboot (serial, see [FLASHING](FLASHING.md#3-one-time-u-boot-provisioning-serial-once--scriptscard-prep-hostbootsh)).
Left as-is, the card sits idle until `octboot` pushes an image again.

## Troubleshooting

- **`octnic: bad magic 0xffffffff`** — the card isn't up yet (still booting) or BAR2 isn't
  enabled. Wait for `octboot`'s heartbeat, or re-run it.
- **`oct1` has no IPv4 after autostart** — NetworkManager grabbed it; install
  `99-octnic-unmanaged.conf` (above). `scripts/nic-up.sh` also sets `nmcli device set oct1
  managed no`.
- **One port stops receiving (TX still fine) after heavy load** — the RX ring desynced
  under drop pressure. First-line recovery, no card reboot needed: `sudo rmmod octnic &&
  sudo modprobe octnic ports=2` and re-add the IPs — the card disarms on unload and re-arms
  with the new pools — full ring resync, RX back to rate.
- **Host hard-freeze under heavy load** — the card can *wedge* under sustained traffic, and a
  synchronous BAR read to a wedged card stalls the CPU and freezes the host. Recover with a
  host reboot. To park a suspect card without rebooting: `setpci -s <BDF> COMMAND=0000` then
  `echo 1 > /sys/bus/pci/devices/0000:<BDF>/remove` (bring back with
  `echo 1 > /sys/bus/pci/rescan`). Don't leave `octnic` loaded on an idle/wedged card.
- **Card won't boot after a wedge** — soft resets don't clear card RAM; only a full host
  reboot power-cycles the card.
- **Fresh boot before each benchmark** — a card that has been hammered gives degraded/zero
  throughput until re-booted with `octboot`.
