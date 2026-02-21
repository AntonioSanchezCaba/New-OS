/*
 * drivers/pci.c - PCI bus enumeration
 *
 * Scans all PCI buses (0-7), slots (0-31), and functions (0-7) using
 * the standard Config Mechanism #1 (I/O ports 0xCF8/0xCFC).
 * Stores found devices in the global pci_devices[] array.
 */
#include <drivers/pci.h>
#include <kernel.h>
#include <string.h>

pci_device_t pci_devices[PCI_MAX_DEVICES];
int          pci_device_count = 0;

/* =========================================================
 * Config space I/O port access
 * ========================================================= */

uint32_t pci_config_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    uint32_t addr = (1u << 31)              /* Enable bit */
                  | ((uint32_t)bus   << 16)
                  | ((uint32_t)slot  << 11)
                  | ((uint32_t)func  <<  8)
                  | (offset & 0xFC);         /* DWord-aligned */
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

void pci_config_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset,
                      uint32_t val)
{
    uint32_t addr = (1u << 31)
                  | ((uint32_t)bus   << 16)
                  | ((uint32_t)slot  << 11)
                  | ((uint32_t)func  <<  8)
                  | (offset & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, val);
}

/* Read 16-bit word from config space */
static uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    uint32_t dword = pci_config_read(bus, slot, func, off & 0xFC);
    return (uint16_t)(dword >> ((off & 2) * 8));
}

/* Read 8-bit byte from config space */
static uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    uint32_t dword = pci_config_read(bus, slot, func, off & 0xFC);
    return (uint8_t)(dword >> ((off & 3) * 8));
}

/* =========================================================
 * BAR decoding
 * ========================================================= */

/*
 * Read and decode one BAR. Returns the decoded base address.
 * Sets *is_mmio to true if MMIO, false if I/O port.
 */
static uint32_t decode_bar(uint8_t bus, uint8_t slot, uint8_t func,
                            int bar_num, bool* is_mmio)
{
    uint8_t off = (uint8_t)(PCI_BAR0 + bar_num * 4);
    uint32_t raw = pci_config_read(bus, slot, func, off);

    if (raw & 0x1) {
        /* I/O BAR */
        *is_mmio = false;
        return raw & 0xFFFFFFFC;
    } else {
        /* Memory BAR */
        *is_mmio = true;
        uint8_t type = (uint8_t)((raw >> 1) & 0x3);
        if (type == 0) {
            /* 32-bit MMIO */
            return raw & 0xFFFFFFF0;
        }
        /* 64-bit MMIO: ignore high 32 bits for now */
        return raw & 0xFFFFFFF0;
    }
}

/* =========================================================
 * Device enumeration
 * ========================================================= */

static void pci_check_function(uint8_t bus, uint8_t slot, uint8_t func)
{
    if (pci_device_count >= PCI_MAX_DEVICES) return;

    uint32_t id = pci_config_read(bus, slot, func, PCI_VENDOR_ID);
    uint16_t vendor = (uint16_t)(id & 0xFFFF);
    uint16_t device = (uint16_t)(id >> 16);

    if (vendor == 0xFFFF) return;  /* No device present */

    pci_device_t* dev = &pci_devices[pci_device_count++];
    memset(dev, 0, sizeof(*dev));

    dev->bus       = bus;
    dev->slot      = slot;
    dev->func      = func;
    dev->vendor_id = vendor;
    dev->device_id = device;

    uint32_t class_dword = pci_config_read(bus, slot, func, PCI_REVISION_ID);
    dev->revision   = (uint8_t)(class_dword & 0xFF);
    dev->prog_if    = (uint8_t)((class_dword >>  8) & 0xFF);
    dev->subclass   = (uint8_t)((class_dword >> 16) & 0xFF);
    dev->class_code = (uint8_t)((class_dword >> 24) & 0xFF);
    dev->irq_line   = pci_read8(bus, slot, func, PCI_INTERRUPT_LINE);

    /* Decode BARs 0-5 */
    for (int i = 0; i < 6; i++) {
        dev->bar[i] = decode_bar(bus, slot, func, i, &dev->bar_is_mmio[i]);
    }

    kinfo("PCI %02x:%02x.%x  vendor=%04x device=%04x class=%02x:%02x irq=%u",
          bus, slot, func, vendor, device,
          dev->class_code, dev->subclass, dev->irq_line);
}

static void pci_check_slot(uint8_t bus, uint8_t slot)
{
    /* Check function 0 first */
    uint32_t id = pci_config_read(bus, slot, 0, PCI_VENDOR_ID);
    if ((id & 0xFFFF) == 0xFFFF) return;  /* No device */

    pci_check_function(bus, slot, 0);

    /* Multi-function device? */
    uint8_t header = pci_read8(bus, slot, 0, PCI_HEADER_TYPE);
    if (header & 0x80) {
        for (uint8_t func = 1; func < 8; func++) {
            uint32_t fid = pci_config_read(bus, slot, func, PCI_VENDOR_ID);
            if ((fid & 0xFFFF) != 0xFFFF)
                pci_check_function(bus, slot, func);
        }
    }
}

void pci_init(void)
{
    pci_device_count = 0;
    kinfo("PCI: scanning bus...");

    for (uint8_t bus = 0; bus < 8; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            pci_check_slot(bus, slot);
        }
    }

    kinfo("PCI: found %d device(s)", pci_device_count);
}

/* =========================================================
 * Device search
 * ========================================================= */

pci_device_t* pci_find_device(uint16_t vendor, uint16_t device)
{
    for (int i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].vendor_id == vendor &&
            pci_devices[i].device_id == device) {
            return &pci_devices[i];
        }
    }
    /* Also try alternate device IDs for e1000 */
    if (vendor == PCI_VENDOR_INTEL && device == PCI_DEVICE_E1000) {
        uint16_t alts[] = { PCI_DEVICE_E1000_A, PCI_DEVICE_E1000_B, 0 };
        for (int j = 0; alts[j]; j++) {
            for (int i = 0; i < pci_device_count; i++) {
                if (pci_devices[i].vendor_id == vendor &&
                    pci_devices[i].device_id == alts[j]) {
                    return &pci_devices[i];
                }
            }
        }
    }
    return NULL;
}

/* =========================================================
 * Bus mastering
 * ========================================================= */

void pci_enable_bus_mastering(pci_device_t* dev)
{
    uint32_t cmd = pci_config_read(dev->bus, dev->slot, dev->func, PCI_COMMAND);
    cmd |= PCI_CMD_BUS_MASTER | PCI_CMD_MEMORY | PCI_CMD_IO;
    pci_config_write(dev->bus, dev->slot, dev->func, PCI_COMMAND, cmd);
}

/* =========================================================
 * BAR physical address helper (64-bit capable)
 * ========================================================= */

uint64_t pci_bar_address(pci_device_t* dev, int bar_idx)
{
    if (bar_idx < 0 || bar_idx >= 6) return 0;
    return (uint64_t)dev->bar[bar_idx];
}
