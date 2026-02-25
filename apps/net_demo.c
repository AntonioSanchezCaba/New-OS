/*
 * apps/net_demo.c — AetherOS network stack demonstration
 *
 * Exercises the full network stack from a kernel context:
 *
 *   Phase 1 — ICMP Ping
 *     Sends 4 ICMP echo requests to the QEMU gateway (10.0.2.2)
 *     and measures round-trip time using the g_icmp_reply_seq flag
 *     that icmp_receive() updates.
 *
 *   Phase 2 — UDP echo
 *     Sends a single UDP datagram to port 7 (Echo) on the gateway.
 *     Waits up to 2 s for the reply using udp_recv_blocking().
 *
 *   Phase 3 — TCP connect / send / recv
 *     Opens a TCP connection to port 80 on the gateway.
 *     Sends a minimal HTTP/1.0 HEAD request and prints the
 *     first 64 bytes of the response.
 *
 * All three phases use the kernel-side APIs directly, without going
 * through the BSD socket layer, so they work before userland is up.
 *
 * To run from the kernel shell (terminal surface):
 *   net_demo_run();
 *
 * The function is also called automatically from kernel_main when
 * net_demo_run() is registered, but it can be driven on-demand.
 */

#include <net/net.h>
#include <net/ip.h>
#include <net/tcp.h>
#include <net/udp.h>
#include <net/socket.h>
#include <drivers/timer.h>
#include <kernel.h>
#include <string.h>

/* =========================================================
 * Helpers
 * ========================================================= */

/* Spin up to @timeout_ms milliseconds waiting for an ICMP echo reply
 * with sequence @seq.  Pumps net_iface.poll() while waiting.
 * Returns elapsed ticks (>0) on success, 0 on timeout. */
static uint32_t ping_wait_reply(uint16_t seq, uint32_t timeout_ms)
{
    uint32_t freq     = TIMER_FREQ;
    uint32_t deadline = timer_get_ticks() + timeout_ms * freq / 1000u;
    uint32_t t0       = timer_get_ticks();

    while (timer_get_ticks() < deadline) {
        if (net_iface.poll) net_iface.poll();
        if (g_icmp_reply_seq == seq) {
            return timer_get_ticks() - t0;
        }
    }
    return 0;
}

/* =========================================================
 * Phase 1 — ICMP Ping
 * ========================================================= */

static void demo_ping(ip4_addr_t target)
{
    klog_info("net_demo: === ICMP Ping ===");
    klog_info("net_demo: target %u.%u.%u.%u",
              (target >>  0) & 0xFF, (target >>  8) & 0xFF,
              (target >> 16) & 0xFF, (target >> 24) & 0xFF);

    for (uint16_t seq = 1; seq <= 4; seq++) {
        int ret = icmp_ping(target, seq);
        if (ret < 0) {
            klog_warn("net_demo: ping seq=%u send failed (ARP timeout?)", seq);
            continue;
        }

        uint32_t elapsed_ticks = ping_wait_reply(seq, 2000);
        if (elapsed_ticks == 0) {
            klog_warn("net_demo: ping seq=%u timeout", seq);
        } else {
            uint32_t rtt_ms = elapsed_ticks * 1000u / TIMER_FREQ;
            klog_info("net_demo: ping seq=%u reply in %u ms", seq, rtt_ms);
        }

        /* 1-second inter-packet gap */
        uint32_t gap = timer_get_ticks() + TIMER_FREQ;
        while (timer_get_ticks() < gap) {
            if (net_iface.poll) net_iface.poll();
        }
    }
}

/* =========================================================
 * Phase 2 — UDP Echo (port 7)
 * ========================================================= */

static void demo_udp_echo(ip4_addr_t target)
{
    klog_info("net_demo: === UDP Echo ===");

    const char* msg      = "AetherOS UDP echo test";
    uint16_t    src_port = 54321;
    uint16_t    dst_port = 7;       /* Well-known echo port */

    /* Register a one-shot listener on src_port */
    int ret = udp_send(target, src_port, dst_port, msg, strlen(msg));
    if (ret < 0) {
        klog_warn("net_demo: udp_send failed");
        return;
    }
    klog_info("net_demo: UDP sent %zu bytes to port %u", strlen(msg), dst_port);

    /* Wait for echo reply */
    char rxbuf[256];
    int  rxlen = udp_recv_blocking(src_port, rxbuf, sizeof(rxbuf) - 1, 2000);
    if (rxlen < 0) {
        klog_warn("net_demo: UDP echo timeout (is QEMU echo server running?)");
        return;
    }

    rxbuf[rxlen] = '\0';
    klog_info("net_demo: UDP echo reply (%d bytes): \"%s\"", rxlen, rxbuf);
}

/* =========================================================
 * Phase 3 — TCP connect, HTTP HEAD, recv
 * ========================================================= */

static void demo_tcp(ip4_addr_t target)
{
    klog_info("net_demo: === TCP Connect ===");

    /* Use the BSD socket layer end-to-end */
    int sd = sock_create(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sd < 0) {
        klog_warn("net_demo: sock_create failed (%d)", sd);
        return;
    }

    sockaddr_in_t addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(80);
    addr.sin_addr   = target;

    klog_info("net_demo: connecting to %u.%u.%u.%u:80 ...",
              (target >>  0) & 0xFF, (target >>  8) & 0xFF,
              (target >> 16) & 0xFF, (target >> 24) & 0xFF);

    int ret = sock_connect(sd, &addr);
    if (ret < 0) {
        klog_warn("net_demo: connect failed (%d) — is there an HTTP server?", ret);
        sock_close(sd);
        return;
    }
    klog_info("net_demo: TCP connected");

    /* Send HTTP/1.0 HEAD request */
    const char* req = "HEAD / HTTP/1.0\r\nHost: 10.0.2.2\r\n\r\n";
    ssize_t sent = sock_send(sd, req, strlen(req), 0);
    klog_info("net_demo: sent %zd bytes", sent);

    /* Receive response (up to 128 bytes) */
    char rxbuf[129];
    memset(rxbuf, 0, sizeof(rxbuf));

    /* Poll for up to 3 seconds */
    uint32_t deadline = timer_get_ticks() + TIMER_FREQ * 3;
    ssize_t  rxlen    = 0;
    while (timer_get_ticks() < deadline && rxlen == 0) {
        if (net_iface.poll) net_iface.poll();
        rxlen = sock_recv(sd, rxbuf, sizeof(rxbuf) - 1, MSG_DONTWAIT);
        if (rxlen == -EAGAIN) rxlen = 0;
    }

    if (rxlen > 0) {
        rxbuf[rxlen] = '\0';
        /* Print only first line (up to '\r' or '\n') */
        for (int i = 0; i < rxlen; i++) {
            if (rxbuf[i] == '\r' || rxbuf[i] == '\n') { rxbuf[i] = '\0'; break; }
        }
        klog_info("net_demo: TCP response: \"%s\"", rxbuf);
    } else {
        klog_warn("net_demo: TCP recv timeout");
    }

    sock_close(sd);
    klog_info("net_demo: TCP socket closed");
}

/* =========================================================
 * Phase 4 — TCP passive open (echo server for 5 s)
 * ========================================================= */

static void demo_tcp_server(void)
{
    klog_info("net_demo: === TCP Echo Server (port 9999, 5-second window) ===");

    int lsd = sock_create(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (lsd < 0) { klog_warn("net_demo: server sock_create failed"); return; }

    sockaddr_in_t addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(9999);
    addr.sin_addr   = 0;   /* INADDR_ANY */

    sock_bind(lsd, &addr);
    sock_listen(lsd, 4);
    klog_info("net_demo: listening on 0.0.0.0:9999 — connect with: nc 10.0.2.15 9999");

    uint32_t deadline = timer_get_ticks() + TIMER_FREQ * 5;
    while (timer_get_ticks() < deadline) {
        if (net_iface.poll) net_iface.poll();

        int csd = sock_accept(lsd, NULL);
        if (csd < 0) continue;   /* accept() returns -ETIMEDOUT after 5 s */

        klog_info("net_demo: accepted connection sd=%d", csd);

        /* Echo everything back */
        char buf[256];
        for (int i = 0; i < 8; i++) {
            ssize_t n = sock_recv(csd, buf, sizeof(buf), MSG_DONTWAIT);
            if (n > 0) {
                sock_send(csd, buf, (size_t)n, 0);
                klog_info("net_demo: echo'd %zd bytes", n);
            }
            if (net_iface.poll) net_iface.poll();
        }
        sock_close(csd);
        klog_info("net_demo: connection closed");
        break;  /* one connection per demo run */
    }

    sock_close(lsd);
    klog_info("net_demo: server done");
}

/* =========================================================
 * Entry point
 * ========================================================= */

void net_demo_run(void)
{
    if (!net_iface.up) {
        klog_warn("net_demo: network interface is down — aborting");
        return;
    }

    klog_info("net_demo: ============================================");
    klog_info("net_demo: AetherOS Network Stack Demo");
    klog_info("net_demo: local IP %u.%u.%u.%u",
              (net_iface.ip >>  0) & 0xFF, (net_iface.ip >>  8) & 0xFF,
              (net_iface.ip >> 16) & 0xFF, (net_iface.ip >> 24) & 0xFF);
    klog_info("net_demo: ============================================");

    /* QEMU user-mode gateway — always reachable */
    ip4_addr_t gw = net_iface.gateway ? net_iface.gateway : IP4(10, 0, 2, 2);

    demo_ping(gw);
    demo_udp_echo(gw);
    demo_tcp(gw);
    demo_tcp_server();

    klog_info("net_demo: all phases complete");
}
