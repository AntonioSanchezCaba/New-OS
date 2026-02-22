/*
 * include/drivers/pci.h - PCI bus driver
 */
#ifndef DRIVERS_PCI_H
#define DRIVERS_PCI_H

#include <types.h>

/* PCI config space access ports */
#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

/* PCI header fields (offsets in config space) */
#define PCI_VENDOR_ID       0x00
#define PCI_DEVICE_ID       0x02
#define PCI_COMMAND         0x04
#define PCI_STATUS          0x06
#define PCI_REVISION_ID     0x08
#define PCI_PROG_IF         0x09
#define PCI_SUBCLASS        0x0A
#define PCI_CLASS           0x0B
#define PCI_CACHE_LINE_SIZE 0x0C
#define PCI_LATENCY_TIMER   0x0D
#define PCI_HEADER_TYPE     0x0E
#define PCI_BIST            0x0F
#define PCI_BAR0            0x10
#define PCI_BAR1            0x14
#define PCI_BAR2            0x18
#define PCI_BAR3            0x1C
#define PCI_BAR4            0x20
#define PCI_BAR5            0x24
#define PCI_INTERRUPT_LINE  0x3C
#define PCI_INTERRUPT_PIN   0x3D

/* PCI command register bits */
#define PCI_CMD_IO          0x001
#define PCI_CMD_MEMORY      0x002
#define PCI_CMD_BUS_MASTER  0x004

/* Known vendor/device IDs */
#define PCI_VENDOR_INTEL    0x8086
#define PCI_DEVICE_E1000    0x100E   /* Intel 82540EM (QEMU default) */
#define PCI_DEVICE_E1000_A  0x100F
#define PCI_DEVICE_E1000_B  0x1010

/* PCI device descriptor */
typedef struct {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision;
    uint8_t  irq_line;
    uint32_t bar[6];          /* Base Address Registers */
    bool     bar_is_mmio[6];  /* Is BAR MMIO (vs I/O port)? */
} pci_device_t;

#define PCI_MAX_DEVICES 32

extern pci_device_t pci_devices[PCI_MAX_DEVICES];
extern int pci_device_count;

/* PCI driver API */
void     pci_init(void);
uint32_t pci_config_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_config_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val);
pci_device_t* pci_find_device(uint16_t vendor, uint16_t device);
pci_device_t* pci_find_class(uint8_t class, uint8_t subclass, uint8_t prog_if);
uint32_t pci_read_bar(pci_device_t* dev, int bar_idx);
void     pci_enable_bus_mastering(pci_device_t* dev);
uint64_t pci_bar_address(pci_device_t* dev, int bar_idx);

#endif /* DRIVERS_PCI_H */
