// SPDX-License-Identifier: GPL-2.0
/* card_sim.c — Linux userspace daemon that simulates the Cavium CN6640 card.
 *
 * Maps a file-backed shared memory region at a configurable address.
 * The Windows driver (when run under WINE or a simulator) or the octboot.c
 * tool can access this region as if it were PCIe BAR2.
 *
 * This is used to:
 * 1. Test octboot.c image push + handshake flow
 * 2. Provide a "live card" for driver integration testing
 * 3. Simulate RX frames from the card side
 *
 * Usage: sudo ./card_sim <mem_region_file> [port_count]
 * The shared memory layout matches linuxmod/octnic.c exactly.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <signal.h>
#include <time.h>

/* ---- Constants (match winmod/octnic.h and linuxmod/octnic.c) ---- */

#define OCTSHM_MAGIC              0x4f435348u   /* "OCSH" */
#define OCTSHM_VERSION            2u
#define RING_SZ                   128u
#define RING_MASK                 (RING_SZ - 1)
#define BUF_SZ                    9216u
#define CTRL_OFF                  0x000000u
#define TXDESC_OFF                0x001000u
#define RXDESC_OFF                0x002000u
#define TXBUF_OFF                 0x100000u
#define RXBUF_OFF                 0x280000u
#define PORT_STRIDE               0x400000u     /* 4MB per port */
#define MAX_PORTS                 2u

/* Descriptor struct (must match winmod/octnic.h) */
typedef struct {
    uint32_t len;
    uint32_t flags;
} shm_desc_t;

/* Control page field offsets */
#define C_MAGIC                   0x00u
#define C_VERSION                 0x04u
#define C_CARD_READY              0x08u
#define C_HOST_READY              0x0cu
#define C_TX_PROD                 0x10u
#define C_TX_CONS                 0x14u
#define C_RX_PROD                 0x18u
#define C_RX_CONS                 0x1cu
#define C_HEARTBEAT               0x20u
#define C_DMA_ENABLE              0x24u
#define C_TX_DMA_BASE             0x28u
#define C_RX_DMA_BASE             0x30u
#define C_RESV0                   0x38u       /* card board temp */
#define C_RESV1                   0x3cu       /* card Octeon die temp */
#define C_LINK                    0x40u

/* Handshake offset (used by octboot) */
#define HANDSHAKE_OFF             0x100u

/* Total region size: MAX_PORTS * PORT_STRIDE */
#define REGION_SIZE               (PORT_STRIDE * MAX_PORTS)

/* ---- Global state ---- */

static volatile uint8_t *g_region = NULL;
static int g_running = 1;
static int g_card_ready = 0;
static uint32_t g_heartbeat = 0;
static uint64_t g_hb_start = 0;

/* Per-port state */
typedef struct {
    volatile uint32_t *ctrl;     /* pointer to ctrl page */
    volatile uint32_t tx_prod;
    volatile uint32_t tx_cons;
    volatile uint32_t rx_prod;
    volatile uint32_t rx_cons;
    /* TX descriptor ring */
    shm_desc_t tx_desc[RING_SZ];
    /* RX descriptor ring */
    shm_desc_t rx_desc[RING_SZ];
} port_state_t;

static port_state_t g_ports[MAX_PORTS];
static int g_num_ports = 1;

/* ---- Signal handling ---- */

static void sig_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ---- Initialize shared memory ---- */

static void init_region(volatile uint8_t *base, int num_ports)
{
    int i;

    printf("[card_sim] Initializing %d port region (%zu bytes)\n",
           num_ports, (size_t)PORT_STRIDE * num_ports);

    for (i = 0; i < num_ports; i++) {
        volatile uint32_t *ctrl = (volatile uint32_t *)(base + i * PORT_STRIDE);
        port_state_t *p = &g_ports[i];

        /* Control page */
        ctrl[C_MAGIC / 4]      = OCTSHM_MAGIC;
        ctrl[C_VERSION / 4]    = OCTSHM_VERSION;
        ctrl[C_CARD_READY / 4] = 0;       /* starts not ready */
        ctrl[C_HOST_READY / 4] = 0;
        ctrl[C_TX_PROD / 4]    = 0;
        ctrl[C_TX_CONS / 4]    = 0;
        ctrl[C_RX_PROD / 4]    = 0;
        ctrl[C_RX_CONS / 4]    = 0;
        ctrl[C_HEARTBEAT / 4]  = 0;
        ctrl[C_DMA_ENABLE / 4] = 0;
        ctrl[C_TX_DMA_BASE / 4] = 0;
        ctrl[C_RX_DMA_BASE / 4] = 0;
        ctrl[C_RESV0 / 4]      = 85000;   /* 85C board temp */
        ctrl[C_RESV1 / 4]      = 75000;   /* 75C die temp */
        ctrl[C_LINK / 4]       = (1u << 31) | 1;  /* link published + up */

        p->ctrl = ctrl;
        p->tx_prod = 0;
        p->tx_cons = 0;
        p->rx_prod = 0;
        p->rx_cons = 0;

        /* Zero descriptor rings */
        memset(p->tx_desc, 0, sizeof(p->tx_desc));
        memset(p->rx_desc, 0, sizeof(p->rx_desc));

        printf("[card_sim] Port %d: ctrl at offset 0x%06x\n", i, i * PORT_STRIDE);
    }
}

/* ---- TX processing (card reads from host) ---- */

static void process_tx(int port_idx)
{
    port_state_t *p = &g_ports[port_idx];
    volatile uint32_t *ctrl = p->ctrl;
    uint32_t prod, cons;

    prod = (uint32_t)ctrl[C_TX_PROD / 4];
    cons = p->tx_cons;

    while ((int)(prod - cons) > 0 && (int)(prod - cons) < (int)RING_SZ) {
        uint32_t slot = cons & RING_MASK;
        shm_desc_t *desc = &p->tx_desc[slot];

        /* Check phase bit */
        if ((desc->flags & 1u) != (cons >> 7 & 1u)) break;

        if (desc->len > 0 && desc->len <= BUF_SZ) {
            /* Card "received" a frame from host.
             * Echo it back on RX ring for testing. */
            uint32_t rx_slot = p->rx_prod & RING_MASK;
            shm_desc_t *rx_desc = &p->rx_desc[rx_slot];

            /* Copy frame data to RX buffer area */
            uint8_t *src = (uint8_t *)(g_region + port_idx * PORT_STRIDE + TXBUF_OFF + slot * BUF_SZ);
            uint8_t *dst = (uint8_t *)(g_region + port_idx * PORT_STRIDE + RXBUF_OFF + rx_slot * BUF_SZ);

            rx_desc->len = desc->len;
            rx_desc->flags = (p->rx_prod >> 7 & 1u) | 1u;  /* phase | valid */

            if (desc->len > 0) {
                memcpy(dst, src, desc->len);
            }

            p->rx_prod++;
            ctrl[C_RX_PROD / 4] = p->rx_prod;

            printf("[card_sim] Port %d: TX frame %u bytes -> echoed to RX slot %u\n",
                   port_idx, desc->len, rx_slot);
        }

        /* Advance consumer */
        memset(desc, 0, sizeof(*desc));
        cons++;
        p->tx_cons = cons;
        ctrl[C_TX_CONS / 4] = cons;
    }
}

/* ---- Simulate RX from card side (generate ARP/ICMP test frames) ---- */

static void generate_rx_frame(int port_idx)
{
    port_state_t *p = &g_ports[port_idx];
    volatile uint32_t *ctrl = p->ctrl;
    uint32_t slot = p->rx_prod & RING_MASK;
    shm_desc_t *desc = &p->rx_desc[slot];
    uint8_t *buf = (uint8_t *)(g_region + port_idx * PORT_STRIDE + RXBUF_OFF + slot * BUF_SZ);

    /* Generate a simple ARP request frame */
    uint8_t arp_frame[60] = {
        /* Destination MAC (broadcast) */
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        /* Source MAC (card's simulated MAC) */
        0x02, 0xc4, 0x00, 0x01, 0x02, 0x03,
        /* EtherType: ARP */
        0x08, 0x06,
        /* ARP payload (simplified) */
        0x00, 0x01,  /* HW type: Ethernet */
        0x08, 0x00,  /* Proto type: IPv4 */
        0x06,        /* HW size */
        0x04,        /* Proto size */
        0x00, 0x01,  /* Opcode: request */
        /* Sender MAC */
        0x02, 0xc4, 0x00, 0x01, 0x02, 0x03,
        /* Sender IP */
        0x0a, 0x00, 0x00, 0x01,  /* 10.0.0.1 */
        /* Target MAC */
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        /* Target IP */
        0x0a, 0x00, 0x00, 0x02,  /* 10.0.0.2 */
    };

    desc->len = sizeof(arp_frame);
    desc->flags = (p->rx_prod >> 7 & 1u) | 1u;  /* phase | valid */

    memcpy(buf, arp_frame, sizeof(arp_frame));

    p->rx_prod++;
    ctrl[C_RX_PROD / 4] = p->rx_prod;

    printf("[card_sim] Port %d: generated ARP request (%u bytes) -> RX slot %u\n",
           port_idx, desc->len, slot);
}

/* ---- Heartbeat update ---- */

static void update_heartbeat(void)
{
    if (g_hb_start == 0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        g_hb_start = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    }

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    g_heartbeat = (uint32_t)(now - g_hb_start);

    for (int i = 0; i < g_num_ports; i++) {
        g_ports[i].ctrl[C_HEARTBEAT / 4] = g_heartbeat;
    }
}

/* ---- Main loop ---- */

int main(int argc, char *argv[])
{
    const char *mem_file = NULL;
    int fd, i;
    struct stat st;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <mem_region_file> [num_ports]\n", argv[0]);
        fprintf(stderr, "\nCreates a shared memory region simulating the CN6640 card.\n");
        fprintf(stderr, "The Windows driver or octboot.c can access this as BAR2.\n");
        return 1;
    }

    mem_file = argv[1];
    if (argc >= 3) g_num_ports = atoi(argv[2]);
    if (g_num_ports < 1 || g_num_ports > MAX_PORTS) {
        fprintf(stderr, "Port count must be 1-%d\n", MAX_PORTS);
        return 1;
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    /* Open or create the memory region file */
    fd = open(mem_file, O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    /* Get current size */
    if (fstat(fd, &st) < 0 || st.st_size == 0) {
        /* Initialize new region */
        if (ftruncate(fd, REGION_SIZE) < 0) {
            perror("ftruncate");
            close(fd);
            return 1;
        }
        init_region(NULL, g_num_ports);  /* just validate params */
    }

    /* Map the region */
    g_region = mmap(NULL, REGION_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (g_region == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    printf("[card_sim] Shared memory mapped at %p (%u bytes)\n",
           g_region, REGION_SIZE);
    printf("[card_sim] File: %s\n", mem_file);
    printf("[card_sim] Ports: %d\n", g_num_ports);

    /* Initialize if this is a fresh region */
    if (*(volatile uint32_t *)(g_region + C_MAGIC) != OCTSHM_MAGIC) {
        init_region(g_region, g_num_ports);
        printf("[card_sim] Card memory initialized\n");
    } else {
        printf("[card_sim] Existing card memory found\n");
        /* Restore port state pointers */
        for (i = 0; i < g_num_ports; i++) {
            g_ports[i].ctrl = (volatile uint32_t *)(g_region + i * PORT_STRIDE);
        }
    }

    printf("[card_sim] Card SIM running. Press Ctrl+C to stop.\n");
    printf("[card_sim] BAR2 layout:\n");
    printf("  CTRL page:     0x%06x (magic=0x%08x)\n", CTRL_OFF, OCTSHM_MAGIC);
    printf("  TX desc ring:  0x%06x\n", TXDESC_OFF);
    printf("  RX desc ring:  0x%06x\n", RXDESC_OFF);
    printf("  TX buffer:     0x%06x\n", TXBUF_OFF);
    printf("  RX buffer:     0x%06x\n", RXBUF_OFF);

    /* Main loop */
    int tick = 0;
    while (g_running) {
        update_heartbeat();

        /* Process any TX frames from host */
        for (i = 0; i < g_num_ports; i++) {
            process_tx(i);
        }

        /* Generate RX frames every ~5 seconds (simulates card network activity) */
        if (++tick % 500 == 0) {
            for (i = 0; i < g_num_ports; i++) {
                generate_rx_frame(i);
            }
        }

        /* Simulate host setting CARD_READY after handshake */
        if (!g_card_ready && *(volatile uint32_t *)(g_region + HANDSHAKE_OFF) == OCTSHM_MAGIC) {
            printf("[card_sim] Handshake detected! Setting card ready.\n");
            g_card_ready = 1;
            for (i = 0; i < g_num_ports; i++) {
                g_ports[i].ctrl[C_CARD_READY / 4] = 1;
            }
        }

        usleep(2000);  /* 2ms tick */
    }

    printf("[card_sim] Stopping...\n");
    munmap((void *)g_region, REGION_SIZE);
    return 0;
}
