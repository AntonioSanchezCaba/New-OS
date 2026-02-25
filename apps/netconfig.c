/*
 * apps/netconfig.c — AetherOS Network Configuration (ARE SURF_FLOAT)
 *
 * Displays live network interface status:
 *   - Link state, MAC, IP, netmask, gateway, DNS server
 *   - e1000 TX/RX packet counters
 *   - ICMP ping with RTT readout (click "Ping" button)
 *   - "Renew DHCP" action button
 *
 * Opens as a draggable SURF_FLOAT window via surface_netconfig_open().
 * Self-invalidates every second to show live counters; while a ping
 * is in flight it polls every render pass until a reply arrives.
 */
#include <aether/are.h>
#include <aether/surface.h>
#include <aether/input.h>
#include <gui/draw.h>
#include <gui/font.h>
#include <net/net.h>
#include <net/ip.h>
#include <net/dhcp.h>
#include <net/dns.h>
#include <drivers/e1000.h>
#include <drivers/timer.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>

/* =========================================================
 * Geometry
 * ========================================================= */
#define NC_W         420
#define NC_H         360
#define NC_HDR_H     32
#define NC_ROW_H     22
#define NC_PAD       14
#define NC_LBL_W     130
#define NC_VAL_X     (NC_PAD + NC_LBL_W + 4)
#define NC_REFRESH   100   /* ticks between auto-refresh */

/* Colours */
#define NC_BG        ACOLOR(0x0C, 0x10, 0x1A, 0xFF)
#define NC_HDR_BG    ACOLOR(0x10, 0x18, 0x2C, 0xFF)
#define NC_HDR_FG    ACOLOR(0x80, 0xC0, 0xFF, 0xFF)
#define NC_ROW_BG    ACOLOR(0x12, 0x18, 0x24, 0xFF)
#define NC_ROW_ALT   ACOLOR(0x10, 0x16, 0x20, 0xFF)
#define NC_LBL_FG    ACOLOR(0x80, 0x90, 0xA0, 0xFF)
#define NC_VAL_FG    ACOLOR(0xD0, 0xE0, 0xFF, 0xFF)
#define NC_GOOD_FG   ACOLOR(0x60, 0xE0, 0x80, 0xFF)
#define NC_BAD_FG    ACOLOR(0xE0, 0x50, 0x40, 0xFF)
#define NC_BORDER    ACOLOR(0x20, 0x30, 0x48, 0xFF)
#define NC_BTN_BG    ACOLOR(0x20, 0x48, 0x80, 0xFF)
#define NC_BTN_FG    ACOLOR(0xFF, 0xFF, 0xFF, 0xFF)
#define NC_PING_OK   ACOLOR(0x60, 0xE0, 0x60, 0xFF)
#define NC_PING_ERR  ACOLOR(0xE0, 0x60, 0x40, 0xFF)
#define NC_PING_WAIT ACOLOR(0xFF, 0xC0, 0x40, 0xFF)

#define PING_IDLE    0
#define PING_WAIT    1
#define PING_OK      2
#define PING_TIMEOUT 3

/* =========================================================
 * State
 * ========================================================= */
typedef struct {
    sid_t    sid;
    bool     open;
    uint32_t last_refresh;
    /* Ping */
    int      ping_state;
    uint16_t ping_seq;
    uint32_t ping_sent_ticks;
    uint32_t ping_rtt_ms;
    /* Button hit areas (surface-relative Y) */
    int      dhcp_btn_y;
    int      ping_btn_y;
} nc_state_t;

static nc_state_t g_nc;

/* =========================================================
 * Helpers
 * ========================================================= */
static void nc_draw_row(canvas_t* c, int y, const char* label,
                         const char* value, acolor_t val_color, bool alt)
{
    draw_rect(c, 0, y, NC_W, NC_ROW_H, alt ? NC_ROW_ALT : NC_ROW_BG);
    draw_rect(c, 0, y + NC_ROW_H - 1, NC_W, 1, NC_BORDER);
    draw_string(c, NC_PAD, y + (NC_ROW_H - FONT_H)/2,
                label, NC_LBL_FG, ACOLOR(0,0,0,0));
    draw_string(c, NC_VAL_X, y + (NC_ROW_H - FONT_H)/2,
                value, val_color, ACOLOR(0,0,0,0));
}

/* =========================================================
 * Render
 * ========================================================= */
static void nc_render(sid_t id, uint32_t* pixels, uint32_t w, uint32_t h,
                       void* ud)
{
    (void)ud;
    canvas_t c = { .pixels = pixels, .width = (int)w, .height = (int)h };

    uint32_t now = timer_get_ticks();

    /* Poll ping result */
    if (g_nc.ping_state == PING_WAIT) {
        if (g_icmp_reply_seq == g_nc.ping_seq) {
            g_nc.ping_rtt_ms = (now - g_nc.ping_sent_ticks) * 1000 / TIMER_FREQ;
            g_nc.ping_state  = PING_OK;
        } else if (now - g_nc.ping_sent_ticks > (uint32_t)TIMER_FREQ * 2) {
            g_nc.ping_state = PING_TIMEOUT;
        }
    }

    draw_rect(&c, 0, 0, (int)w, (int)h, NC_BG);

    /* Header */
    int y = 0;
    draw_rect(&c, 0, y, (int)w, NC_HDR_H, NC_HDR_BG);
    draw_string(&c, NC_PAD, y + (NC_HDR_H - FONT_H)/2,
                "Network  eth0", NC_HDR_FG, ACOLOR(0,0,0,0));
    y += NC_HDR_H;

    /* Link state */
    acolor_t link_col = net_iface.up ? NC_GOOD_FG : NC_BAD_FG;
    nc_draw_row(&c, y, "Link",
                net_iface.up ? "UP  RUNNING" : "DOWN",
                link_col, false);
    y += NC_ROW_H;

    /* MAC */
    char mac_str[20];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             net_iface.mac.b[0], net_iface.mac.b[1], net_iface.mac.b[2],
             net_iface.mac.b[3], net_iface.mac.b[4], net_iface.mac.b[5]);
    nc_draw_row(&c, y, "MAC", mac_str, NC_VAL_FG, true);
    y += NC_ROW_H;

    /* IPv4 addresses */
    uint32_t ip = net_iface.ip, nm = net_iface.netmask, gw = net_iface.gateway;
    char ip_str[18], nm_str[18], gw_str[18];
    snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u",
             ip&0xFF,(ip>>8)&0xFF,(ip>>16)&0xFF,(ip>>24)&0xFF);
    snprintf(nm_str, sizeof(nm_str), "%u.%u.%u.%u",
             nm&0xFF,(nm>>8)&0xFF,(nm>>16)&0xFF,(nm>>24)&0xFF);
    snprintf(gw_str, sizeof(gw_str), "%u.%u.%u.%u",
             gw&0xFF,(gw>>8)&0xFF,(gw>>16)&0xFF,(gw>>24)&0xFF);

    uint32_t dns_ip = dns_get_server();
    char dns_str[18];
    snprintf(dns_str, sizeof(dns_str), "%u.%u.%u.%u",
             dns_ip&0xFF,(dns_ip>>8)&0xFF,(dns_ip>>16)&0xFF,(dns_ip>>24)&0xFF);

    nc_draw_row(&c, y, "IPv4",    ip_str,  NC_VAL_FG, false); y += NC_ROW_H;
    nc_draw_row(&c, y, "Netmask", nm_str,  NC_VAL_FG, true);  y += NC_ROW_H;
    nc_draw_row(&c, y, "Gateway", gw_str,  NC_VAL_FG, false); y += NC_ROW_H;
    nc_draw_row(&c, y, "DNS",     dns_str, NC_VAL_FG, true);  y += NC_ROW_H;

    /* NIC counters */
    e1000_stats_t stats = { 0, 0, 0, 0 };
    e1000_get_stats(&stats);
    char tx_str[20], rx_str[20];
    snprintf(tx_str, sizeof(tx_str), "%u pkts", (unsigned)stats.tx_packets);
    snprintf(rx_str, sizeof(rx_str), "%u pkts", (unsigned)stats.rx_packets);
    nc_draw_row(&c, y, "TX",  tx_str, NC_VAL_FG, false); y += NC_ROW_H;
    nc_draw_row(&c, y, "RX",  rx_str, NC_VAL_FG, true);  y += NC_ROW_H;

    /* Divider */
    y += 8;
    draw_rect(&c, NC_PAD, y, (int)w - 2*NC_PAD, 1, NC_BORDER);
    y += 8;

    /* Renew DHCP button */
    g_nc.dhcp_btn_y = y;
    draw_rect(&c, NC_PAD, y, 120, 26, NC_BTN_BG);
    draw_string(&c, NC_PAD + (120 - 11*FONT_W)/2, y + (26 - FONT_H)/2,
                "Renew DHCP", NC_BTN_FG, ACOLOR(0,0,0,0));
    y += 34;

    /* Ping button + result */
    g_nc.ping_btn_y = y;
    draw_rect(&c, NC_PAD, y, 60, 26, ACOLOR(0x18, 0x38, 0x58, 0xFF));
    draw_string(&c, NC_PAD + (60 - 4*FONT_W)/2, y + (26 - FONT_H)/2,
                "Ping", NC_BTN_FG, ACOLOR(0,0,0,0));

    acolor_t ping_col;
    const char* ping_txt;
    char rtt_buf[32];
    switch (g_nc.ping_state) {
    case PING_WAIT:
        ping_txt = "waiting..."; ping_col = NC_PING_WAIT; break;
    case PING_OK:
        snprintf(rtt_buf, sizeof(rtt_buf), "%s  ok  %u ms",
                 gw_str, (unsigned)g_nc.ping_rtt_ms);
        ping_txt = rtt_buf; ping_col = NC_PING_OK; break;
    case PING_TIMEOUT:
        ping_txt = "timeout"; ping_col = NC_PING_ERR; break;
    default:
        ping_txt = gw_str; ping_col = NC_LBL_FG; break;
    }
    draw_string(&c, NC_PAD + 68, y + (26 - FONT_H)/2,
                ping_txt, ping_col, ACOLOR(0,0,0,0));

    /* Keep surface live: re-invalidate if ping in flight or 1s elapsed */
    if (g_nc.ping_state == PING_WAIT ||
        now - g_nc.last_refresh >= (uint32_t)NC_REFRESH) {
        g_nc.last_refresh = now;
        surface_invalidate(id);
    }
}

/* =========================================================
 * Input
 * ========================================================= */
static void nc_input(sid_t id, const input_event_t* ev, void* ud)
{
    (void)ud;
    if (ev->type != INPUT_POINTER) return;
    bool click = (ev->pointer.buttons  & IBTN_LEFT) &&
                !(ev->pointer.prev_buttons & IBTN_LEFT);
    if (!click) return;

    int mx = ev->pointer.x, my = ev->pointer.y;

    if (mx >= NC_PAD && mx < NC_PAD + 120 &&
        my >= g_nc.dhcp_btn_y && my < g_nc.dhcp_btn_y + 26) {
        dhcp_discover();
        surface_invalidate(id);
        return;
    }
    if (mx >= NC_PAD && mx < NC_PAD + 60 &&
        my >= g_nc.ping_btn_y && my < g_nc.ping_btn_y + 26) {
        if (g_nc.ping_state != PING_WAIT && net_iface.up) {
            g_nc.ping_seq++;
            g_nc.ping_sent_ticks = timer_get_ticks();
            g_nc.ping_state      = PING_WAIT;
            icmp_ping(net_iface.gateway, g_nc.ping_seq);
            surface_invalidate(id);
        }
    }
}

/* =========================================================
 * Close
 * ========================================================= */
static void nc_on_close(sid_t id, void* ud)
{
    (void)id; (void)ud;
    g_nc.open = false;
    g_nc.sid  = SID_NONE;
}

/* =========================================================
 * Public API
 * ========================================================= */
sid_t surface_netconfig_open(void)
{
    if (g_nc.open && g_nc.sid != SID_NONE) return g_nc.sid;

    g_nc.ping_state   = PING_IDLE;
    g_nc.ping_seq     = 0;
    g_nc.last_refresh = timer_get_ticks();
    g_nc.open         = true;

    g_nc.sid = are_add_surface(SURF_FLOAT, NC_W, NC_H,
                                "Network", "N",
                                nc_render, nc_input,
                                nc_on_close, NULL);
    surface_invalidate(g_nc.sid);
    return g_nc.sid;
}
