/*
 * drivers/mouse.c - PS/2 mouse driver
 *
 * Communicates with the secondary PS/2 port (IRQ 12) to receive
 * standard 3-byte mouse packets. Tracks screen position, buttons,
 * and generates GUI mouse events.
 */
#include <drivers/mouse.h>
#include <drivers/framebuffer.h>
#include <drivers/keyboard.h>
#include <gui/event.h>
#include <interrupts.h>
#include <kernel.h>
#include <types.h>

/* PS/2 controller ports */
#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_CMD     0x64

/* PS/2 controller commands */
#define PS2_CMD_ENABLE_PORT2   0xA8
#define PS2_CMD_GET_CFG        0x20
#define PS2_CMD_SET_CFG        0x60
#define PS2_CMD_WRITE_PORT2    0xD4

/* PS/2 mouse commands */
#define MOUSE_CMD_SET_SAMPLE    0xF3
#define MOUSE_CMD_ENABLE        0xF4
#define MOUSE_CMD_SET_DEFAULTS  0xF6
#define MOUSE_CMD_RESET         0xFF
#define MOUSE_ACK               0xFA

mouse_state_t mouse = { .x = 400, .y = 300, .buttons = 0 };

static int  max_x = 1023;
static int  max_y = 767;

/* Packet accumulation: PS/2 mouse sends 3 bytes per movement */
static uint8_t packet[3];
static int     packet_idx = 0;

/* Write to PS/2 data port, with status check */
static void ps2_write_mouse(uint8_t cmd)
{
    /* Wait for input buffer empty */
    int timeout = 100000;
    while ((inb(PS2_STATUS) & 0x02) && timeout--) cpu_pause();

    /* Signal write to port 2 */
    outb(PS2_CMD, PS2_CMD_WRITE_PORT2);
    timeout = 100000;
    while ((inb(PS2_STATUS) & 0x02) && timeout--) cpu_pause();

    outb(PS2_DATA, cmd);
}

static uint8_t ps2_read(void)
{
    int timeout = 100000;
    while (!(inb(PS2_STATUS) & 0x01) && timeout--) cpu_pause();
    return inb(PS2_DATA);
}

/*
 * mouse_init - enable the secondary PS/2 port and the mouse device.
 */
void mouse_init(void)
{
    /* Enable IRQ12 (secondary PS/2 port) */
    int timeout = 100000;
    while ((inb(PS2_STATUS) & 0x02) && timeout--) cpu_pause();
    outb(PS2_CMD, PS2_CMD_ENABLE_PORT2);

    /* Read and modify PS/2 controller configuration byte */
    timeout = 100000;
    while ((inb(PS2_STATUS) & 0x02) && timeout--) cpu_pause();
    outb(PS2_CMD, PS2_CMD_GET_CFG);
    timeout = 100000;
    while (!(inb(PS2_STATUS) & 0x01) && timeout--) cpu_pause();
    uint8_t cfg = inb(PS2_DATA);

    /* Enable secondary port interrupt (bit 1), enable port */
    cfg |= 0x02;
    cfg &= ~0x20; /* Clear "disable port 2" */
    timeout = 100000;
    while ((inb(PS2_STATUS) & 0x02) && timeout--) cpu_pause();
    outb(PS2_CMD, PS2_CMD_SET_CFG);
    timeout = 100000;
    while ((inb(PS2_STATUS) & 0x02) && timeout--) cpu_pause();
    outb(PS2_DATA, cfg);

    /* Reset mouse */
    ps2_write_mouse(MOUSE_CMD_RESET);
    ps2_read(); /* ACK */
    ps2_read(); /* 0xAA self-test */
    ps2_read(); /* 0x00 mouse ID */

    /* Set defaults */
    ps2_write_mouse(MOUSE_CMD_SET_DEFAULTS);
    ps2_read(); /* ACK */

    /* Enable data reporting */
    ps2_write_mouse(MOUSE_CMD_ENABLE);
    ps2_read(); /* ACK */

    /* Register IRQ 12 handler */
    irq_register_handler(12, (irq_handler_t)mouse_irq_handler);

    mouse.x = max_x / 2;
    mouse.y = max_y / 2;

    kinfo("PS/2 mouse initialized");
}

void mouse_set_bounds(int mx, int my)
{
    max_x = mx - 1;
    max_y = my - 1;
}

/*
 * mouse_irq_handler - called on every IRQ 12.
 * Accumulates 3-byte packets and posts GUI events.
 */
void mouse_irq_handler(void* regs)
{
    (void)regs;

    uint8_t data = inb(PS2_DATA);
    packet[packet_idx++] = data;

    if (packet_idx < 3) return;
    packet_idx = 0;

    /* Decode the 3-byte packet */
    uint8_t flags = packet[0];

    /* Discard if overflow bits are set */
    if (flags & 0xC0) return;

    /* X and Y movement (signed, 9-bit with sign bit in flags byte) */
    int dx = (int)packet[1] - ((flags & 0x10) ? 256 : 0);
    int dy = (int)packet[2] - ((flags & 0x20) ? 256 : 0);

    /* Mouse Y axis is inverted (up = negative in PS/2, up = lower Y on screen) */
    dy = -dy;

    mouse.prev_buttons = mouse.buttons;
    mouse.buttons      = flags & 0x07;
    mouse.dx = dx;
    mouse.dy = dy;

    mouse.x += dx;
    mouse.y += dy;

    /* Clamp to screen bounds */
    if (mouse.x < 0)      mouse.x = 0;
    if (mouse.y < 0)      mouse.y = 0;
    if (mouse.x > max_x)  mouse.x = max_x;
    if (mouse.y > max_y)  mouse.y = max_y;

    /* Post event to GUI queue */
    gui_post_mouse(mouse.x, mouse.y, dx, dy, mouse.buttons);
}

void mouse_get_event(mouse_event_t* evt)
{
    evt->x              = mouse.x;
    evt->y              = mouse.y;
    evt->dx             = mouse.dx;
    evt->dy             = mouse.dy;
    evt->buttons        = mouse.buttons;
    evt->left_clicked   = (mouse.buttons & MOUSE_BTN_LEFT) &&
                          !(mouse.prev_buttons & MOUSE_BTN_LEFT);
    evt->left_released  = !(mouse.buttons & MOUSE_BTN_LEFT) &&
                          (mouse.prev_buttons & MOUSE_BTN_LEFT);
    evt->right_clicked  = (mouse.buttons & MOUSE_BTN_RIGHT) &&
                          !(mouse.prev_buttons & MOUSE_BTN_RIGHT);
}

/* ============================================================
 * Mouse cursor: a classic 16x16 arrow sprite
 * ============================================================ */

/* Cursor bitmap: 1 = filled, 2 = outline, 0 = transparent */
static const uint8_t cursor_mask[16][16] = {
    {1,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,2,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,2,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,2,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,2,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,2,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,2,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,1,2,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,2,2,2,0,0,0,0,0,0,0},
    {1,1,1,2,1,1,2,0,0,0,0,0,0,0,0,0},
    {1,1,2,0,1,1,1,2,0,0,0,0,0,0,0,0},
    {1,2,0,0,0,1,1,2,0,0,0,0,0,0,0,0},
    {2,0,0,0,0,1,1,1,2,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,1,1,2,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,1,2,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,2,0,0,0,0,0,0,0,0,0},
};

void mouse_draw_cursor(uint32_t* back_buf, int fw, int fh)
{
    int cx = mouse.x;
    int cy = mouse.y;

    for (int row = 0; row < 16; row++) {
        for (int col = 0; col < 16; col++) {
            int px = cx + col;
            int py = cy + row;
            if (px < 0 || py < 0 || px >= fw || py >= fh) continue;

            uint8_t v = cursor_mask[row][col];
            if (v == 1) {
                back_buf[py * fw + px] = 0xFFFFFFFF; /* White */
            } else if (v == 2) {
                back_buf[py * fw + px] = 0xFF000000; /* Black outline */
            }
            /* 0 = transparent, skip */
        }
    }
}
