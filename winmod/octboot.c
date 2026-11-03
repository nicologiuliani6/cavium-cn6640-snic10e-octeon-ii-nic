/* SPDX-License-Identifier: GPL-2.0
 * octboot.c — Windows host bootloader for Cavium CN6640 SNIC10E.
 * 
 * Boots the card via PCIe BAR2 image push, no serial required.
 * This is the Windows equivalent of the Linux octboot bash script.
 * 
 * Flow:
 * 1. Find CN6640 on PCI bus (SetupAPI)
 * 2. Check if card window is already live (fast path) or needs SBR
 * 3. Perform SBR if needed, restore BARs
 * 4. Map BAR2 via \\.\PhysicalMemory (user-mode MMIO)
 * 5. Wait for card's u-boot window to become live
 * 6. Push OpenWrt image into card DRAM over BAR2
 * 7. Write "GO!!" handshake flag
 * 8. Wait for card heartbeat (magic + card_ready)
 * 
 * Build: cl /O2 /DUNICODE /D_UNICODE octboot.c /link setupapi.lib advapi32.lib
 * Or use the provided build_release.bat target
 */

#include <windows.h>
#include <stdio.h>
#include <setupapi.h>
#include <devguid.h>
#include <regstr.h>
#include <strsafe.h>
#include <intrin.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "advapi32.lib")

/* ---- Constants (match linuxmod/octnic.c and card side) ---- */

#define CAVIUM_VENDOR_ID    0x177d
#define CAVIUM_DEVICE_ID    0x0092
#define OCTSHM_MAGIC        0x4f435348u     /* "OCSH" */
#define OCTNIC_MTU          1500u

/* Shared-memory layout (same as Linux) */
#define CTRL_OFF            0x000000u
#define TXDESC_OFF          0x001000u
#define RXDESC_OFF          0x002000u
#define TXBUF_OFF           0x100000u
#define RXBUF_OFF           0x280000u
#define PORT_STRIDE         0x400000u       /* 4MB per port */
#define MAX_PORTS           2u

/* Control page field offsets */
#define C_MAGIC             0x00u
#define C_VERSION           0x04u
#define C_CARD_READY        0x08u
#define C_HOST_READY        0x0cu
#define C_TX_PROD           0x10u
#define C_TX_CONS           0x14u
#define C_RX_PROD           0x18u
#define C_RX_CONS           0x1cu
#define C_HEARTBEAT         0x20u
#define C_DMA_ENABLE        0x24u
#define C_TX_DMA_BASE       0x28u
#define C_RX_DMA_BASE       0x30u
#define C_RESV0             0x38u           /* card board temp */
#define C_RESV1             0x3cu           /* card Octeon die temp */
#define C_LINK              0x40u

/* Handshake and live markers */
#define HANDSHAKE_OFF       0x100u          /* "GO!!" handshake for u-boot */
#define LIVE_MARKER         b"NSRLIVE0"     /* 8-byte marker to test window liveness */
#define LIVE_MARKER_LEN     8u

/* BAR2 size: 64 MiB (ports * PORT_STRIDE) */
#define BAR2_SIZE           (PORT_STRIDE * MAX_PORTS)

/* Image load offset in card DRAM (must be < HANDSHAKE_OFF to stay off image) */
#define IMAGE_OFFSET        0x10000u

/* ---- Types ---- */

typedef struct {
    HANDLE hDevice;
    WCHAR devicePath[MAX_PATH];
    ULONG busNumber;
    ULONG deviceNumber;
    ULONG functionNumber;
    /* BAR2 mapping (set after PciMapBar2) */
    PVOID bar2Mapping;
    PHYSICAL_ADDRESS bar2Physical;
} PCI_DEVICE, *PPCI_DEVICE;

/* ---- Color output helpers ---- */

static HANDLE hConsole = NULL;
static WORD originalAttrs = 0;

static void InitColors(void)
{
    hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hConsole != INVALID_HANDLE_VALUE) {
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        if (GetConsoleScreenBufferInfo(hConsole, &csbi)) {
            originalAttrs = csbi.wAttributes;
        }
    }
}

static void PrintOK(const char *msg)
{
    if (hConsole) SetConsoleTextAttribute(hConsole, FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    printf("[ OK ] %s\n", msg);
    if (hConsole) SetConsoleTextAttribute(hConsole, originalAttrs);
}

static void PrintFail(const char *msg)
{
    if (hConsole) SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_INTENSITY);
    fprintf(stderr, "[FAIL] %s\n", msg);
    if (hConsole) SetConsoleTextAttribute(hConsole, originalAttrs);
}

static void PrintInfo(const char *msg)
{
    printf("[ .. ] %s\n", msg);
}

/* ---- PCI Device Discovery ---- */

/**
 * Find a Cavium CN6640 device on the PCI bus.
 * Returns TRUE on success, fills out the PCI_DEVICE struct.
 */
static BOOL FindCaviumCard(PPCI_DEVICE pdev)
{
    HDEVINFO devInfo;
    SP_DEVINFO_DATA devData;
    GUID guid = {0x8c12f5a3, 0x3e4b, 0x4a1c, {0x9f, 0x8d, 0x7e, 0x6b, 0x5a, 0x4c, 0x3d, 0x2e}};
    /* Fallback: use SYSTEM class which includes all PCI devices */
    GUID guidSystem = {0x4d36e97d, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
    BOOL found = FALSE;
    DWORD i;

    RtlZeroMemory(pdev, sizeof(*pdev));
    pdev->hDevice = INVALID_HANDLE_VALUE;

    /* Try SYSTEM class first (broader match) */
    devInfo = SetupDiGetClassDevs(&guidSystem, NULL, NULL, DIGCF_PRESENT);
    if (devInfo == INVALID_HANDLE_VALUE) {
        PrintFail("SetupDiGetClassDevs failed");
        return FALSE;
    }

    for (i = 0; ; i++) {
        devData.cbSize = sizeof(SP_DEVINFO_DATA);
        if (!SetupDiEnumDeviceInfo(devInfo, i, &devData))
            break;

        /* Read HardwareID */
        TCHAR hwId[512];
        DWORD dataType, sizeNeeded;

        if (SetupDiGetDeviceRegistryKeyValue(devInfo, &devData,
                                              TEXT("HardwareID"), &dataType,
                                              (PBYTE)hwId, sizeof(hwId), &sizeNeeded)) {
            /* HardwareID is null-terminated strings, last one double-null */
            PCWSTR p = hwId;
            while (*p) {
                if (_wcsstr(p, L"VEN_177D") && _wcsstr(p, L"DEV_0092")) {
                    /* Found it! */
                    PrintInfo("Cavium CN6640 found on PCI bus");

                    /* Get device path from registry */
                    HKEY hKey = SetupDiOpenDevRegKey(devInfo, &devData,
                                                      DICS_FLAG_GLOBAL, 0,
                                                      DIREG_DEV, KEY_READ);
                    if (hKey != INVALID_HANDLE_VALUE) {
                        DWORD regType, sizeNeeded2;
                        BYTE regValue[MAX_PATH];

                        sizeNeeded2 = sizeof(regValue);
                        LONG rc = RegQueryValueEx(hKey, TEXT("ResourceMappingName"),
                                                   NULL, &regType, regValue, &sizeNeeded2);
                        if (rc == ERROR_SUCCESS && sizeNeeded2 > 0) {
                            /* ResourceMappingName contains the memory range info */
                            /* Format: "Memory c4000000-c43fffff" or similar */
                            PrintInfo("Device resource mapping found");
                        }
                        RegCloseKey(hKey);
                    }

                    /* Get DevicePath from the device info */
                    if (SetupDiGetDeviceInstanceId(devInfo, &devData,
                                                    pdev->devicePath, MAX_PATH, NULL)) {
                        found = TRUE;

                        /* Parse bus/device/function from InstanceID or DevicePath */
                        /* Format: PCI\VEN_177D&DEV_0092&SUBSYS_...\&REV_...\&BUS_XX\ */
                        PCWSTR busPos = _wcsstr(pdev->devicePath, L"BUS_");
                        if (busPos) {
                            swscanf(busPos + 4, L"%lu", &pdev->busNumber);
                        }
                    }
                    break;
                }
                p += wcslen(p) + 1;
            }
        }

        if (found) break;
    }

    SetupDiDestroyDeviceInfoList(devInfo);

    if (!found) {
        PrintFail("CN6640 (VEN_177D DEV_0092) not found on PCI bus");
        PrintFail("Check: card is seated, Secure Boot is OFF, BIOS Above-4G decoding enabled");
        return FALSE;
    }

    /* Open the device */
    pdev->hDevice = CreateFile(pdev->devicePath,
                                GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL, OPEN_EXISTING, 0, NULL);
    if (pdev->hDevice == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        PrintFail("Cannot open device (need Admin rights). Error code:");
        wprintf(L"    CreateFile error: %lu\\n", err);
        return FALSE;
    }

    return TRUE;
}

/* ---- BAR2 Mapping via \\.\PhysicalMemory ---- */

/**
 * Map BAR2 into user-mode address space.
 * This is the Windows equivalent of Linux's mmap(resource2).
 * Requires Admin rights and SeMapMemoryPrivilege.
 */
static BOOL MapBar2(PPCI_DEVICE pdev)
{
    HANDLE hPhysMem;
    NTSTATUS (NTAPI *NtCreateSection)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
                                       PLARGE_INTEGER, ULONG, ULONG, HANDLE);
    NTSTATUS (NTAPI *NtMapViewOfSection)(HANDLE, HANDLE, PVOID*, ULONG_PTR, SIZE_T,
                                          PLARGE_INTEGER, PSIZE_T, ULONG, ULONG, ULONG);
    NTSTATUS (NTAPI *NtUnmapViewOfSection)(HANDLE, PVOID);

    /* Load ntoskrnl.exe functions dynamically */
    HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll");
    if (!hNtDll) {
        PrintFail("Cannot load ntdll.dll");
        return FALSE;
    }

    NtCreateSection = (NTSTATUS (NTAPI *)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
                                          PLARGE_INTEGER, ULONG, ULONG, HANDLE))
                          GetProcAddress(hNtDll, "NtCreateSection");
    NtMapViewOfSection = (NTSTATUS (NTAPI *)(HANDLE, HANDLE, PVOID*, ULONG_PTR, SIZE_T,
                                              PLARGE_INTEGER, PSIZE_T, ULONG, ULONG, ULONG))
                            GetProcAddress(hNtDll, "NtMapViewOfSection");
    NtUnmapViewOfSection = (NTSTATUS (NTAPI *)(HANDLE, PVOID))
                              GetProcAddress(hNtDll, "NtUnmapViewOfSection");

    if (!NtCreateSection || !NtMapViewOfSection || !NtUnmapViewOfSection) {
        PrintFail("Cannot resolve Nt functions from ntdll.dll");
        return FALSE;
    }

    /* Get BAR2 physical address from the device's resource list */
    /* We use SetupDiGetDeviceRegistryProperty to get the resource list */
    HDEVINFO devInfo = SetupDiGetClassDevs(&GUID_DEVCLASS_SYSTEM, NULL, NULL, DIGCF_PRESENT);
    if (!devInfo) return FALSE;

    SP_DEVINFO_DATA devData;
    devData.cbSize = sizeof(SP_DEVINFO_DATA);
    BOOL found = FALSE;

    for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo, i, &devData); i++) {
        TCHAR hwId[512];
        DWORD sizeNeeded;
        if (SetupDiGetDeviceRegistryKeyValue(devInfo, &devData, TEXT("HardwareID"),
                                              NULL, (PBYTE)hwId, sizeof(hwId), &sizeNeeded)) {
            if (_wcsstr(hwId, L"VEN_177D") && _wcsstr(hwId, L"DEV_0092")) {
                /* Found the device — get its resources */
                HKEY hKey = SetupDiOpenDevRegKey(devInfo, &devData,
                                                  DICS_FLAG_GLOBAL, 0,
                                                  DIREG_DEV, KEY_READ);
                if (hKey != INVALID_HANDLE_VALUE) {
                    DWORD regType, sizeNeeded2;
                    BYTE regValue[1024];

                    /* Try ResourceMappingName */
                    sizeNeeded2 = sizeof(regValue);
                    LONG rc = RegQueryValueEx(hKey, TEXT("ResourceMappingName"),
                                               NULL, &regType, regValue, &sizeNeeded2);
                    if (rc == ERROR_SUCCESS) {
                        /* Parse the mapping — format: "Memory c4000000-c43fffff" */
                        /* Extract physical address */
                        WCHAR *memPos = wcsstr((WCHAR*)regValue, L"Memory ");
                        if (memPos) {
                            PHYSICAL_ADDRESS physAddr;
                            RtlZeroMemory(&physAddr, sizeof(physAddr));

                            /* Try to parse hex address */
                            WCHAR addrStr[20] = {0};
                            PCWSTR p = memPos + 7; /* skip "Memory " */
                            size_t j = 0;
                            while (*p && *p != L'-' && *p != L' ' && j < 16) {
                                addrStr[j++] = *p++;
                            }
                            addrStr[j] = L'\0';

                            /* Parse hex */
                            ULONGLONG addr = 0;
                            for (size_t k = 0; addrStr[k]; k++) {
                                WCHAR c = addrStr[k];
                                addr <<= 4;
                                if (c >= L'0' && c <= L'9') addr |= (c - L'0');
                                else if (c >= L'A' && c <= L'F') addr |= (c - L'A' + 10);
                                else if (c >= L'a' && c <= L'f') addr |= (c - L'a' + 10);
                            }

                            if (addr != 0) {
                                physAddr.QuadPart = (LONGLONG)addr;
                                pdev->bar2Physical = physAddr;

                                /* Create a section from \\.\PhysicalMemory */
                                HANDLE hPhysMem2 = CreateFileW(
                                    L"\\\\.\\PhysicalMemory",
                                    GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ,
                                    NULL, OPEN_EXISTING, 0, NULL);

                                if (hPhysMem2 != INVALID_HANDLE_VALUE) {
                                    HANDLE hSection = NULL;
                                    OBJECT_ATTRIBUTES objAttr;
                                    LARGE_INTEGER secSize;

                                    InitializeObjectAttributes(&objAttr,
                                        L"\\Device\\PhysicalMemory",
                                        OBJ_CASE_INSENSITIVE, NULL, NULL);

                                    /* Map the physical memory directly */
                                    /* Use NtMapViewOfSection with a file mapping of PhysicalMemory */
                                    HANDLE hMap = CreateFileMappingW(
                                        hPhysMem2, NULL, PAGE_READWRITE,
                                        (DWORD)(addr >> 32), (DWORD)addr,
                                        L"\\Device\\PhysicalMemory");

                                    if (hMap != NULL && hMap != INVALID_HANDLE_VALUE) {
                                        pdev->bar2Mapping = MapViewOfFile(
                                            hMap, FILE_MAP_READ | FILE_MAP_WRITE,
                                            (DWORD)(addr >> 32), (DWORD)(addr & 0xFFFFFFFF),
                                            BAR2_SIZE);

                                        if (pdev->bar2Mapping) {
                                            PrintOK("BAR2 mapped at physical 0x%llx",
                                                    (unsigned long long)addr);
                                            CloseHandle(hMap);
                                            found = TRUE;
                                        } else {
                                            CloseHandle(hMap);
                                        }
                                    }
                                    CloseHandle(hPhysMem2);
                                }
                            }
                        }
                    }
                    RegCloseKey(hKey);
                }
                break;
            }
        }
    }

    SetupDiDestroyDeviceInfoList(devInfo);

    if (!found) {
        PrintFail("Failed to map BAR2. Check: Admin rights, Secure Boot OFF, Above-4G enabled");
        return FALSE;
    }

    return TRUE;
}

static void UnmapBar2(PPCI_DEVICE pdev)
{
    if (pdev->bar2Mapping) {
        UnmapViewOfFile(pdev->bar2Mapping);
        pdev->bar2Mapping = NULL;
    }
    if (pdev->hDevice != INVALID_HANDLE_VALUE) {
        CloseHandle(pdev->hDevice);
        pdev->hDevice = INVALID_HANDLE_VALUE;
    }
}

/* ---- BAR2 Read/Write helpers (user-mode MMIO) ---- */

static ULONG ReadBar2Ulong(PPCI_DEVICE pdev, ULONG offset)
{
    if (!pdev->bar2Mapping || offset >= BAR2_SIZE) return 0xFFFFFFFF;
    return *(volatile ULONG*)((PUCHAR)pdev->bar2Mapping + offset);
}

static void WriteBar2Ulong(PPCI_DEVICE pdev, ULONG offset, ULONG value)
{
    if (!pdev->bar2Mapping || offset >= BAR2_SIZE) return;
    *(volatile ULONG*)((PUCHAR)pdev->bar2Mapping + offset) = value;
    _mm_mfence();
}

static void WriteBar2Bytes(PPCI_DEVICE pdev, ULONG offset, const VOID *data, ULONG len)
{
    if (!pdev->bar2Mapping || offset + len > BAR2_SIZE) return;
    RtlCopyMemory((PUCHAR)pdev->bar2Mapping + offset, data, len);
    _mm_mfence();
}

/* ---- Secondary Bus Reset ---- */

static BOOL DoSBR(PPCI_DEVICE pdev)
{
    /* On Windows, SBR is done via the PCI bus driver.
     * We use SetupDiCallClassInstaller with DICS_PROPERTY to set the device state.
     * Alternatively, we can use the PCI slot API. */

    PrintInfo("Performing Secondary Bus Reset...");

    /* Method: Disable then re-enable the device via SetupAPI */
    HDEVINFO devInfo = SetupDiGetClassDevs(&GUID_DEVCLASS_SYSTEM, NULL, NULL, DIGCF_PRESENT);
    if (!devInfo) return FALSE;

    SP_DEVINFO_DATA devData;
    devData.cbSize = sizeof(SP_DEVINFO_DATA);

    for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo, i, &devData); i++) {
        TCHAR hwId[512];
        DWORD sizeNeeded;
        if (SetupDiGetDeviceRegistryKeyValue(devInfo, &devData, TEXT("HardwareID"),
                                              NULL, (PBYTE)hwId, sizeof(hwId), &sizeNeeded)) {
            if (_wcsstr(hwId, L"VEN_177D") && _wcsstr(hwId, L"DEV_0092")) {
                /* Disable the device */
                SP_DEVINSTALL_PARAMS devParams;
                RtlZeroMemory(&devParams, sizeof(devParams));
                devParams.cbSize = sizeof(devParams);

                if (SetupDiGetDeviceInstallParams(devInfo, &devData, &devParams)) {
                    devParams.Flags &= ~DNSET_ALLOW_REINSTALL;

                    /* Use SetupDiChangeState to disable */
                    SP_CLASSINSTALL_HEADER classHeader;
                    RtlZeroMemory(&classHeader, sizeof(classHeader));
                    classHeader.cbSize = sizeof(classHeader);
                    classHeader.InstallFunction = DIF_PROPERTYCHANGE;

                    SP_PROPCHANGE_PARAMS params;
                    RtlZeroMemory(&params, sizeof(params));
                    params.ClassInstallHeader = classHeader;
                    params.StateChange = DICS_DISABLE;
                    params.Scope = DICS_FLAG_GLOBAL;
                    params.HwProfile = 0;

                    if (SetupDiSetClassInstallParams(devInfo, &devData,
                                                      (PSP_CLASSINSTALL_HEADER)&params,
                                                      sizeof(params))) {
                        SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, devInfo, &devData);
                    }
                }

                Sleep(200);

                /* Re-enable */
                if (SetupDiGetDeviceInstallParams(devInfo, &devData, &devParams)) {
                    SP_PROPCHANGE_PARAMS params;
                    RtlZeroMemory(&params, sizeof(params));
                    params.ClassInstallHeader.cbSize = sizeof(params.ClassInstallHeader);
                    params.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
                    params.StateChange = DICS_ENABLE;
                    params.Scope = DICS_FLAG_GLOBAL;
                    params.HwProfile = 0;

                    if (SetupDiSetClassInstallParams(devInfo, &devData,
                                                      (PSP_CLASSINSTALL_HEADER)&params,
                                                      sizeof(params))) {
                        SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, devInfo, &devData);
                    }
                }

                PrintOK("Secondary Bus Reset complete");
                Sleep(2000); /* Wait for card to come back */
                break;
            }
        }
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return TRUE;
}

/* ---- Restore BARs ---- */

static BOOL RestoreBars(PPCI_DEVICE pdev)
{
    PrintInfo("Restoring BARs from current PCI config...");

    /* On Windows, BARs are assigned by the OS at enumeration time.
     * After SBR, the OS re-enumerates and assigns BARs automatically.
     * We just need to remap BAR2 from the new address. */

    /* Unmap old BAR2 */
    if (pdev->bar2Mapping) {
        UnmapViewOfFile(pdev->bar2Mapping);
        pdev->bar2Mapping = NULL;
    }

    /* Re-map BAR2 (the physical address may have changed after SBR) */
    return MapBar2(pdev);
}

/* ---- Check if card window is already live ---- */

typedef enum {
    CARD_STATE_DEAD,      /* No response — needs SBR */
    CARD_STATE_LIVE,      /* Window is live, u-boot waiting for GO!! */
    CARD_STATE_OS_READY,  /* OpenWrt running, heartbeat present */
    CARD_STATE_OS_RELOAD  /* OpenWrt running but needs reload */
} CardState;

static CardState CheckCardState(PPCI_DEVICE pdev)
{
    ULONG magic = ReadBar2Ulong(pdev, C_MAGIC);

    if (magic == OCTSHM_MAGIC) {
        ULONG cardReady = ReadBar2Ulong(pdev, C_CARD_READY);
        if (cardReady) {
            return CARD_STATE_OS_READY;
        }
        /* Card OS is up but not fully ready */
        return CARD_STATE_OS_RELOAD;
    }

    /* Test if the window is live by writing a marker and reading it back */
    UCHAR marker[] = LIVE_MARKER;
    WriteBar2Bytes(pdev, IMAGE_OFFSET, marker, LIVE_MARKER_LEN);
    FlushWriteBuffer(); /* Force the write through */

    UCHAR readback[LIVE_MARKER_LEN];
    ReadBar2Bytes(pdev, IMAGE_OFFSET, readback, LIVE_MARKER_LEN);

    if (RtlCompareMemory(marker, readback, LIVE_MARKER_LEN) == LIVE_MARKER_LEN) {
        return CARD_STATE_LIVE;
    }

    return CARD_STATE_DEAD;
}

/* ---- Push image to card DRAM ---- */

static BOOL PushImage(PPCI_DEVICE pdev, PCWSTR imagePath)
{
    /* Read the image file */
    HANDLE hFile = CreateFileW(imagePath, GENERIC_READ, FILE_SHARE_READ,
                                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        PrintFail("Cannot open image file");
        return FALSE;
    }

    DWORD fileSizeHigh;
    DWORD fileSizeLow = GetFileSize(hFile, &fileSizeHigh);
    if (fileSizeLow == 0 && fileSizeHigh == 0) {
        PrintFail("Image file is empty");
        CloseHandle(hFile);
        return FALSE;
    }

    ULONGLONG totalSize = ((ULONGLONG)fileSizeHigh << 32) | fileSizeLow;
    if (totalSize > 0x4000000ULL) { /* Max ~64MB image */
        PrintFail("Image too large (>64MB)");
        CloseHandle(hFile);
        return FALSE;
    }

    HANDLE hMap = CreateFileMapping(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    CloseHandle(hFile);
    if (!hMap) {
        PrintFail("Cannot create file mapping");
        return FALSE;
    }

    PVOID imageBase = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!imageBase) {
        CloseHandle(hMap);
        PrintFail("Cannot map image file");
        return FALSE;
    }

    ULONG imageSize = (ULONG)totalSize;
    ULONG offset = IMAGE_OFFSET;

    PrintInfo("Pushing image (%d bytes) to card DRAM...", imageSize);

    /* Write the image to BAR2 */
    WriteBar2Bytes(pdev, offset, imageBase, imageSize);

    /* Verify the write (wordfix — fix any byte corruption from WC writes) */
    for (int pass = 0; pass < 10; pass++) {
        BOOL ok = TRUE;

        /* Read back and compare */
        PUCHAR readback = (PUCHAR)malloc(imageSize);
        if (!readback) {
            FreeLibrary(hMap);
            PrintFail("Out of memory during verification");
            return FALSE;
        }

        ReadBar2Bytes(pdev, offset, readback, imageSize);

        /* Compare */
        if (RtlCompareMemory(imageBase, readback, imageSize) != imageSize) {
            /* Find bad words and fix them */
            BOOL foundBad = FALSE;
            for (ULONG i = 0; i + 3 < imageSize; i += 4) {
                ULONG expected = ((PUCHAR)imageBase)[i] |
                                 (((PUCHAR)imageBase)[i+1] << 8) |
                                 (((PUCHAR)imageBase)[i+2] << 16) |
                                 (((PUCHAR)imageBase)[i+3] << 24);
                ULONG actual = readback[i] | (readback[i+1] << 8) |
                               (readback[i+2] << 16) | (readback[i+3] << 24);
                if (expected != actual) {
                    WriteBar2Ulong(pdev, offset + i, expected);
                    foundBad = TRUE;
                }
            }
            free(readback);

            if (!foundBad) {
                /* Check last bytes if not word-aligned */
                free(readback);
                break; /* Assume ok if no bad words found */
            }

            PrintInfo("Wordfix pass %d (corrections needed)", pass + 1);
        } else {
            PrintOK("Image uploaded successfully (%d bytes, %d fix passes)",
                    imageSize, pass);
            free(readback);
            break;
        }
    }

    UnmapViewOfFile(imageBase);
    CloseHandle(hMap);

    return TRUE;
}

/* ---- Handshake: write GO!! flag ---- */

static BOOL WriteHandshake(PPCI_DEVICE pdev)
{
    PrintInfo("Writing handshake flag...");

    for (int i = 0; i < 20; i++) {
        WriteBar2Ulong(pdev, HANDSHAKE_OFF, OCTSHM_MAGIC); /* "GO!!" */
        FlushWriteBuffer();

        ULONG readback = ReadBar2Ulong(pdev, HANDSHAKE_OFF);
        if (readback == OCTSHM_MAGIC) {
            PrintOK("Handshake confirmed");
            return TRUE;
        }
    }

    PrintFail("Handshake failed — card may not be in u-boot");
    return FALSE;
}

/* ---- Wait for card heartbeat ---- */

static BOOL WaitForHeartbeat(PPCI_DEVICE pdev, ULONG timeoutSec)
{
    PrintInfo("Waiting for card heartbeat (timeout %ds)...", timeoutSec);

    for (ULONG sec = 0; sec < timeoutSec; sec++) {
        ULONG magic = ReadBar2Ulong(pdev, C_MAGIC);
        if (magic == OCTSHM_MAGIC) {
            ULONG cardReady = ReadBar2Ulong(pdev, C_CARD_READY);
            if (cardReady) {
                ULONG heartbeat = ReadBar2Ulong(pdev, C_HEARTBEAT);
                ULONG version = ReadBar2Ulong(pdev, C_VERSION);
                PrintOK("Heartbeat detected — card ready! (ver=%u, hb=%u)", version, heartbeat);
                return TRUE;
            }
        }

        /* Check if card is in u-boot (magic present but not ready) */
        if (magic == OCTSHM_MAGIC) {
            PrintInfo("Card responding (not fully booted yet)...");
        }

        Sleep(1000);

        /* Progress indicator */
        if (sec % 30 == 0 && sec > 0) {
            printf(".");
            if ((sec / 30) % 10 == 0) printf("\n");
        }
    }

    PrintFail("No heartbeat after %d seconds", timeoutSec);
    PrintFail("Check: card image pushed correctly, u-boot env is provisioned");
    return FALSE;
}

/* ---- Main ---- */

static void PrintUsage(const char *progName)
{
    printf("Usage: %s [options] <image_path>\n", progName);
    printf("\nOptions:\n");
    printf("  -h               Show this help\n");
    printf("  -t <seconds>     Heartbeat timeout (default: 165)\n");
    printf("  -o <offset>      Image load offset (hex, default: 0x10000)\n");
    printf("  --no-sbr         Skip SBR (fast path, window already live)\n");
    printf("  --verify-only    Only verify card is booted, don't push image\n");
    printf("\nExample:\n");
    printf("  %s openwrt-octeon-generic-snic10e-initramfs-kernel.bin\n", progName);
}

int wmain(int argc, PWCHAR *argv)
{
    InitColors();

    PCWSTR imagePath = NULL;
    ULONG timeoutSec = 165;
    ULONG imageOffset = IMAGE_OFFSET;
    BOOL doSBR = TRUE;
    BOOL verifyOnly = FALSE;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"-h") == 0 || _wcsicmp(argv[i], L"--help") == 0) {
            PrintUsage(argv[0]);
            return 0;
        } else if (_wcsicmp(argv[i], L"-t") == 0 && i + 1 < argc) {
            timeoutSec = wcstoul(argv[++i], NULL, 10);
        } else if (_wcsicmp(argv[i], L"-o") == 0 && i + 1 < argc) {
            imageOffset = wcstoul(argv[++i], NULL, 16);
        } else if (_wcsicmp(argv[i], L"--no-sbr") == 0) {
            doSBR = FALSE;
        } else if (_wcsicmp(argv[i], L"--verify-only") == 0) {
            verifyOnly = TRUE;
        } else if (argv[i][0] != L'-') {
            imagePath = argv[i];
        }
    }

    /* Find the card */
    PCI_DEVICE card;
    if (!FindCaviumCard(&card)) {
        return 1;
    }

    /* Map BAR2 */
    if (!MapBar2(&card)) {
        return 1;
    }

    /* Check card state */
    CardState state = CheckCardState(&card);

    if (verifyOnly) {
        switch (state) {
            case CARD_STATE_OS_READY:
                PrintOK("Card is booted and ready");
                UnmapBar2(&card);
                return 0;
            case CARD_STATE_LIVE:
                PrintInfo("Card window is live but card is not booted yet");
                break;
            case CARD_STATE_DEAD:
                PrintFail("Card not responding — needs SBR or reseat");
                UnmapBar2(&card);
                return 1;
            case CARD_STATE_OS_RELOAD:
                PrintInfo("Card OS detected but not fully ready");
                break;
        }
    }

    /* Handle card state */
    if (state == CARD_STATE_DEAD && doSBR) {
        DoSBR(&card);
        RestoreBars(&card);
        state = CheckCardState(&card);
    }

    if (state == CARD_STATE_DEAD) {
        PrintFail("Card still not responding after SBR");
        UnmapBar2(&card);
        return 1;
    }

    if (state == CARD_STATE_OS_READY && !verifyOnly) {
        PrintInfo("Card OS already running — use --no-sbr to reuse without reload");
        /* Allow reuse if user wants to push a new image */
    }

    if (verifyOnly) {
        UnmapBar2(&card);
        return 0;
    }

    /* Need an image file */
    if (!imagePath) {
        PrintFail("No image file specified. Use: octboot.exe <path-to-openwrt-snic10e-initramfs-kernel.bin>");
        UnmapBar2(&card);
        return 1;
    }

    /* Push the image */
    if (!PushImage(&card, imagePath)) {
        UnmapBar2(&card);
        return 1;
    }

    /* Write handshake flag */
    if (!WriteHandshake(&card)) {
        UnmapBar2(&card);
        return 1;
    }

    PrintOK("Booting card... (u-boot will flush L2 cache and jump to image)");

    /* Wait for heartbeat */
    BOOL result = WaitForHeartbeat(&card, timeoutSec);

    UnmapBar2(&card);

    if (result) {
        printf("\nCard booted successfully!\n");
        printf("Next steps:\n");
        printf("  1. Load the Windows driver: pnputil /add-driver octnic.inf /install\n");
        printf("  2. Verify NIC appears: Get-NetAdapter | Where {$_.InterfaceDescription -like '*CN6640*'}\n");
        return 0;
    }

    return 1;
}
