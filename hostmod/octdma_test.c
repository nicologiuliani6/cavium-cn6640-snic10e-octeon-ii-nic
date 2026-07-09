// SPDX-License-Identifier: GPL-2.0
/* octdma_test: host side of the first DMA proof. Allocates a coherent buffer
 * bound to the Cavium EP (04:00.0), enables bus-master, and prints the bus
 * (IOVA) address. Pass that address to the card module (test_busaddr=) so the
 * card writes a magic value into it via its outbound PCIe path. A timer here
 * watches the buffer and reports when the card's write lands. With IOMMU on,
 * any mis-addressed card write faults (dmesg DMAR) instead of corrupting RAM. */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/dma-mapping.h>
#include <linux/timer.h>

#define SENTINEL 0x0ull
#define INMAGIC  0xC0FFEEC0FFEEC0FFull	/* host->card INBOUND read source */
#define SIZE     4096

static struct pci_dev *pdev;
static void *buf;
static dma_addr_t dma_handle;
static struct timer_list t;
static int seen;

static void watch(struct timer_list *tp)
{
	int i;

	for (i = 0; i < SIZE / 8 && !seen; i++) {	/* scan WHOLE buffer, not just [0] */
		u64 v = *((volatile u64 *)buf + i);
		if (v != INMAGIC && v != SENTINEL) {	/* any change from init magic = card wrote */
			seen = 1;
			pr_info("octdma_test: CARD WROTE 0x%016llx at off %d (bus=0x%llx)\n",
				(unsigned long long)v, i * 8,
				(unsigned long long)dma_handle);
		}
	}
	mod_timer(&t, jiffies + msecs_to_jiffies(200));
}

static int __init m_init(void)
{
	pdev = pci_get_device(0x177d, 0x0092, NULL);
	if (!pdev) { pr_err("octdma_test: EP 177d:0092 not found\n"); return -ENODEV; }
	if (dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64))) {
		pr_err("octdma_test: no 64-bit DMA\n");
		pci_dev_put(pdev); return -EIO;
	}
	buf = dma_alloc_coherent(&pdev->dev, SIZE, &dma_handle, GFP_KERNEL);
	if (!buf) { pci_dev_put(pdev); return -ENOMEM; }
	/* INBOUND test: fill with a read-source magic. OUTBOUND test: watch() must
	 * compare against this (not 0), since DPI writes DPI_MAGIC over it. */
	{ int j; for (j = 0; j < 64; j += 8) *(volatile u64 *)((u8 *)buf + j) = INMAGIC; }
	pci_set_master(pdev);				/* enable bus-master (IOMMU-contained) */
	pr_info("octdma_test: buf bus(IOVA)=0x%llx virt=%p size=%d master ON. "
		"insmod card test_busaddr=0x%llx\n",
		(unsigned long long)dma_handle, buf, SIZE,
		(unsigned long long)dma_handle);
	timer_setup(&t, watch, 0);
	mod_timer(&t, jiffies + msecs_to_jiffies(200));
	return 0;
}

static void __exit m_exit(void)
{
	del_timer_sync(&t);
	pci_clear_master(pdev);
	dma_free_coherent(&pdev->dev, SIZE, buf, dma_handle);
	pci_dev_put(pdev);
	pr_info("octdma_test: unloaded\n");
}
module_init(m_init);
module_exit(m_exit);
MODULE_LICENSE("GPL");
