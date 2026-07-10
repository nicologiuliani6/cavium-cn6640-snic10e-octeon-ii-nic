# Usage

## Hands-off (systemd)

`system/cavium-nic.service` runs `cavium-up.sh` at boot: it boots the card with `octboot`,
loads `octnic ports=2`, and wires up both ports (`twocard-up.sh`).

The one-shot installer does all of the below (module build+install, the two host configs,
and the service) in one go:

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
sudo ip addr add 10.0.0.1/24 dev oct0
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

The autostart uses `ports=2 dma=1 hrx=1 rxthreads=8 ntxq=8 poll_us=20`.

## Test rig (netns)

To benchmark on a single host where the peer NIC (HP NC523) is in the **same** machine, put
each peer port in its own network namespace — otherwise the kernel short-circuits the two
local IPs in RAM and never touches the card. `twocard-up.sh` does this automatically:

```
oct0 (default ns) 10.9.9.1   <->  card xaui0 <->DAC<-> NC523 f1  (ns nc,  10.9.9.2)
oct1 (default ns) 10.9.10.1  <->  card xaui1 <->DAC<-> NC523 f0  (ns nc2, 10.9.10.2)
```

```bash
# iperf3, port 0
sudo ip netns exec nc  iperf3 -s -B 10.9.9.2 &
sudo iperf3 -c 10.9.9.2 -B 10.9.9.1 -P8 -t10        # add -R for reverse
# port 1
sudo ip netns exec nc2 iperf3 -s -B 10.9.10.2 &
sudo iperf3 -c 10.9.10.2 -B 10.9.10.1 -P8 -t10
```

With a real external peer (switch / another machine) you don't need namespaces — just
assign IPs to `oct0`/`oct1`.

## Card temperature

The card feeds its board + die temperature to the host over the BAR2 control page;
`card-temp.sh` and the baked `rc.local` daemon expose it. Read it host-side via the
`octnic` hwmon (`sensors` shows `cavium_card`).

## Troubleshooting

- **`octnic: bad magic 0xffffffff`** — the card isn't up yet (still booting) or BAR2 isn't
  enabled. Wait for `octboot`'s heartbeat, or re-run it.
- **`oct1` has no IPv4 after autostart** — NetworkManager grabbed it; install
  `99-octnic-unmanaged.conf` (above). `twocard-up.sh` also sets `nmcli device set oct1
  managed no`.
- **One port stops receiving (TX still fine) after heavy load** — the RX ring desynced
  under drop pressure. First-line recovery, no card reboot needed: `sudo rmmod octnic &&
  sudo modprobe octnic ports=2` and re-add the IPs — the card disarms on unload and re-arms
  with the new pools (validated: full ring resync, RX back to line rate).
- **Host hard-freeze under heavy load** — this OEM card can *wedge* under sustained
  traffic; a synchronous BAR read to a wedged card stalls the CPU and freezes the host. It
  is a defect of this (second-hand) board, not the driver. Recover with a host reboot. To
  park a suspect card without rebooting: `setpci -s <BDF> COMMAND=0000` then
  `echo 1 > /sys/bus/pci/devices/0000:<BDF>/remove` (bring back with
  `echo 1 > /sys/bus/pci/rescan`). Don't leave `octnic` loaded on an idle/wedged card.
- **Card won't boot after a wedge** — soft resets don't clear card RAM; a full host reboot
  power-cycles the card. See `restore-1g.sh` for a known-good fallback config.
- **Fresh boot before each benchmark** — a card that has been hammered gives degraded/zero
  throughput until re-booted with `octboot`.
