/* SPDX-License-Identifier: GPL-2.0
 * octnic_pci.h — User-mode PCI config access for Windows.
 * Provides setpci-like read/write of PCI config space and BAR mapping
 * without requiring a kernel driver (uses SetupAPI + CreateFile("\\\\.\\PCIx").
 * This is the Windows equivalent of Linux's /sys/bus/pci/devices/.../resourceN
 * and setpci, needed by octboot.ps1 to push the card image.
 */

#pragma once

#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <devguid.h>

/* PCI config space offsets */
#define PCI_VENDOR_ID       0x00
#define PCI_DEVICE_ID       0x02
#define PCI_COMMAND         0x04
#define PCI_BASE_ADDRESS_0  0x10
#define PCI_BASE_ADDRESS_1  0x14
#define PCI_BASE_ADDRESS_2  0x18
#define PCI_BASE_ADDRESS_3  0x1c
#define PCI_BASE_ADDRESS_4  0x20
#define PCI_BASE_ADDRESS_5  0x24

/* PCIe capability offsets */
#define PCI_CAP_ID_EXP      0x10          /* PCI Express capability ID */

/* Windows device path format for Cavium CN6640 */
#define CAVIUM_VENDOR_ID    0x177d
#define CAVIUM_DEVICE_ID    0x0092
#define CAVIUM_CLASS_GUID   "{8c12f5a3-3e4b-4a1c-9f8d-7e6b5a4c3d2e}"

/* BAR2 size — 64 MiB (card DRAM window) */
#define CAVIUM_BAR2_SIZE    (64 * 1024 * 1024)

/* Device instance path buffer size */
#define MAX_DEVICE_PATH     512

/* Return status */
typedef enum {
    PCI_SUCCESS = 0,
    PCI_ERROR_NOT_FOUND,
    PCI_ERROR_ACCESS_DENIED,
    PCI_ERROR_MAP_FAILED,
    PCI_ERROR_READ_FAILED,
    PCI_ERROR_WRITE_FAILED,
    PCI_ERROR_INVALID_HANDLE
} PCI_STATUS;

/* Opaque handle to a PCI device */
typedef struct _PCI_DEVICE_HANDLE {
    HANDLE hDevice;                   /* Handle from CreateFile("\\\\.\\PCIx") */
    ULONG BusNumber;
    ULONG DeviceNumber;
    ULONG FunctionNumber;
    ULONG VendorId;
    ULONG DeviceId;
    ULONG Bar2Base;                   /* Mapped BAR2 base address (user-mode VA) */
    PVOID Bar2Mapping;                /* Memory-mapped BAR2 region */
    SIZE_T Bar2Size;
    WCHAR DevicePath[MAX_DEVICE_PATH];
} PCI_DEVICE_HANDLE, *PPCI_DEVICE_HANDLE;

/* ---- Core functions ---- */

/**
 * Find a Cavium CN6640 device on the PCI bus.
 * Returns PCI_SUCCESS and fills out the handle on success.
 */
PCI_STATUS PciFindCaviumCard(PPCI_DEVICE_HANDLE handle);

/**
 * Open a specific PCI device by its device path string.
 */
PCI_STATUS PciOpenDeviceByPath(PCWSTR devicePath, PPCI_DEVICE_HANDLE handle);

/**
 * Close a PCI device handle and unmap BAR2 if mapped.
 */
VOID PciCloseDevice(PPCI_DEVICE_HANDLE handle);

/**
 * Read a value from PCI config space (1/2/4 bytes).
 */
PCI_STATUS PciReadConfig(PPCI_DEVICE_HANDLE handle, ULONG offset, PULONG value, ULONG bytes);

/**
 * Write a value to PCI config space (1/2/4 bytes).
 */
PCI_STATUS PciWriteConfig(PPCI_DEVICE_HANDLE handle, ULONG offset, ULONG value, ULONG bytes);

/**
 * Map BAR2 of the device into user-mode address space for direct access.
 * This is how octboot.ps1 pushes the image — same as Linux's mmap(resource2).
 */
PCI_STATUS PciMapBar2(PPCI_DEVICE_HANDLE handle);

/**
 * Unmap BAR2.
 */
VOID PciUnmapBar2(PPCI_DEVICE_HANDLE handle);

/**
 * Read/write the mapped BAR2 region directly (user-mode MMIO).
 */
PCI_STATUS PciReadBar2(PPCI_DEVICE_HANDLE handle, ULONG offset, PVOID buffer, ULONG length);
PCI_STATUS PciWriteBar2(PPCI_DEVICE_HANDLE handle, ULONG offset, PVOID buffer, ULONG length);

/**
 * Perform a Secondary Bus Reset (SBR) on the device.
 * Equivalent to Linux's: setpci -s $BRIDGE BRIDGE_CONTROL=40:40; sleep 0.2; setpci -s $BRIDGE BRIDGE_CONTROL=00:40
 */
PCI_STATUS PciSecondaryBusReset(PPCI_DEVICE_HANDLE handle);

/**
 * Read the bridge control register to find the bridge device for SBR.
 */
PCI_STATUS PciGetBridgeControl(PCWSTR bridgePath, PUSHORT pBridgeControl);
PCI_STATUS PciSetBridgeControl(PCWSTR bridgePath, USHORT value);
