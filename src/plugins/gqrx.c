/*
 * freq-tuner - gqrx plugin.
 *
 * Drives gqrx through its remote-control protocol (a hamlib rigctld-style
 * TCP line protocol on localhost:7356 by default):
 *
 *     f          -> "<freq>\n"            current frequency [Hz]
 *     m          -> "<mode>\n<width>\n"   demodulator mode, passband width
 *     F <hz>\n   -> "RPRT 0\n"            set frequency (soft tune)
 *
 * Every tick re-reads the CURRENT frequency from gqrx and then applies a
 * relative change of the CURRENT filter width (the second line of `m`) in
 * coarse mode (the default) or one tenth of it in fine mode. Pushing the
 * knob toggles between the two modes. Nothing is hardcoded or configured:
 * the tuning step always scales with the filter width gqrx is using right
 * now, and the absolute frequency is always taken from gqrx itself, so
 * mouse tuning and mode/filter changes cannot desynchronise the knob.
 *
 * Fail-open: if gqrx cannot be reached, tick() returns 0 so the core lets
 * the event fall through and the knob keeps working as volume.
 *
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE

#include "gqrx.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* line reader buffer for the socket */
#define RBUF_CAP 256
/* poll timeout on socket reads, ms */
#define IO_TIMEOUT_MS 250
/* retry interval when gqrx is unreachable, ms */
#define RETRY_MS 1000
/* refresh interval for the filter width while connected, ms */
#define WIDTH_REFRESH_MS 2000

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000L);
}

struct gqrx {
    int         fd;             /* TCP socket or -1 */
    int         connected;
    long long   freq;           /* last frequency read from gqrx [Hz] */
    long long   width;          /* last filter width read from gqrx [Hz] */
    long long   last_sync;      /* last successful f/m read */
    long long   last_retry;     /* last connect attempt */
    int         active;         /* this plugin currently handles the knob */
    int         coarse;         /* 1 = full filter width, 0 = width/10 */
    char        rbuf[RBUF_CAP]; /* partial-recv buffer for readline */
    size_t      rstart;
    size_t      rused;
    char        host[64];
    int         port;
    int         verbose;
};

static struct gqrx g;

/* ------------------------------------------------------------------ I/O */

static int gqrx_recv(char *buf, size_t cap)
{
    struct timeval tv = { .tv_sec = 0, .tv_usec = IO_TIMEOUT_MS * 1000L };
    fd_set rfds;
    int n, rc;

    FD_ZERO(&rfds);
    FD_SET(g.fd, &rfds);
    rc = select(g.fd + 1, &rfds, NULL, NULL, &tv);
    if (rc <= 0)
        return -1;
    n = (int)recv(g.fd, buf, cap, 0);
    if (n <= 0) {
        g.connected = 0;
        close(g.fd);
        g.fd = -1;
        return -1;
    }
    return n;
}

/* Read one line (without trailing '\n') from the gqrx socket. Returns 0 on
 * error/timeout, 1 on success. Truncates overlong lines. */
static int gqrx_readline(char *line, size_t cap)
{
    size_t used = 0;

    while (used + 1 < cap) {
        char c;

        if (g.rstart < g.rused) {
            c = g.rbuf[g.rstart++];
        } else {
            int n = gqrx_recv(g.rbuf, sizeof(g.rbuf));
            if (n < 0)
                return 0;
            g.rstart = 0;
            g.rused = (size_t)n;
            c = g.rbuf[g.rstart++];
        }
        if (c == '\n') {
            line[used] = '\0';
            return 1;
        }
        line[used++] = c;
    }
    line[used] = '\0';
    return 1;
}

static int gqrx_send(const char *fmt, long long v)
{
    char buf[64];
    int len;

    len = snprintf(buf, sizeof(buf), fmt, v);
    if (len < 0 || (size_t)len >= sizeof(buf))
        return -1;
    if (send(g.fd, buf, (size_t)len, MSG_NOSIGNAL) != len) {
        g.connected = 0;
        close(g.fd);
        g.fd = -1;
        return -1;
    }
    return 0;
}

/* ----------------------------------------------------------------- state */

static int gqrx_connect(void)
{
    struct addrinfo hints, *res = NULL, *ai;
    char port[16];
    int rc, fd = -1;

    if (g.fd >= 0)
        return g.connected ? 0 : -1;

    if (now_ms() - g.last_retry < RETRY_MS)
        return -1;
    g.last_retry = now_ms();

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port, sizeof(port), "%d", g.port);

    rc = getaddrinfo(g.host, port, &hints, &res);
    if (rc != 0)
        return -1;

    for (ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        return -1;

    g.fd = fd;
    g.rstart = 0;
    g.rused = 0;
    g.connected = 1;
    g.last_sync = 0; /* force a fresh f/m read */
    if (g.verbose)
        fprintf(stderr, "freq-tuner[gqrx]: connected to %s:%d\n", g.host, g.port);
    return 0;
}

/* Read current frequency and filter width from gqrx. Returns 0 on failure. */
static int gqrx_sync(void)
{
    char line[64];

    if (gqrx_send("f\n", 0) != 0)
        return 0;
    if (!gqrx_readline(line, sizeof(line)))
        return 0;
    g.freq = atoll(line);

    if (gqrx_send("m\n", 0) != 0)
        return 0;
    if (!gqrx_readline(line, sizeof(line)))   /* mode, ignored */
        return 0;
    if (!gqrx_readline(line, sizeof(line)))   /* passband width */
        return 0;
    g.width = atoll(line);

    g.last_sync = now_ms();
    if (g.verbose)
        fprintf(stderr, "freq-tuner[gqrx]: freq=%lld width=%lld\n", g.freq, g.width);
    return 1;
}

/* --------------------------------------------------------- plugin vtable */

static int gqrx_init(const struct ft_cfg *cfg)
{
    g.fd = -1;
    g.connected = 0;
    g.width = 0;
    g.last_sync = 0;
    g.last_retry = 0;
    g.active = 0;
    g.coarse = 1;
    g.rstart = 0;
    g.rused = 0;
    g.host[0] = '\0';
    g.port = 7356;
    g.verbose = cfg->verbose;

    if (cfg->gqrx_host != NULL) {
        snprintf(g.host, sizeof(g.host), "%s", cfg->gqrx_host);
    } else {
        snprintf(g.host, sizeof(g.host), "%s", "127.0.0.1");
    }
    if (cfg->gqrx_port > 0)
        g.port = cfg->gqrx_port;
    return 0;
}

static void gqrx_focus(int active)
{
    g.active = active != 0;
    if (!g.active)
        return;
    if (gqrx_connect() == 0)
        (void)gqrx_sync();
}

static int gqrx_tick(int dir)
{
    long long target, step;
    char line[64];

    if (gqrx_connect() != 0)
        return 0;
    if (g.last_sync == 0 && !gqrx_sync())
        return 0;
    if (g.width <= 0 && !gqrx_sync())
        return 0;

    /* Always re-read the current frequency so the change is strictly
     * relative to what gqrx is tuned to right now. */
    if (gqrx_send("f\n", 0) != 0)
        return 0;
    if (!gqrx_readline(line, sizeof(line)))
        return 0;
    g.freq = atoll(line);

    step = g.coarse ? g.width : g.width / 10;
    target = g.freq + (long long)dir * step;
    if (gqrx_send("F %lld\n", target) != 0)
        return 0;
    if (!gqrx_readline(line, sizeof(line)))   /* RPRT 0, ignored */
        return 0;
    g.freq = target;
    g.last_sync = now_ms();
    return 1;
}

static int gqrx_press(void)
{
    /* Knob push toggles coarse (full filter width) and fine (width/10)
     * tuning. Always consumed so the press never reaches the system. */
    g.coarse = !g.coarse;
    return 1;
}

static void gqrx_poll(void)
{
    if (g.fd < 0)
        return;
    /* Refresh the filter width so mode/filter changes in gqrx are picked up. */
    if (now_ms() - g.last_sync >= WIDTH_REFRESH_MS)
        (void)gqrx_sync();
}

static void gqrx_deinit(void)
{
    if (g.fd >= 0) {
        close(g.fd);
        g.fd = -1;
    }
}

struct ft_plugin ft_plugin_gqrx = {
    .name = "gqrx",
    .apps = "gqrx",
    .init = gqrx_init,
    .focus = gqrx_focus,
    .tick = gqrx_tick,
    .press = gqrx_press,
    .poll = gqrx_poll,
    .deinit = gqrx_deinit,
};