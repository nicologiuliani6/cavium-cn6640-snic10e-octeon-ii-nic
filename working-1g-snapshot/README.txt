WORKING 1.09 Gbit/s RX-DMA hybrid NIC snapshot (2026-07-01)
- card: octshm_card.ko dma=1, host_bar defaults f8000000/f4000000
- host: octshm_host.ko dma=1, base=0xf4000000
- Cavium BDF 02:00.0 (bridge 00:01.1), BAR2=f4000000
- perf: TCP fwd 1.09 Gbit/s, rev 858 Mbit/s (Cavium port1<->port2 DAC loopback)
Restore: sudo bash restore-1g.sh  (card must be at OpenWrt shell @115200 ttyUSB0)
