# Usage

> **Status: not verified.** Written from the Linux driver and the FreeBSD port design. Not tested on a FreeBSD host or on hardware. See [README](../README.md).

## Loading the module

```sh
kldload octnic          # loads if_octnic.ko
kldstat | grep octnic   # verify: shows if_octnic among loaded modules
```

The module auto-probes PCI device `177d:0092` (Cavium CN6640 SNIC10E). If the card is not
present or BAR2 cannot be mapped, `kldstat` will show the module but `dmesg` will report
`bad magic 0xffffffff at BAR2`.

## Sysctl Parameters

All parameters are exposed as sysctls under `kern.octnic.*`:

| Sysctl | Default | Meaning |
|---|---|---|
| `kern.octnic.ports` | `1` | Number of card ports (`1`=oct0, `2`=oct0+oct1) |
| `kern.octnic.poll_us` | `200` | RX poll interval in microseconds |
| `kern.octnic.ntxq` | `1` | Number of TX queues (multi-core xmit) |
| `kern.octnic.rxthreads` | `1` | Parallel RX drain threads (power of 2: 1/2/4/8) |
| `kern.octnic.hrx` | `1` | Host-RAM RX descriptors (card DPI writes 8-byte headers) |
| `kern.octnic.lockfree` | `1` | Per-slot phase bit instead of card rx_prod |
| `kern.octnic.ztx` | `0` | Zero-copy TX via inbound DPI |
| `kern.octnic.dma` | `0` | DMA mode (requires IOMMU) |
| `kern.octnic.temp_warn` | `85000` | Temperature warning threshold (millidegC) |
| `kern.octnic.temp_crit` | `100000` | Temperature critical threshold (millidegC) |
| `kern.octnic.hb_timeout` | `10` | Heartbeat freeze timeout (seconds) |
| `kern.octnic.p_base_mw` | `18000` | Power model baseline (mW) |
| `kern.octnic.p_gbps_mw` | `700` | Power model traffic term, mW per Gbit/s |

Change at runtime:

```sh
sysctl kern.octnic.ports=2
sysctl kern.octnic.rxthreads=8
sysctl kern.octnic.hrx=1
```

To list all octnic sysctls:

```sh
sysctl -a | grep kern.octnic
```

## Interface Configuration

After the module is loaded and the card is booted, the interfaces appear as standard FreeBSD netdevs:

```sh
ifconfig oct0 inet 10.9.9.1/24 up
ifconfig oct1 inet 10.9.10.1/24 up

# Jumbo frames (set same MTU on peer)
ifconfig oct0 mtu 9000
ifconfig oct1 mtu 9000
```

Verify link state:

```sh
ifconfig oct0
# Shows LINK STATE, flags, and statistics
```

Carrier follows the real SFP+ link: no cable or peer = `LINK_STATE_DOWN`, no IP shown.

## Card Boot (octboot)

The card's OpenWrt runs from RAM and must be pushed by the host after power-on. The `octboot`
tool is Linux-only today; on FreeBSD the card cannot be booted yet. Linux usage:

```sh
./octboot                    # boot the card, wait for heartbeat
./octboot -Probe             # check card state: osready|os|live|dead
./octboot -Download          # push image only (skip if already up)
```

The rc.d script `rc.d/cavium_nic` automates this: it runs `octboot`, then brings up both interfaces.

Enable at boot:

```sh
sysctl net.cavium_nic_enable=YES   # or add to /etc/rc.conf: cavium_nic_enable="YES"
service cavium_nic start
```

## Benchmarking

Both ports are standard netdevs, so `iperf3` is the method:

```sh
# on the peer
iperf3 -s

# on this host, per port (adjust IP to match your config)
sudo iperf3 -c <peer-ip> -B 10.9.9.1 -P8 -t10     # forward direction
sudo iperf3 -c <peer-ip> -B 10.9.9.1 -P8 -t10 -R  # reverse direction
```

MTU must match on both sides. Set to 9000 for maximum throughput. Benchmark on a freshly
booted card -- a card that has been under heavy load may need a reboot to recover full performance.

## Uninstall

```sh
# Stop the service
service cavium_nic stop

# Unload the module
kldunload octnic

# Remove from /etc/rc.conf if present
sysctl net.cavium_nic_enable=NO   # or edit /etc/rc.conf manually

# Full uninstall via script
sh freebsd/install.sh --uninstall
```

The card OS lives in RAM, so removing the host side is the complete rollback. The card's
u-boot environment remains on flash (one-time provisioning).

## Troubleshooting

- **`bad magic 0xffffffff at BAR2`** -- the card isn't up yet or BAR2 isn't enabled.
  Run `./octboot` and wait for the heartbeat message before loading the module.

- **No link after boot** -- verify the card is provisioned (one-time u-boot env via serial)
  and that a DAC cable connects the SFP+ port to a peer. Run `./octboot -Probe` to check.

- **Low RX throughput** -- increase `kern.octnic.rxthreads` (max 8), ensure `hrx=1` is set,
  and spread poll threads across CPU cores.

- **Host freeze under heavy load** -- the card can wedge under sustained traffic. A synchronous
  BAR read to a wedged card stalls the PCIe bus. Recover with a host reboot. To park a suspect
  card without rebooting: use `pciconf -r <BDF>` to remove the device, then `pciconf -e <BDF>`
  to rescan.

- **`kldload: can't load if_octnic.ko: invalid file format`** -- the module was built for a
  different FreeBSD version. Rebuild with `make -C freebsd/octnic` against the current kernel.
