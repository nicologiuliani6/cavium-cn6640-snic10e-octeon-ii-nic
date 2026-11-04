/* SPDX-License-Identifier: GPL-2.0
 * octnic_ctl.c â€” control device \\.\octnic: raw BAR2 access for the Windows card booter.
 * Administrators / SYSTEM only (SDDL). Writes are verified and word-fixed like octboot's
 * "wordfix" pass (write-combining can corrupt words on the way to card DRAM).
 */

#include "octnic.h"
#include "octnic_ioctl.h"

static NDIS_HANDLE g_ctlHandle;
static PDEVICE_OBJECT g_ctlDev;

/* Secondary Bus Reset of the card's upstream bridge, then restore the card's config
 * (BAR0..3, COMMAND, MaxPayload 256) exactly like `octboot` does with setpci. */
static ULONG g_sbrDiag[4];
static NTSTATUS OctnicDoSbr(OCT_SBR* r)
{
    LARGE_INTEGER d;
    ULONG bars[4];
    USHORT cmd = 0, bc = 0, dev = 0;
    UCHAR cap = 0, id = 0, guard = 0;
    ULONG i;

    {   /* validate the targets: never reset anything but the bridge directly above our card */
        ULONG ids = 0; UCHAR hdr = 0, sec = 0;
        HalGetBusDataByOffset(PCIConfiguration, r->cardBus, r->cardSlot, &ids, 0x00, 4);
        HalGetBusDataByOffset(PCIConfiguration, r->bridgeBus, r->bridgeSlot, &hdr, 0x0E, 1);
        HalGetBusDataByOffset(PCIConfiguration, r->bridgeBus, r->bridgeSlot, &sec, 0x19, 1);
        g_sbrDiag[0] = ids; g_sbrDiag[1] = hdr; g_sbrDiag[2] = sec; g_sbrDiag[3] = 0;
        if (ids != ((PCI_DEVICE_ID_CN6640 << 16) | PCI_VENDOR_ID_CAVIUM) || (hdr & 0x7F) != 1 || sec != r->cardBus) {
            g_sbrDiag[3] = 1;
            return STATUS_SUCCESS;                /* refused: caller inspects diag in the reply */
        }
    }
    {
        ULONG n1 = HalGetBusDataByOffset(PCIConfiguration, r->cardBus, r->cardSlot, bars, 0x10, sizeof(bars));
        ULONG n2 = HalGetBusDataByOffset(PCIConfiguration, r->cardBus, r->cardSlot, &cmd, 0x04, 2);
        ULONG n3 = HalGetBusDataByOffset(PCIConfiguration, r->bridgeBus, r->bridgeSlot, &bc, 0x3E, 2);
        OctLog("octnic: SBR cfg read card(%lu,%lu)=%lu/%lu bridge(%lu,%lu)=%lu\n", r->cardBus, r->cardSlot, n1, n2, r->bridgeBus, r->bridgeSlot, n3);
        if (n1 != sizeof(bars) || n2 != 2 || n3 != 2) return STATUS_IO_DEVICE_ERROR;
    }
    OctLog("octnic: SBR: BAR0=%08X BAR2=%08X cmd=%04X bridgeCtl=%04X\n", bars[0], bars[2], cmd, bc);

    bc |= 0x40;
    HalSetBusDataByOffset(PCIConfiguration, r->bridgeBus, r->bridgeSlot, &bc, 0x3E, 2);
    d.QuadPart = -2000000LL;  KeDelayExecutionThread(KernelMode, FALSE, &d);    /* 200 ms */
    bc &= (USHORT)~0x40;
    HalSetBusDataByOffset(PCIConfiguration, r->bridgeBus, r->bridgeSlot, &bc, 0x3E, 2);
    d.QuadPart = -20000000LL; KeDelayExecutionThread(KernelMode, FALSE, &d);    /* 2 s settle */

    for (i = 0; i < 4; i++)
        HalSetBusDataByOffset(PCIConfiguration, r->cardBus, r->cardSlot, &bars[i], 0x10 + i * 4, 4);
    cmd |= 0x06;                                                                /* MEM + bus master */
    HalSetBusDataByOffset(PCIConfiguration, r->cardBus, r->cardSlot, &cmd, 0x04, 2);

    /* MaxPayload 256B (SBR resets it to 128B): DevCtl[7:5] = 001 */
    if (HalGetBusDataByOffset(PCIConfiguration, r->cardBus, r->cardSlot, &cap, 0x34, 1) == 1) {
        while (cap >= 0x40 && guard++ < 48) {
            HalGetBusDataByOffset(PCIConfiguration, r->cardBus, r->cardSlot, &id, cap, 1);
            if (id == 0x10) {
                HalGetBusDataByOffset(PCIConfiguration, r->cardBus, r->cardSlot, &dev, cap + 8, 2);
                dev = (USHORT)((dev & ~0x00e0) | 0x0020);
                HalSetBusDataByOffset(PCIConfiguration, r->cardBus, r->cardSlot, &dev, cap + 8, 2);
                break;
            }
            HalGetBusDataByOffset(PCIConfiguration, r->cardBus, r->cardSlot, &cap, cap + 1, 1);
        }
    }
    OctLog("octnic: SBR done, card config restored\n");
    return STATUS_SUCCESS;
}
static NTSTATUS OctnicIoctl(PIO_STACK_LOCATION sp, PVOID buf, PULONG_PTR info)
{
    ULONG ctl = sp->Parameters.DeviceIoControl.IoControlCode;
    ULONG inLen = sp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = sp->Parameters.DeviceIoControl.OutputBufferLength;
    SIZE_T barLen = 0;
    PUCHAR bar;
    NTSTATUS st = STATUS_SUCCESS;

    bar = OctnicCtlAcquire(&barLen);
    if (bar == NULL) return STATUS_DEVICE_NOT_READY;

    switch (ctl) {
    case IOCTL_OCT_STATUS: {
        OCT_STATUS* s = (OCT_STATUS*)buf;
        if (outLen < sizeof(*s)) { st = STATUS_BUFFER_TOO_SMALL; break; }
        RtlZeroMemory(s, sizeof(*s));
        s->ready  = g_Card.ready ? 1 : 0;
        s->nCand  = 1;
        s->candLen = (ULONG)barLen;
        s->w0     = *(volatile ULONG*)(bar + 0);
        s->w8     = *(volatile ULONG*)(bar + 8);
        s->w100   = *(volatile ULONG*)(bar + 0x100);
        s->w10000 = *(volatile ULONG*)(bar + 0x10000);
        s->hb     = (s->w0 == OCTSHM_MAGIC) ? *(volatile ULONG*)(bar + C_HEARTBEAT) : 0;
        s->armed0 = (ULONG)g_Card.ports[0].armed;
        s->armed1 = (ULONG)g_Card.ports[1].armed;
        s->link0  = g_Card.ports[0].adapter ? (ULONG)g_Card.ports[0].adapter->linkUp : 0;
        s->link1  = g_Card.ports[1].adapter ? (ULONG)g_Card.ports[1].adapter->linkUp : 0;
        *info = sizeof(*s);
        break;
    }
    case IOCTL_OCT_WRITE: {
        OCT_XFER* x = (OCT_XFER*)buf;
        PUCHAR data;
        ULONG pass, i, bad = 0;
        if (inLen < sizeof(*x) || outLen < sizeof(ULONG)) { st = STATUS_BUFFER_TOO_SMALL; break; }
        if (x->offset == OCT_OFF_SBR) {
            if (outLen < sizeof(g_sbrDiag)) { st = STATUS_BUFFER_TOO_SMALL; break; }          /* secondary bus reset request (payload = OCT_SBR) */
            if (x->len != sizeof(OCT_SBR)) { st = STATUS_INVALID_BUFFER_SIZE; break; }
            if (inLen < sizeof(*x) + sizeof(OCT_SBR)) { st = STATUS_BUFFER_OVERFLOW; break; }
            OCT_SBR req = *(OCT_SBR*)(x + 1);
            OctnicCtlRelease();                  /* SBR takes seconds: do not pin the BAR meanwhile */
            g_sbrDiag[3] = 0;
            st = OctnicDoSbr(&req);
            RtlCopyMemory(buf, g_sbrDiag, sizeof(g_sbrDiag));   /* [3]=1: refused (target is not our card/bridge) */
            *info = sizeof(g_sbrDiag);
            return st;
        }
        if (x->len == 0 || x->len > OCT_MAX_XFER || inLen < sizeof(*x) + x->len ||
            (x->offset & 3) || (x->len & 3) ||
            (SIZE_T)x->offset + x->len > barLen) { st = STATUS_INVALID_PARAMETER; break; }
        data = (PUCHAR)(x + 1);
        RtlCopyMemory(bar + x->offset, data, x->len);
        _mm_sfence(); _mm_mfence();
        for (pass = 0; pass < 10; pass++) {
            bad = 0;
            for (i = 0; i < x->len; i += 4) {
                if (*(volatile ULONG*)(bar + x->offset + i) != *(ULONG*)(data + i)) {
                    *(volatile ULONG*)(bar + x->offset + i) = *(ULONG*)(data + i);
                    bad++;
                }
            }
            _mm_sfence(); _mm_mfence();
            if (!bad) break;
        }
        *(PULONG)buf = bad;                 /* words still wrong after the last pass */
        *info = sizeof(ULONG);
        break;
    }
    case IOCTL_OCT_READ: {
        OCT_XFER* x = (OCT_XFER*)buf;
        ULONG xl, xo;
        if (inLen < sizeof(*x)) { st = STATUS_BUFFER_TOO_SMALL; break; }
        xl = x->len; xo = x->offset;
        if (xl == 0 || xl > OCT_MAX_XFER || outLen < xl || (xo & 3) || (xl & 3) ||
            (SIZE_T)xo + xl > barLen) { st = STATUS_INVALID_PARAMETER; break; }
        RtlCopyMemory(buf, bar + xo, xl);
        *info = xl;
        break;
    }
    case IOCTL_OCT_SBR:
        OctLog("octnic: IOCTL_SBR in=%lu\n", inLen);
        if (inLen < sizeof(OCT_SBR)) { st = STATUS_BUFFER_TOO_SMALL; break; }
        OctnicCtlRelease();                 /* SBR takes seconds: do not pin the BAR meanwhile */
        return OctnicDoSbr((OCT_SBR*)buf);
    default:
        st = STATUS_INVALID_DEVICE_REQUEST;
    }

    OctnicCtlRelease();
    return st;
}

static NTSTATUS OctnicDispatch(PDEVICE_OBJECT dev, PIRP irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS st = STATUS_SUCCESS;
    ULONG_PTR info = 0;
    UNREFERENCED_PARAMETER(dev);

    if (sp->MajorFunction == IRP_MJ_DEVICE_CONTROL)
        st = OctnicIoctl(sp, irp->AssociatedIrp.SystemBuffer, &info);

    irp->IoStatus.Status = st;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

NTSTATUS OctnicCtlRegister(NDIS_HANDLE driverHandle)
{
    if (g_ctlHandle) return STATUS_SUCCESS;
    NDIS_DEVICE_OBJECT_ATTRIBUTES a;
    PDRIVER_DISPATCH disp[IRP_MJ_MAXIMUM_FUNCTION + 1];
    NDIS_STRING dn = NDIS_STRING_CONST("\\Device\\octnic");
    NDIS_STRING sn = NDIS_STRING_CONST("\\DosDevices\\octnic");
    UNICODE_STRING sddl = RTL_CONSTANT_STRING(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");

    RtlZeroMemory(disp, sizeof(disp));
    disp[IRP_MJ_CREATE] = OctnicDispatch;
    disp[IRP_MJ_CLOSE] = OctnicDispatch;
    disp[IRP_MJ_DEVICE_CONTROL] = OctnicDispatch;

    RtlZeroMemory(&a, sizeof(a));
    a.Header.Type     = NDIS_OBJECT_TYPE_DEVICE_OBJECT_ATTRIBUTES;
    a.Header.Revision = NDIS_DEVICE_OBJECT_ATTRIBUTES_REVISION_1;
    a.Header.Size     = NDIS_SIZEOF_DEVICE_OBJECT_ATTRIBUTES_REVISION_1;
    a.DeviceName      = &dn;
    a.SymbolicName    = &sn;
    a.MajorFunctions  = disp;
    a.ExtensionSize   = 0;
    a.DefaultSDDLString = &sddl;
    return NdisRegisterDeviceEx(driverHandle, &a, &g_ctlDev, &g_ctlHandle);
}

VOID OctnicCtlUnregister(VOID)
{
    if (g_ctlHandle) { NdisDeregisterDeviceEx(g_ctlHandle); g_ctlHandle = NULL; }
}
