/*
 * include/drivers/usb.h — USB HID abstraction layer
 *
 * Provides a controller-agnostic USB stack skeleton with UHCI support.
 * USB devices are enumerated and matched to class drivers (HID keyboard,
 * HID mouse). Input events are fed into the existing keyboard/mouse state.
 *
 * Architecture:
 *   PCI scan → find UHCI/OHCI/xHCI controller → usb_init()
 *   → enumerate root hub → enumerate devices → match class driver
 *   → HID driver polls interrupt endpoint → update kbd/mouse state
 */
#pragma once
#include <types.h>

/* USB speed */
#define USB_SPEED_LOW   0
#define USB_SPEED_FULL  1
#define USB_SPEED_HIGH  2

/* USB class codes relevant to HID */
#define USB_CLASS_HID       0x03
#define USB_SUBCLASS_BOOT   0x01
#define USB_PROTOCOL_KBD    0x01
#define USB_PROTOCOL_MOUSE  0x02

/* Max devices/endpoints */
#define USB_MAX_DEVICES   16
#define USB_MAX_ENDPOINTS  8

typedef enum {
    USB_CTRL_NONE,
    USB_CTRL_UHCI,
    USB_CTRL_OHCI,
    USB_CTRL_XHCI,
} usb_ctrl_type_t;

typedef struct {
    uint8_t  address;
    uint8_t  speed;
    uint8_t  class;
    uint8_t  subclass;
    uint8_t  protocol;
    uint16_t vendor_id;
    uint16_t product_id;
    bool     configured;
    bool     hid_keyboard;
    bool     hid_mouse;

    /* Endpoint for interrupt IN (HID reports) */
    uint8_t  ep_addr;
    uint16_t ep_maxpkt;
    uint8_t  ep_interval;
} usb_device_t;

/* Initialise USB stack — scans PCI for controllers */
void usb_init(void);

/* Poll all HID devices for new reports (call each timer tick) */
void usb_hid_poll(void);

/* Get device list */
int              usb_device_count(void);
const usb_device_t* usb_device_get(int idx);

/* Returns true if any USB keyboard is connected and active */
bool usb_kbd_available(void);

/* Returns true if any USB mouse is connected and active */
bool usb_mouse_available(void);
