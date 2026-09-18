/* Sim-only Modbus TCP link + poller task (design 6.3, Task 10). Implements
 * mb_transact_fn over Winsock and runs the link-agnostic poller
 * (src/modbus/modbus_poller.c) exactly as the H757 firmware will over RTU --
 * same schedule, decode and publish code, so pointing this sim at the live
 * Cortex (192.168.7.3:502, READ-ONLY) exercises the real firmware data path.
 *
 * Host/port: HMI_MODBUS_HOST / HMI_MODBUS_PORT env vars, default the
 * emulator's 127.0.0.1:5020. Blocking sockets with select()-based timeouts
 * on every send/recv; reconnects with a fixed backoff on any error or
 * timeout so a dead peer never spins the task. Winsock; sim submodule only. */
#ifdef PRODUCER_MODBUS

#include "FreeRTOS.h"
#include "task.h"

#include "modbus_poller.h"
#include "modbus_tcp.h"
#include "topology.h"
#include "transport.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MB_LINK_DEFAULT_HOST "127.0.0.1"
#define MB_LINK_DEFAULT_PORT 5020u
#define MB_LINK_TIMEOUT_MS   1000u
#define MB_LINK_RECONNECT_BACKOFF_MS 2000u
#define MB_LINK_STATS_PERIOD_MS      5000u

/* MBAP reply cap: 7-byte header + (fc + byte-count + up to MB_MAX_READ_COUNT
 * registers), well under the poller's own RSP_CAP (260) -- mirrored here so
 * this file has no dependency on modbus_poller.c's internals. */
#define MB_LINK_RSP_CAP 260

static SOCKET   g_sock = INVALID_SOCKET;
static uint32_t g_backoff_until_ms;
static char     g_host[128];
static uint16_t g_port;
static int      g_env_loaded = 0;

/* Set by mb_link_transact on every return: 1 on a byte-level success (the
 * poller may still find the reply invalid), 0 on any I/O failure or refused
 * connect. Read by modbus_link_task to pace the loop -- see there. */
static volatile int g_last_transact_ok = 1;

static uint32_t mb_link_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void mb_link_load_env(void)
{
    const char *h, *p;

    if (g_env_loaded) { return; }

    h = getenv("HMI_MODBUS_HOST");
    p = getenv("HMI_MODBUS_PORT");

    strncpy(g_host, (h && *h) ? h : MB_LINK_DEFAULT_HOST, sizeof g_host - 1);
    g_host[sizeof g_host - 1] = '\0';

    g_port = (uint16_t)MB_LINK_DEFAULT_PORT;
    if (p && *p) {
        char *endp = NULL;
        long v = strtol(p, &endp, 10);

        if (endp == p || *endp != '\0' || v < 1 || v > 65535) {
            printf("modbus: bad HMI_MODBUS_PORT '%s', falling back to %u\n",
                   p, (unsigned)MB_LINK_DEFAULT_PORT);
            fflush(stdout);
        } else {
            g_port = (uint16_t)v;
        }
    }

    g_env_loaded = 1;
}

static void mb_link_close(void)
{
    if (g_sock != INVALID_SOCKET) {
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
    }
}

/* Waits up to timeout_ms for `s` to become readable (for_write == 0) or
 * writable (for_write == 1). Returns 1 if ready, 0 on timeout or error. */
static int mb_link_wait_ready(SOCKET s, int for_write, uint32_t timeout_ms)
{
    fd_set fds;
    struct timeval tv;
    int r;

    FD_ZERO(&fds);
    FD_SET(s, &fds);
    tv.tv_sec = (long)(timeout_ms / 1000u);
    tv.tv_usec = (long)((timeout_ms % 1000u) * 1000u);

    r = for_write ? select(0, NULL, &fds, NULL, &tv) : select(0, &fds, NULL, NULL, &tv);
    return r > 0;
}

/* Bounded by an ABSOLUTE deadline (ms, mb_link_now_ms() clock), not a fresh
 * per-call duration -- callers share one deadline across a whole transaction
 * (see mb_link_transact) so the transaction's total wall time is bounded by
 * timeout_ms, not by timeout_ms per phase (review r0 M1). */
static int mb_link_recv_until(SOCKET s, uint8_t *buf, int len, uint32_t deadline_ms)
{
    int got = 0;

    while (got < len) {
        uint32_t now = mb_link_now_ms();
        uint32_t remain;
        int n;

        if ((int32_t)(deadline_ms - now) <= 0) { return -1; }
        remain = deadline_ms - now;

        if (!mb_link_wait_ready(s, 0, remain)) { return -1; }

        n = recv(s, (char *)buf + got, len - got, 0);
        if (n <= 0) { return -1; }
        got += n;
    }
    return got;
}

static int mb_link_send_until(SOCKET s, const uint8_t *buf, int len, uint32_t deadline_ms)
{
    int sent = 0;

    while (sent < len) {
        uint32_t now = mb_link_now_ms();
        uint32_t remain;
        int n;

        if ((int32_t)(deadline_ms - now) <= 0) { return -1; }
        remain = deadline_ms - now;

        if (!mb_link_wait_ready(s, 1, remain)) { return -1; }

        n = send(s, (const char *)buf + sent, len - sent, 0);
        if (n <= 0) { return -1; }
        sent += n;
    }
    return sent;
}

/* Non-blocking connect gated by select() so a dead/unreachable host never
 * blocks the task, then back to blocking mode -- mb_link_recv_until/send_until
 * supply their own select()-based bound on the blocking socket. */
static int mb_link_try_connect(uint32_t timeout_ms)
{
    struct sockaddr_in addr;
    SOCKET s;
    u_long nb;
    int rc;

    mb_link_load_env();

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { return 0; }

    nb = 1;
    ioctlsocket(s, FIONBIO, &nb);

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(g_port);
    if (inet_pton(AF_INET, g_host, &addr.sin_addr) != 1) {
        printf("modbus: bad HMI_MODBUS_HOST '%s'\n", g_host);
        closesocket(s);
        return 0;
    }

    rc = connect(s, (struct sockaddr *)&addr, sizeof addr);
    if (rc == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(s);
        return 0;
    }
    if (rc == SOCKET_ERROR) {
        fd_set wfds, efds;
        struct timeval tv;
        int err = 0;
        int errlen = sizeof err;

        FD_ZERO(&wfds); FD_SET(s, &wfds);
        FD_ZERO(&efds); FD_SET(s, &efds);
        tv.tv_sec = (long)(timeout_ms / 1000u);
        tv.tv_usec = (long)((timeout_ms % 1000u) * 1000u);

        if (select(0, NULL, &wfds, &efds, &tv) <= 0 || FD_ISSET(s, &efds)) {
            closesocket(s);
            return 0;
        }
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &errlen) != 0 || err != 0) {
            closesocket(s);
            return 0;
        }
    }

    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);

    {
        int yes = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&yes, sizeof yes);
    }

    g_sock = s;
    printf("modbus: connected to %s:%u\n", g_host, (unsigned)g_port);
    fflush(stdout);
    return 1;
}

/* Reconnect with a fixed backoff -- a dead peer costs one failed connect()
 * per MB_LINK_RECONNECT_BACKOFF_MS, never a spin. */
static int mb_link_ensure_connected(uint32_t timeout_ms)
{
    uint32_t now;

    if (g_sock != INVALID_SOCKET) { return 1; }

    now = mb_link_now_ms();
    if ((int32_t)(now - g_backoff_until_ms) < 0) { return 0; }

    if (!mb_link_try_connect(timeout_ms)) {
        printf("modbus: connect to %s:%u failed, retrying in %u ms\n",
               g_host, (unsigned)g_port, (unsigned)MB_LINK_RECONNECT_BACKOFF_MS);
        fflush(stdout);
        g_backoff_until_ms = mb_link_now_ms() + MB_LINK_RECONNECT_BACKOFF_MS;
        return 0;
    }
    return 1;
}

/* mb_link_t.drain: called by the poller after any non-OK transaction (timeout,
 * short, CRC, unit, fc, count, TCP tid, exception) before the next request.
 * TCP has no "flush unread bytes" short of closing -- a late/mismatched reply
 * left on the wire would otherwise be misread as the answer to a later,
 * unrelated request, one transaction behind, forever (modbus_poller.h's
 * MB_ERR_TID note). Closing also covers a timeout inside transact() itself,
 * which already closes before returning -1 (below) for the same reason. */
static void mb_link_drain(void *ctx)
{
    (void)ctx;
    mb_link_close();
}

/* mb_transact_fn: send req, read one complete MBAP reply (7-byte header,
 * then length-1 more bytes), reconnecting on any error or timeout. The
 * poller built req itself (tid included via mb_tcp_build_read); this link
 * only moves bytes. Every failure path closes the socket first -- a timed-
 * out or short reply must never be left for the next transact() to read as
 * its answer.
 *
 * timeout_ms is the WHOLE transaction's budget: one deadline is computed
 * once connected and shared across send, header recv and body recv, so a
 * peer that trickles bytes across all three phases cannot hold the call for
 * up to 3x timeout_ms (review r0 M1) -- connecting itself (rare: only on the
 * first call or after a failure) keeps its own full timeout_ms budget in
 * mb_link_ensure_connected, separate from this deadline. */
static int mb_link_transact(void *ctx, const uint8_t *req, int req_len,
                             uint8_t *rsp, int rsp_cap, uint32_t timeout_ms)
{
    uint32_t deadline;
    uint8_t hdr[7];
    uint16_t mbap_len;
    int rest;

    (void)ctx;

    if (!mb_link_ensure_connected(timeout_ms)) {
        g_last_transact_ok = 0;
        return -1;
    }

    deadline = mb_link_now_ms() + timeout_ms;

    if (mb_link_send_until(g_sock, req, req_len, deadline) < 0) {
        mb_link_close();
        g_last_transact_ok = 0;
        return -1;
    }
    if (mb_link_recv_until(g_sock, hdr, 7, deadline) < 0) {
        mb_link_close();
        g_last_transact_ok = 0;
        return -1;
    }

    mbap_len = (uint16_t)(((uint16_t)hdr[4] << 8) | hdr[5]);
    if (mbap_len < 1) {
        mb_link_close();
        g_last_transact_ok = 0;
        return -1;
    }
    rest = (int)mbap_len - 1;
    if (7 + rest > rsp_cap || 7 + rest > MB_LINK_RSP_CAP) {
        mb_link_close();
        g_last_transact_ok = 0;
        return -1;
    }

    memcpy(rsp, hdr, 7);
    if (rest > 0 && mb_link_recv_until(g_sock, rsp + 7, rest, deadline) < 0) {
        mb_link_close();
        g_last_transact_ok = 0;
        return -1;
    }

    g_last_transact_ok = 1;
    return 7 + rest;
}

static const char *mb_link_topo_status_name(topo_status_t s)
{
    switch (s) {
    case TOPO_NOT_READ:           return "NOT_READ";
    case TOPO_OK:                 return "OK";
    case TOPO_UNKNOWN_PANEL_TYPE: return "UNKNOWN_PANEL_TYPE";
    case TOPO_BAD_CONFIG:         return "BAD_CONFIG";
    default:                      return "?";
    }
}

/* A step whose transact failed (link down/refused, backing off) returns in
 * microseconds -- without a floor here that busy-spins the task at 100% of
 * one core against a dead peer. */
#define MB_LINK_DOWN_STEP_DELAY_MS 100u
#define MB_LINK_UP_STEP_DELAY_MS     1u

/* FreeRTOS producer task: runs the link-agnostic poller one transaction per
 * step, with a yield between steps (short while the link is up, >= 100 ms
 * while it is down so a refused/dead peer never busy-spins), and logs an
 * H757-console-format stats line every 5 s. */
void modbus_link_task(void *pv)
{
    WSADATA wsa;
    mb_link_t link;
    uint32_t last_stats_ms;

    (void)pv;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("modbus: WSAStartup failed\n");
        vTaskDelete(NULL);
        return;
    }

    mb_link_load_env();
    printf("modbus: target %s:%u\n", g_host, (unsigned)g_port);
    fflush(stdout);

    link.fn = mb_link_transact;
    link.ctx = NULL;
    link.unit = 1;
    link.is_tcp = 1;
    link.timeout_ms = MB_LINK_TIMEOUT_MS;
    link.now_ms = mb_link_now_ms;
    link.drain = mb_link_drain;

    if (!mb_poller_init(&link, MODBUS_MAP_CTX, MODBUS_MAP_CTX_N)) {
        /* Refused (bad map/link, never a runtime condition here) -- every
         * mb_poller_step() call from here on returns MB_STEP_IDLE, so the
         * loop below backs off at MB_LINK_DOWN_STEP_DELAY_MS forever rather
         * than spinning. Logged once so an idle producer is diagnosable
         * instead of looking like a silently dead task. */
        printf("modbus: mb_poller_init refused the map -- producer stays idle\n");
        fflush(stdout);
    }

    last_stats_ms = mb_link_now_ms();

    for (;;) {
        int step = mb_poller_step();

        {
            uint32_t now = mb_link_now_ms();
            if (now - last_stats_ms >= MB_LINK_STATS_PERIOD_MS) {
                mb_stats_t st;
                bcms_topology_t topo;

                mb_poller_stats(&st);
                bcms_topology_get(&topo);

                printf("MB cyc=%lu ms=%lu/%lu ok=%lu to=%lu crc=%lu exc=%lu nan=%lu cfg=%lu ovr=%lu topo=%s/%u%s\n",
                       (unsigned long)st.cycles, (unsigned long)st.cycle_ms_last, (unsigned long)st.cycle_ms_max,
                       (unsigned long)st.ok, (unsigned long)st.timeout, (unsigned long)st.crc,
                       (unsigned long)st.exc, (unsigned long)st.nan, (unsigned long)st.cfg_changes,
                       (unsigned long)st.overruns,
                       mb_link_topo_status_name(topo.status), (unsigned)topo.circuits,
                       topo.standard_branch_unverified ? "/unv" : "");
                fflush(stdout);
                last_stats_ms = now;
            }
        }

        /* MB_STEP_IDLE (compared explicitly, never `if (mb_poller_step())`
         * -- MB_STEP_IDLE is -1, a true C value) means nothing ran at all
         * (not initialised, or init was refused): back off >= 100 ms so a
         * permanently idle poller never busy-spins. A transaction that DID
         * run (MB_STEP_TXN or MB_STEP_CYCLE) paces on whether it actually
         * succeeded, same as before. */
        if (step == MB_STEP_IDLE) {
            vTaskDelay(pdMS_TO_TICKS(MB_LINK_DOWN_STEP_DELAY_MS));
        } else {
            vTaskDelay(pdMS_TO_TICKS(g_last_transact_ok ? MB_LINK_UP_STEP_DELAY_MS
                                                         : MB_LINK_DOWN_STEP_DELAY_MS));
        }
    }
}

#endif /* PRODUCER_MODBUS */
