/*
 * apps/netconfig.c — AetherOS Network Configuration
 *
 * Displays network interface status and allows diagnostics:
 *   - MAC address, IP, netmask, gateway, DNS server
 *   - DHCP lease info (lease time)
 *   - Ping host (ICMP echo, shows RTT or timeout)
 *   - Renew DHCP button
 *   - TX/RX packet counters from e1000
 *
 * Integration: call app_netconfig_create() to open the window;
 * netconfig_tick() is called from the desktop main loop every second.
 */
#include <gui/window.h>
#include <gui/draw.h>
#include <gui/event.h>
#include <gui/theme.h>
#include <net/net.h>
#include <net/ip.h>
#include <net/dhcp.h>
#include <net/dns.h>
#include <drivers/e1000.h>
#include <drivers/timer.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>
#include <kernel/version.h>

/* =========================================================
 * Layout
 * ========================================================= */
#define NC_W         480
#define NC_H         400
#define NC_HDR_H      28
#define NC_ROW_H      22
#define NC_INDENT      8
#define NC_LBL_W     148
#define NC_VAL_X     (NC_INDENT + NC_LBL_W + 4)
#define NC_REFRESH   100   /* ticks between auto-refresh */

/* Ping states */
#define PING_IDLE    0
#define PING_WAIT    1
#define PING_OK      2
#define PING_TIMEOUT 3

/* =========================================================
 * State
 * ========================================================= */
typedef struct {
    wid_t    wid;
    uint32_t last_ticks;
    /* Ping */
    int      ping_state;
    char     ping_host[64];
    int      ping_len;
    bool     ping_input_focused;
    uint16_t ping_seq;
    uint32_t ping_sent;
    uint32_t ping_rtt_ms;
} nc_t;

static nc_t* g_nc = NULL;

/* =========================================================
 * Formatting helpers
 * ========================================================= */
static void fmt_uint(char* buf, size_t sz, uint32_t n)
{
    char tmp[12]; int i = 0;
    if (!sz) return;
    if (n == 0) { buf[0]='0'; buf[1]='\0'; return; }
    while (n) { tmp[i++] = '0'+(n%10); n /= 10; }
    int j = 0;
    for (int k = i-1; k >= 0 && j < (int)sz-1; k--) buf[j++] = tmp[k];
    buf[j] = '\0';
}

static void fmt_ip(char* buf, size_t sz, uint32_t ip_ne)
{
    /* ip_ne is in network (big-endian) byte order */
    uint8_t a = (uint8_t)((ip_ne >> 24) & 0xFF);
    uint8_t b = (uint8_t)((ip_ne >> 16) & 0xFF);
    uint8_t c = (uint8_t)((ip_ne >>  8) & 0xFF);
    uint8_t d = (uint8_t)( ip_ne        & 0xFF);
    char tmp[4];
    buf[0] = '\0';
#define OC(v) do { \
    uint8_t _v=(v); int i_=0; char r_[4]; \
    if(!_v){r_[0]='0';i_=1;}else{while(_v){r_[i_++]='0'+_v%10;_v/=10;}} \
    char s_[4]; for(int k_=i_-1,j_=0;k_>=0;k_--,j_++) s_[j_]=r_[k_]; s_[i_]='\0'; \
    strncat(buf,s_,sz-strlen(buf)-1); \
} while(0)
    OC(a); strncat(buf,".",sz-strlen(buf)-1);
    OC(b); strncat(buf,".",sz-strlen(buf)-1);
    OC(c); strncat(buf,".",sz-strlen(buf)-1);
    OC(d);
#undef OC
    (void)tmp;
}

static void fmt_mac(char* buf, size_t sz, const mac_addr_t* mac)
{
    static const char h[] = "0123456789ABCDEF";
    char t[18];
    for (int i = 0; i < 6; i++) {
        t[i*3+0] = h[(mac->b[i]>>4)&0xF];
        t[i*3+1] = h[ mac->b[i]    &0xF];
        t[i*3+2] = (i<5) ? ':' : '\0';
    }
    t[17] = '\0';
    strncpy(buf, t, sz-1); buf[sz-1] = '\0';
}

/* =========================================================
 * Draw helpers
 * ========================================================= */
static void nc_section(canvas_t* c, int* y, const char* title)
{
    const theme_t* th = theme_current();
    draw_rect(c, 0, *y, NC_W, NC_HDR_H, th->win_title_bg);
    draw_hline(c, 0, *y, NC_W, th->panel_border);
    draw_string(c, NC_INDENT, *y+(NC_HDR_H-FONT_H)/2,
                title, th->win_title_text, rgba(0,0,0,0));
    *y += NC_HDR_H;
    draw_hline(c, 0, *y, NC_W, th->panel_border);
}

static void nc_row(canvas_t* c, int* y, const char* label,
                   const char* value, uint32_t val_col)
{
    draw_string(c, NC_INDENT, *y+(NC_ROW_H-FONT_H)/2,
                label, rgba(0x80,0xA0,0xC0,0xFF), rgba(0,0,0,0));
    draw_string(c, NC_VAL_X, *y+(NC_ROW_H-FONT_H)/2,
                value, val_col, rgba(0,0,0,0));
    *y += NC_ROW_H;
}

/* =========================================================
 * Draw
 * ========================================================= */
static void nc_draw(nc_t* nc, canvas_t* c)
{
    const theme_t* th = theme_current();
    draw_rect(c, 0, 0, NC_W, NC_H, th->win_bg);

    int y = 0;
    char buf[80];

    /* === Interface === */
    nc_section(c, &y, "Network Interface");

    bool up = net_iface.up;
    nc_row(c, &y, "Status:",
           up ? "UP  (link active)" : "DOWN (no link)",
           up ? th->ok : th->error);

    fmt_mac(buf, sizeof(buf), &net_iface.mac);
    nc_row(c, &y, "MAC Address:", buf, th->text_primary);

    /* net_iface.ip is in network byte order — pass directly to fmt_ip */
    if (net_iface.ip == 0) strncpy(buf, "(not assigned)", sizeof(buf)-1);
    else fmt_ip(buf, sizeof(buf), htonl(net_iface.ip));
    nc_row(c, &y, "IP Address:", buf, (net_iface.ip==0)?th->warn:th->ok);

    if (net_iface.netmask == 0) strncpy(buf, "(none)", sizeof(buf)-1);
    else fmt_ip(buf, sizeof(buf), htonl(net_iface.netmask));
    nc_row(c, &y, "Subnet Mask:", buf, th->text_primary);

    if (net_iface.gateway == 0) strncpy(buf, "(none)", sizeof(buf)-1);
    else fmt_ip(buf, sizeof(buf), htonl(net_iface.gateway));
    nc_row(c, &y, "Default Gateway:", buf, th->text_primary);

    uint32_t dns_srv = dns_get_server();
    if (dns_srv == 0) strncpy(buf, "(none)", sizeof(buf)-1);
    else fmt_ip(buf, sizeof(buf), htonl(dns_srv));
    nc_row(c, &y, "DNS Server:", buf, th->text_primary);

    y += 4;

    /* === DHCP === */
    nc_section(c, &y, "DHCP Lease");
    if (dhcp_has_lease()) {
        const dhcp_lease_t* l = dhcp_get_lease();
        fmt_ip(buf, sizeof(buf), htonl(l->ip));
        nc_row(c, &y, "Assigned IP:", buf, th->ok);

        buf[0] = '\0';
        char h2[8], m2[8];
        fmt_uint(h2, sizeof(h2), l->lease_sec / 3600);
        fmt_uint(m2, sizeof(m2), (l->lease_sec % 3600) / 60);
        strncat(buf, h2, sizeof(buf)-strlen(buf)-1);
        strncat(buf, "h ", sizeof(buf)-strlen(buf)-1);
        strncat(buf, m2, sizeof(buf)-strlen(buf)-1);
        strncat(buf, "m", sizeof(buf)-strlen(buf)-1);
        nc_row(c, &y, "Lease Time:", buf, th->text_primary);
    } else {
        nc_row(c, &y, "Status:", "No DHCP lease", th->warn);
    }

    /* Renew button */
    int ren_y = y - NC_ROW_H;
    draw_rect_rounded(c, NC_W-120-8, ren_y+3, 120, NC_ROW_H-6, 3, th->btn_normal);
    draw_string_centered(c, NC_W-120-8, ren_y+3, 120, NC_ROW_H-6,
                         "Renew DHCP", th->btn_text, rgba(0,0,0,0));
    y += 4;

    /* === Ping === */
    nc_section(c, &y, "Ping");

    /* Input box */
    int inp_y = y + 3;
    draw_rect_rounded(c, NC_INDENT, inp_y, 260, NC_ROW_H-6, 3,
                      rgba(0,0,0,0x30));
    draw_rect_outline(c, NC_INDENT, inp_y, 260, NC_ROW_H-6, 1,
                      nc->ping_input_focused ? th->accent : th->panel_border);
    if (nc->ping_host[0])
        draw_string(c, NC_INDENT+4, inp_y+(NC_ROW_H-6-FONT_H)/2,
                    nc->ping_host, th->text_primary, rgba(0,0,0,0));
    else
        draw_string(c, NC_INDENT+4, inp_y+(NC_ROW_H-6-FONT_H)/2,
                    "host or IP  (e.g. 8.8.8.8)",
                    rgba(0x88,0x88,0x88,0xFF), rgba(0,0,0,0));

    /* Cursor blink */
    if (nc->ping_input_focused && ((timer_get_ticks()/30)&1)) {
        int cx = NC_INDENT+4 + nc->ping_len*FONT_W;
        draw_vline(c, cx, inp_y+2, NC_ROW_H-10, th->text_primary);
    }

    /* Ping button */
    bool pinging = (nc->ping_state == PING_WAIT);
    draw_rect_rounded(c, NC_INDENT+268, inp_y, 60, NC_ROW_H-6, 3,
                      pinging ? th->accent : th->btn_normal);
    draw_string_centered(c, NC_INDENT+268, inp_y, 60, NC_ROW_H-6,
                         pinging ? "..." : "Ping",
                         th->btn_text, rgba(0,0,0,0));
    y += NC_ROW_H;

    /* Ping result */
    const char* res_txt = NULL;
    uint32_t    res_col = th->text_primary;
    if (nc->ping_state == PING_OK) {
        buf[0] = '\0';
        strncat(buf, nc->ping_host, 40);
        strncat(buf, ": reply ", sizeof(buf)-strlen(buf)-1);
        char ms[8]; fmt_uint(ms, sizeof(ms), nc->ping_rtt_ms);
        strncat(buf, ms, sizeof(buf)-strlen(buf)-1);
        strncat(buf, " ms", sizeof(buf)-strlen(buf)-1);
        res_txt = buf; res_col = th->ok;
    } else if (nc->ping_state == PING_TIMEOUT) {
        res_txt = "Request timed out"; res_col = th->error;
    } else if (nc->ping_state == PING_WAIT) {
        res_txt = "Sending..."; res_col = th->warn;
    } else {
        res_txt = "Enter a host and press Ping";
        res_col = rgba(0x80,0x80,0x80,0xFF);
    }
    nc_row(c, &y, "Result:", res_txt, res_col);
    y += 4;

    /* === Statistics === */
    nc_section(c, &y, "Interface Statistics");
    e1000_stats_t st; e1000_get_stats(&st);
    char n1[12],n2[12],n3[12],n4[12];
    fmt_uint(n1,sizeof(n1),st.tx_packets); strncat(n1," pkts",sizeof(n1)-strlen(n1)-1);
    fmt_uint(n2,sizeof(n2),st.rx_packets); strncat(n2," pkts",sizeof(n2)-strlen(n2)-1);
    fmt_uint(n3,sizeof(n3),st.tx_errors);  strncat(n3," err", sizeof(n3)-strlen(n3)-1);
    fmt_uint(n4,sizeof(n4),st.rx_errors);  strncat(n4," err", sizeof(n4)-strlen(n4)-1);
    nc_row(c, &y, "Packets sent:",     n1, th->text_primary);
    nc_row(c, &y, "Packets received:", n2, th->text_primary);
    nc_row(c, &y, "TX errors:",        n3, st.tx_errors?th->error:th->text_primary);
    nc_row(c, &y, "RX errors:",        n4, st.rx_errors?th->error:th->text_primary);
}

/* =========================================================
 * Ping
 * ========================================================= */
static void nc_do_ping(nc_t* nc)
{
    if (nc->ping_state == PING_WAIT) return;
    if (!nc->ping_host[0]) return;

    /* dns_resolve returns network-byte-order IP */
    uint32_t target = dns_resolve(nc->ping_host);
    if (target == 0) { nc->ping_state = PING_TIMEOUT; return; }

    nc->ping_seq++;
    nc->ping_state = PING_WAIT;
    nc->ping_sent  = timer_get_ticks();
    icmp_ping(target, nc->ping_seq);
}

static void nc_check_ping(nc_t* nc)
{
    if (nc->ping_state != PING_WAIT) return;

    /* Poll NIC for incoming replies */
    if (net_iface.poll) net_iface.poll();

    if (g_icmp_reply_seq == nc->ping_seq) {
        uint32_t dt = timer_get_ticks() - nc->ping_sent;
        nc->ping_rtt_ms = (dt * 1000) / TIMER_FREQ;
        nc->ping_state  = PING_OK;
        return;
    }
    if (timer_get_ticks() - nc->ping_sent > 3 * TIMER_FREQ)
        nc->ping_state = PING_TIMEOUT;
}

/* =========================================================
 * Event handler
 * ========================================================= */
static void nc_on_event(wid_t wid, gui_event_t* evt, void* ud)
{
    (void)ud;
    nc_t* nc = g_nc;
    if (!nc || nc->wid != wid) return;

    switch (evt->type) {
    case GUI_EVENT_PAINT: {
        canvas_t c = wm_client_canvas(wid);
        nc_draw(nc, &c);
        break;
    }

    case GUI_EVENT_KEY_DOWN:
        if (nc->ping_input_focused) {
            int k = evt->key.keycode;
            char ch = evt->key.ch;
            if (k == KEY_ENTER) {
                nc_do_ping(nc); wm_invalidate(wid);
            } else if (k == KEY_BACKSPACE) {
                if (nc->ping_len > 0)
                    nc->ping_host[--nc->ping_len] = '\0';
                wm_invalidate(wid);
            } else if (ch >= 0x20 && nc->ping_len < 63) {
                nc->ping_host[nc->ping_len++] = ch;
                nc->ping_host[nc->ping_len]   = '\0';
                wm_invalidate(wid);
            }
        }
        break;

    case GUI_EVENT_MOUSE_DOWN: {
        int mx = evt->mouse.x, my = evt->mouse.y;

        /* Compute layout positions to hit-test buttons */
        /* Section 1 (Interface): HDR + 5 rows + 4px gap */
        int y_iface_end = NC_HDR_H + 5*NC_ROW_H + 4;
        /* Section 2 (DHCP): HDR + rows + 4px gap */
        int dhcp_rows = dhcp_has_lease() ? 2 : 1;
        int y_dhcp_end = y_iface_end + NC_HDR_H + dhcp_rows*NC_ROW_H + 4;
        /* Renew button is in last DHCP row */
        int ren_y = y_iface_end + NC_HDR_H + (dhcp_rows-1)*NC_ROW_H + 3;
        if (my >= ren_y && my < ren_y+NC_ROW_H-6 &&
            mx >= NC_W-128 && mx < NC_W-8) {
            dhcp_discover(); wm_invalidate(wid); break;
        }

        /* Section 3 (Ping): HDR then input row */
        int ping_hdr_y = y_dhcp_end;
        int inp_y = ping_hdr_y + NC_HDR_H + 3;

        /* Ping input box */
        if (my >= inp_y && my < inp_y+NC_ROW_H-6) {
            if (mx >= NC_INDENT && mx < NC_INDENT+260)
                nc->ping_input_focused = true;
            else
                nc->ping_input_focused = false;

            /* Ping button */
            if (mx >= NC_INDENT+268 && mx < NC_INDENT+328) {
                nc_do_ping(nc);
            }
            wm_invalidate(wid);
        } else {
            nc->ping_input_focused = false;
        }
        break;
    }

    case GUI_EVENT_CLOSE:
        kfree(nc); g_nc = NULL;
        break;

    default: break;
    }
}

/* =========================================================
 * Public API
 * ========================================================= */
wid_t app_netconfig_create(void)
{
    if (g_nc) {
        wm_focus(g_nc->wid); wm_raise(g_nc->wid);
        return g_nc->wid;
    }
    g_nc = (nc_t*)kmalloc(sizeof(nc_t));
    if (!g_nc) return -1;
    memset(g_nc, 0, sizeof(nc_t));
    g_nc->ping_state = PING_IDLE;

    wid_t wid = wm_create_window("Network — " OS_NAME,
                                  90, 60, NC_W, NC_H, nc_on_event, NULL);
    if (wid < 0) { kfree(g_nc); g_nc = NULL; return -1; }
    g_nc->wid = wid;
    wm_invalidate(wid);
    return wid;
}

void netconfig_tick(void)
{
    if (!g_nc) return;
    window_t* win = wm_get_window(g_nc->wid);
    if (!win) { kfree(g_nc); g_nc = NULL; return; }
    if (!(win->flags & WF_VISIBLE) || (win->flags & WF_MINIMIZED)) return;

    nc_check_ping(g_nc);

    uint32_t now = timer_get_ticks();
    if (now - g_nc->last_ticks >= NC_REFRESH) {
        g_nc->last_ticks = now;
        wm_invalidate(g_nc->wid);
    }
}
