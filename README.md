# cavium-cn6640-snic10e-octeon-ii-nic
Out-of-tree Linux driver stack and boot tooling for the Cavium CN6640-SNIC10E (Octeon II, PCI 177d:0092), exposing the card as two independent 10 GbE interfaces (oct0/oct1) over a reverse-engineered PCIe BAR2 shared-memory datapath. Host module (octnic), on-card OpenWrt modules (octshm_card/octcarrier), serial-free host bootloader (octboot).
