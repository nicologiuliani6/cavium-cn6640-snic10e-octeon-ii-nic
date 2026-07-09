# Piano 10G — mini-liquidio (SLI packet-queue hardware DMA)

## Obiettivo
Host NIC a 10 Gbit/s. Togliere la CPU dalla copia dei pacchetti: usare il motore
hardware SLI packet-output (card→host) e packet-input (host→card), come fa liquidio,
ma con OpenWrt sulla card (niente firmware vendor, che non abbiamo per snic10e).

## Perché può funzionare (fatti verificati)
- Card octeon-ethernet (built-in) enumera porte NPI/PCIe come netdev `npi%d` (cvm_oct_npi_netdev_ops) e fa skb→PKO. => lato-card TX-verso-host quasi gratis.
- Datapath OQ documentato: host allochi descriptor-ring {buffer_ptr, info_ptr}, programmi SLI_OQ_BASE_ADDR64/SIZE/BUFF_INFO_SIZE/CREDIT, card DMAia i pacchetti + incrementa PKTS_SENT, host poll + refill credit.
- Host può R/W qualsiasi CSR card via BAR0 window (octwin, validato).
- SLI->PEM egress funziona (provato: CPU mem-access esce all'host).

## Architettura
```
RX (rete->host, il path che conta per 10G):
  SFP+ xaui0 --RX--> card Linux --bridge--> npi0 (octeon-ethernet)
    --PKO--> NPI port --SLI OQ hardware DMA--> ring RAM host --> netdev host RX
TX (host->rete):
  host netdev TX --> SLI IQ ring --> card PKI/NPI --> bridge --> xaui0 SFP+
```
Contratto host<->card = i registri SLI_OQ/SLI_IQ (indirizzi ring RAM host). Host li
programma via BAR0. Card fa PKO sulla porta NPI mappata a quella coda.

## Fasi

### Fase 0 — recover + baseline  [IN CORSO]
- OpenWrt ricaricato in RAM (~37min). NIC stabile 2.2G ripristinato. Card viva.

### Fase 1 — PROVA egress OQ (card->host DMA hardware) — milestone critica
- Host module (octoq_host): alloc descriptor-ring + N buffer coerenti; scrivi via BAR0
  SLI_OQ_BASE_ADDR64(q)=ring, SLI_OQ_SIZE(q)=N, SLI_OQ_BUFF_INFO_SIZE(q)=bufsz;
  SLI_PKT_OUT_ENB bit q=1; scrivi credit SLI_OQ_PKTS_CREDIT(q)=N; poll SLI_OQ_PKTS_SENT(q).
- Card: inietta 1 pacchetto di test nella coda NPI/OQ (via cvm_oct_npi TX o PKO diretto).
- PASS = PKTS_SENT incrementa E il buffer host contiene i dati. Prova che l'egress
  hardware funziona (ciò che il DPI non faceva). Offsets: SLI_OQ_* base 0x1C00 (+q*offset),
  SLI_PKT_OUT_ENB=0x1010, SLI_OQ_PKTS_SENT/CREDIT per-coda.
- Rischio: NPI non enumerato / porta NPI->OQ mapping. Fallback: PKO diretto a mano.

### Fase 2 — datapath RX completo
- Card: verifica che npi0 esista (se no, abilita interfaccia NPI nel boot/helper).
  Bridge (o forwarding) xaui0 RX -> npi0 TX. Il PKO fa DMA hardware al ring host.
- Host: OQ reader -> netif_rx. Sostituisce la RX poll CPU-copy attuale.
- Test: iperf reverse, misura throughput (atteso >> 2G, verso 10G).

### Fase 3 — datapath TX (host->card, input queue)
- Host: alloc IQ ring; posta istruzioni PKI a SLI_IQ_BASE_ADDR + doorbell.
  Card: npi0 RX -> bridge -> xaui0 TX.
- (Se TX non e' il collo, tieni la PIO attuale.)

### Fase 4 — interruzioni + tuning verso 10G
- Sostituisci poll con MSI-X (SLI_PKT_CNT/TIME_INT). Interrupt coalescing.
- Multi-coda OQ/IQ (piu' core host). Jumbo. NAPI host.
- Misura, elimina colli, punta 10G.

### Fase 5 — 2 porte, mapping 1:1 fisico (ARCHITETTURA FINALE)
- Host vede 2 netdev, ognuno 1:1 con una porta fisica SFP+:
    host oct0  <->  npi0  <->  xaui0  (SFP+ porta 1)
    host oct1  <->  npi1  <->  xaui1  (SFP+ porta 2)
- Forwarding DIRETTO 1:1 per porta (npiX<->xauiX), porte indipendenti, NON bridged
  tra loro. Ogni porta host = una porta fisica, come una NIC 2-porte vera.
- Serve: 2 coppie OQ/IQ (una per porta), 2 netdev host, 2 forwarding path sulla card.
- 10G per porta (target: 10G su ciascuna, 20G aggregato se il PCIe/SLI regge).
- Oggi: 1 sola (oct0). Questo e' lo stato finale dopo che il path a porta singola fa 10G.

## Note iterazione
- Ogni reload OpenWrt via seriale = ~37min (o ~4min se il baud 921600 regge).
  => minimizzare i reload: sviluppare/compilare offline, caricare in batch, testare molto per reload.
- Card e' un endpoint PCIe: NON wedgeare (evitare stati che richiedono reboot host).
- octwin (host) = ispeziona/configura CSR card live senza reboot.

## Stato deliverable
- Fallback stabile: NIC CPU-copy 2.2G fwd / 1.9G rev, restore-1g.sh.
- Se Fase 1 fallisce (egress OQ), 10G non raggiungibile senza firmware snic10e.
```
