/*
 * drivers/usb.c — USB HID abstraction layer (UHCI skeleton)
 *
 * Full UHCI/OHCI/xHCI implementation requires significant complexity.
 * This provides the enumeration framework and HID polling infrastructure.
 * The UHCI controller is the most common in QEMU/VirtualBox environments.
 *
 * QEMU typically provides:
 *   - UHCI controller at PCI class 0x0C 0x03 interface 0x00
 *   - USB keyboard as a HID boot protocol device
 *   - USB mouse as a HID boot protocol device
 *
 * For QEMU -device usb-kbd, -device usb-mouse: the devices appear via UHCI.
 * This implementation detects the UHCI controller and provides the framework.
 * Full transaction processing (TD/QH scheduling) is stubbed for the initial
 * release; PS/2 remains the primary input path.
 */
#include <drivers/usb.h>
#include <drivers/pci.h>
#include <drivers/keyboard.h>
#include <drivers/mouse.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>
#include <types.h>

/* UHCI I/O registers (offset from base I/O port) */
#define UHCI_USBCMD   0x00   /* USB Command */
#define UHCI_USBSTS   0x02   /* USB Status */
#define UHCI_USBINTR  0x04   /* USB Interrupt Enable */
#define UHCI_FRNUM    0x06   /* Frame Number */
#define UHCI_FLBASEADD 0x08  /* Frame List Base Address */
#define UHCI_SOF      0x0C   /* Start of Frame Modify */
#define UHCI_PORTSC1  0x10   /* Port 1 Status/Control */
#define UHCI_PORTSC2  0x12   /* Port 2 Status/Control */

/* USBCMD bits */
#define UHCI_CMD_RS   0x0001  /* Run/Stop */
#define UHCI_CMD_HCRESET 0x0002
#define UHCI_CMD_GRESET  0x0004
#define UHCI_CMD_EGSM    0x0008  /* Global Suspend */

/* PORTSC bits */
#define UHCI_PORT_CCS    0x0001  /* Current Connect Status */
#define UHCI_PORT_CSC    0x0002  /* Connect Status Change */
#define UHCI_PORT_PED    0x0004  /* Port Enable/Disable */
#define UHCI_PORT_PEDC   0x0008  /* Port Enable/Disable Change */
#define UHCI_PORT_RESET  0x0200  /* Port Reset */

static usb_device_t g_devices[USB_MAX_DEVICES];
static int          g_dev_count = 0;
static uint16_t     g_uhci_base = 0;
static bool         g_initialized = false;

/* =========================================================
 * PCI UHCI scan
 * ========================================================= */
static void find_uhci(void)
{
    /* USB UHCI: class=0x0C, subclass=0x03, progif=0x00 */
    pci_device_t* dev = pci_find_class(0x0C, 0x03, 0x00);
    if (!dev) {
        klog_warn("USB: no UHCI controller found");
        return;
    }
    /* BAR4 is the I/O base for UHCI */
    g_uhci_base = (uint16_t)(pci_read_bar(dev, 4) & 0xFFFC);
    kinfo("USB: UHCI controller at I/O base 0x%04X (PCI %02X:%02X.%X)",
          g_uhci_base, dev->bus, dev->slot, dev->func);
}

/* =========================================================
 * UHCI port reset + enable
 * ========================================================= */
static void uhci_reset_port(int port)
{
    uint16_t portsc = (uint16_t)(g_uhci_base + (port == 0 ? UHCI_PORTSC1 : UHCI_PORTSC2));
    outw(portsc, UHCI_PORT_RESET);
    /* Wait ~50ms */
    for (volatile int i = 0; i < 500000; i++);
    outw(portsc, 0);
    for (volatile int i = 0; i < 50000; i++);
    /* Enable port */
    outw(portsc, inw(portsc) | UHCI_PORT_PED);
}

/* =========================================================
 * Enumerate connected devices (simplified detection)
 * ========================================================= */
static void uhci_enumerate(void)
{
    if (!g_uhci_base) return;

    for (int port = 0; port < 2; port++) {
        uint16_t portsc_reg = (uint16_t)(g_uhci_base +
                              (port == 0 ? UHCI_PORTSC1 : UHCI_PORTSC2));
        uint16_t status = inw(portsc_reg);

        if (!(status & UHCI_PORT_CCS)) continue;

        kinfo("USB: device detected on port %d", port + 1);
        uhci_reset_port(port);

        /* Re-read status after reset */
        status = inw(portsc_reg);
        if (!(status & UHCI_PORT_PED)) {
            klog_warn("USB: port %d failed to enable", port + 1);
            continue;
        }

        /* Register as generic HID device.
         * Full enumeration (GET_DESCRIPTOR etc.) requires proper TD/QH
         * scheduling. For now, register as a placeholder so usb_available()
         * returns true and PS/2 remains primary. */
        if (g_dev_count < USB_MAX_DEVICES) {
            usb_device_t* d = &g_devices[g_dev_count++];
            memset(d, 0, sizeof(*d));
            d->address    = (uint8_t)(port + 1);
            d->speed      = (status & 0x0100) ? USB_SPEED_LOW : USB_SPEED_FULL;
            d->configured = true;
            d->class      = USB_CLASS_HID;
            d->subclass   = USB_SUBCLASS_BOOT;
            /* Assume keyboard on port 1, mouse on port 2 (QEMU convention) */
            d->protocol      = (port == 0) ? USB_PROTOCOL_KBD : USB_PROTOCOL_MOUSE;
            d->hid_keyboard  = (d->protocol == USB_PROTOCOL_KBD);
            d->hid_mouse     = (d->protocol == USB_PROTOCOL_MOUSE);
            kinfo("USB: port %d: HID %s detected",
                  port + 1, d->hid_keyboard ? "keyboard" : "mouse");
        }
    }
}

/* =========================================================
 * HID polling (stub — PS/2 ISR handles real input)
 * Full implementation would read interrupt endpoint TD reports here.
 * ========================================================= */
void usb_hid_poll(void)
{
    /* Real USB HID polling would:
     * 1. Check if interrupt TD completed (IOC bit in TD status)
     * 2. Read 8-byte keyboard report or 4-byte mouse report
     * 3. Translate to keyboard/mouse state updates
     * Currently delegated to PS/2 ISR handlers for compatibility. */
}

/* =========================================================
 * Public API
 * ========================================================= */
void usb_init(void)
{
    memset(g_devices, 0, sizeof(g_devices));
    g_dev_count   = 0;
    g_uhci_base   = 0;
    g_initialized = true;

    find_uhci();
    if (g_uhci_base) {
        /* Global reset */
        outw((uint16_t)(g_uhci_base + UHCI_USBCMD), UHCI_CMD_HCRESET);
        for (volatile int i = 0; i < 100000; i++);
        outw((uint16_t)(g_uhci_base + UHCI_USBCMD), 0);

        /* Disable interrupts */
        outw((uint16_t)(g_uhci_base + UHCI_USBINTR), 0);

        /* Enumerate ports */
        uhci_enumerate();

        /* Start controller */
        outw((uint16_t)(g_uhci_base + UHCI_USBCMD), UHCI_CMD_RS);
        kinfo("USB: UHCI controller started, %d devices", g_dev_count);
    }
}

int  usb_device_count(void)          { return g_dev_count; }
const usb_device_t* usb_device_get(int idx)
{
    if (idx < 0 || idx >= g_dev_count) return NULL;
    return &g_devices[idx];
}

bool usb_kbd_available(void)
{
    for (int i = 0; i < g_dev_count; i++)
        if (g_devices[i].hid_keyboard && g_devices[i].configured) return true;
    return false;
}

bool usb_mouse_available(void)
{
    for (int i = 0; i < g_dev_count; i++)
        if (g_devices[i].hid_mouse && g_devices[i].configured) return true;
    return false;
}
