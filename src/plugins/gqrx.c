/*
 * freq-tuner - gqrx plugin.
 *
 * Drives gqrx through its remote-control protocol (a hamlib rigctld-style
 * TCP line protocol on localhost:7356 by default):
 *
 *     f          -> "<freq>\n"            current frequency [Hz]
 *     m          -> "<mode>\n<width>\n"   demodulator mode, passband width
 *     F <hz>\n   -> "RPRT 0\n"            set frequency (soft tune)
 *     q          -> (closes the connection)
 *
 * gqrx answers each command exactly once (f: 1 line, m: 2 lines, F: 1 line)
 * and never pushes data unsolicited, so the plugin tracks the number of
 * replies it has outstanding (g.pend). Every successful readline consumes one
 * pending reply; if a reply that was counted on never arrives, the connection
 * is abandoned and rebuilt, so a slow/stale reply can never be mis-read as the
 * answer to a later command (which used to send the F command for a stale
 * frequency and made the tuning jump erratically).
 *
 * Every tick re-reads the CURRENT frequency from gqrx and then applies a
 * relative change of the CURRENT filter width (the second line of `m`) in
 * coarse mode (the default) or one tenth of it in fine mode. Pushing the
 * knob toggles between the two modes. Nothing is hardcoded or configured:
 * the tuning step always scales with the filter width gqrx is using right
 * now, and the absolute frequency is always taken from gqrx itself, so
 * mouse tuning and mode/filter changes cannot desynchronise the knob.
 *
 * Fail-open is LIMITED to "gqrx cannot even be reached": then tick() returns
 * 0, the core lets the event fall through and the knob keeps working as
 * volume. Once a remote-control session exists a notch is always consumed
 * (never leaked to the system volume) even if the radio is busy or returns a
 * bogus reading, and no F command is sent for a frequency/target that is not
 * a valid positive number - gqrx's F handler has no range check and would
 * otherwise lock the receiver onto 0 Hz (rc_freq is reported as 0 right after
 * gqrx starts, before the SDR reports a carrier).
 *
 * The connection is established on demand (first knob turn after focus) and
 * closed gracefully with `q` after IDLE_DISCONNECT_MS without tuning, because
 * gqrx's stop_server() corrupts its own client list whenever a connection
 * exists at the moment remote control is toggled off (or gqrx quits).
 *
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE

#include "gqrx.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* line reader buffer for the socket */
#define RBUF_CAP 256
/* poll timeout on socket reads, ms */
#define IO_TIMEOUT_MS 250
/* timeout for a non-blocking connect, ms */
#define CONNECT_TIMEOUT_MS 250
/* retry interval when gqrx is unreachable, ms */
#define RETRY_MS 1000
/* refresh interval for the filter width while connected, ms */
#define WIDTH_REFRESH_MS 2000
/* disconnect after this many ms without tuning. gqrx's stop_server() corrupts
 * its own client list whenever a connection exists at the moment remote
 * control is toggled off (or gqrx quits); leaving the client list while idle
 * (via the graceful `q` command) keeps the toggle crash-free. */
#define IDLE_DISCONNECT_MS 3000

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000L);
}

struct gqrx {
    int         fd;             /* TCP socket or -1 */
    int         connected;
    int         pend;           /* replies expected but not yet read */
    int         warned;         /* wrote the "invalid frequency" warning */
    long long   freq;           /* last frequency read from gqrx [Hz] */
    long long   width;          /* last filter width read from gqrx [Hz] */
    long long   last_sync;      /* last successful f/m read */
    long long   last_tune;      /* last knob activity handled by this plugin */
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

/* Drop the socket and forget everything in flight. A fresh connection has an
 * aligned request/reply stream, so this is the self-healing path: it is also
 * used when a counted-on reply fails to arrive in time. */
static void gqrx_reset(void)
{
    if (g.fd >= 0)
        close(g.fd);
    g.fd = -1;
    g.connected = 0;
    g.pend = 0;
    /* allow an immediate reconnect on the next knob turn */
    g.last_retry = 0;
}

static int gqrx_recv(char *buf, size_t cap)
{
    struct timeval tv = { .tv_sec = 0, .tv_usec = IO_TIMEOUT_MS * 1000L };
    fd_set rfds;
    int n, rc;

    FD_ZERO(&rfds);
    FD_SET(g.fd, &rfds);
    rc = select(g.fd + 1, &rfds, NULL, NULL, &tv);
    if (rc <= 0)
        return -1;              /* timeout or select error */
    n = (int)recv(g.fd, buf, cap, 0);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return -1;
    if (n <= 0) {
        if (g.connected)
            fprintf(stderr, "freq-tuner[gqrx]: connection lost (peer closed socket)\n");
        gqrx_reset();
        return -1;
    }
    return n;
}

/* Read one line (without trailing '\n') from the gqrx socket. Returns 0 on
 * error/timeout, 1 on success. Truncates overlong lines. A timeout while a
 * reply is expected (g.pend > 0) is treated as a stream corruption and the
 * connection is dropped so a late reply can never misalign a later command. */
static int gqrx_readline(char *line, size_t cap)
{
    size_t used = 0;

    while (used + 1 < cap) {
        char c;

        if (g.rstart < g.rused) {
            c = g.rbuf[g.rstart++];
        } else {
            int n = gqrx_recv(g.rbuf, sizeof(g.rbuf));
            if (n < 0) {
                if (g.connected && g.pend > 0) {
                    fprintf(stderr, "freq-tuner[gqrx]: protocol read timeout"
                            " - resetting connection\n");
                    gqrx_reset();
                }
                return 0;
            }
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

/* readline that also accounts for one outstanding reply. */
static int gqrx_rdline(char *line, size_t cap)
{
    int ok = gqrx_readline(line, cap);

    if (ok && g.pend > 0)
        g.pend--;
    return ok;
}

static int gqrx_send(const char *fmt, long long v)
{
    char buf[64];
    int len;

    len = snprintf(buf, sizeof(buf), fmt, v);
    if (len < 0 || (size_t)len >= sizeof(buf))
        return -1;
    if (send(g.fd, buf, (size_t)len, MSG_NOSIGNAL) != len) {
        if (g.connected)
            fprintf(stderr, "freq-tuner[gqrx]: connection lost (write failed: %s)\n",
                    strerror(errno));
        gqrx_reset();
        return -1;
    }
    return 0;
}

/* ----------------------------------------------------------------- state */

static int gqrx_connect(void)
{
    struct addrinfo hints, *res = NULL, *ai;
    char port[16];
    int rc, fd = -1, soerr, s;
    socklen_t soerr_len = sizeof(soerr);
    struct timeval tv;
    fd_set wfds;

    if (g.fd >= 0)
        return g.connected ? 0 : -1;

    /* Only slow down when gqrx is actually unreachable; a lost connection is
     * reset with last_retry = 0 so the next turn reconnects immediately. */
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
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK,
                    ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        if (errno == EINPROGRESS) {
            /* wait, but never longer than CONNECT_TIMEOUT_MS */
            tv.tv_sec = CONNECT_TIMEOUT_MS / 1000;
            tv.tv_usec = (CONNECT_TIMEOUT_MS % 1000) * 1000L;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            rc = select(fd + 1, NULL, &wfds, NULL, &tv);
            if (rc > 0 && FD_ISSET(fd, &wfds)
                && getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &soerr_len) == 0
                && soerr == 0)
                break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        return -1;

    /* keep the socket non-blocking from here on */
    s = fcntl(fd, F_SETFL, O_NONBLOCK);
    if (s < 0) {
        close(fd);
        return -1;
    }

    g.fd = fd;
    g.rstart = 0;
    g.rused = 0;
    g.pend = 0;
    g.warned = 0;
    g.connected = 1;
    g.last_sync = 0; /* force a fresh f/m read */
    fprintf(stderr, "freq-tuner[gqrx]: connected to %s:%d\n", g.host, g.port);
    return 0;
}

/* Read current frequency and filter width from gqrx. Returns 0 on failure. */
static int gqrx_sync(void)
{
    char line[64];

    if (gqrx_send("f\n", 0) != 0)
        return 0;
    g.pend++;
    if (!gqrx_rdline(line, sizeof(line)))
        return 0;
    g.freq = atoll(line);

    if (gqrx_send("m\n", 0) != 0)
        return 0;
    g.pend += 2;
    if (!gqrx_rdline(line, sizeof(line)))   /* mode, ignored */
        return 0;
    if (!gqrx_rdline(line, sizeof(line)))   /* passband width */
        return 0;
    g.width = atoll(line);

    g.warned = 0;
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
    g.pend = 0;
    g.warned = 0;
    g.width = 0;
    g.last_sync = 0;
    g.last_tune = 0;
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
    /* Focus alone never opens (or re-arms) the connection: only knob
     * activity does. This keeps the socket closed while gqrx is merely
     * focused (so toggling remote control off is safe) and prevents focus
     * changes from cycling connect/idle forever. */
    g.active = active != 0;
}

static int gqrx_tick(int dir)
{
    long long target, step;
    char line[64];

    if (g.fd < 0) {
        /* Reconnect on demand: after the idle disconnect a fresh burst of
         * knob turns brings the connection back (bounded, non-blocking). */
        if (gqrx_connect() != 0)
            return 0;   /* genuinely unreachable: leave the knob as volume */
    }
    g.last_tune = now_ms();

    /* From here on the motion belongs to gqrx: every notch is consumed so
     * dialing the radio never bleeds into the system volume, and a broken or
     * bogus session is simply dropped and re-established on the next turn. */

    if (g.last_sync == 0) {
        if (!gqrx_sync())
            return 1;
    }

    if (g.width <= 0) {
        /* cannot compute a tuning step (e.g. demod muted or filter unknown) */
        if (g.verbose)
            fprintf(stderr, "freq-tuner[gqrx]: no filter width - not tuning\n");
        return 1;
    }

    /* Always re-read the current frequency so the change is strictly
     * relative to what gqrx is tuned to right now. */
    if (gqrx_send("f\n", 0) != 0)
        return 1;
    g.pend++;
    if (!gqrx_rdline(line, sizeof(line)))
        return 1;
    g.freq = atoll(line);

    /* gqrx's F handler accepts any value (no range check), so tuning on a
     * bogus reading would lock the receiver to 0 Hz. If frequency or target
     * is not a valid positive number, consume the notch without sending
     * anything. Invalid readings happen right after gqrx starts (rc_freq is
     * 0 until the SDR reports a carrier). */
    if (g.freq <= 0) {
        if (!g.warned) {
            g.warned = 1;
            fprintf(stderr, "freq-tuner[gqrx]: gqrx reports invalid frequency"
                    " %lld - not tuning\n", g.freq);
        }
        return 1;
    }

    step = g.coarse ? g.width : g.width / 10;
    target = g.freq + (long long)dir * step;
    if (target <= 0) {
        if (g.verbose)
            fprintf(stderr, "freq-tuner[gqrx]: target %lld <= 0 - not tuning\n", target);
        return 1;
    }

    if (gqrx_send("F %lld\n", target) != 0)
        return 1;
    g.pend++;
    if (!gqrx_rdline(line, sizeof(line)))   /* RPRT 0, ignored */
        return 1;
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
    char peek;
    ssize_t n;

    if (g.fd < 0) {
        /* Reconnect promptly after an unexpected loss while still inside a
         * tuning burst, but never reconnect on our own once idle: the idle
         * disconnect left gqrx's client list intentionally and gqrx being
         * stopped is a normal state to fail open in. */
        if (g.last_tune != 0 &&
            now_ms() - g.last_tune < IDLE_DISCONNECT_MS &&
            now_ms() - g.last_retry >= RETRY_MS) {
            g.last_retry = now_ms();
            (void)gqrx_connect();
        }
        return;
    }

    /* Prompt EOF detection: a closed peer makes the socket readable right
     * away. Peek without consuming so pending responses stay buffered for
     * gqrx_readline, and this runs every poll (~1 s), not only every sync. */
    n = recv(g.fd, &peek, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        if (g.connected)
            fprintf(stderr, "freq-tuner[gqrx]: connection lost (peer closed socket)%s\n",
                    n == 0 ? "" : ": recv error");
        gqrx_reset();
        return;
    }

    /* Graceful idle disconnect so a remote-control toggle (or gqrx quit)
     * outside a tuning burst hits gqrx with an empty client list. */
    if (g.last_tune != 0 && now_ms() - g.last_tune >= IDLE_DISCONNECT_MS) {
        fprintf(stderr, "freq-tuner[gqrx]: idle - closing connection gracefully\n");
        (void)gqrx_send("q\n", 0);   /* q has no reply: gqrx closes the socket */
        gqrx_reset();
        return;
    }

    /* Refresh the filter width so mode/filter changes in gqrx are picked up. */
    if (now_ms() - g.last_sync >= WIDTH_REFRESH_MS)
        (void)gqrx_sync();
}

static void gqrx_deinit(void)
{
    gqrx_reset();
}

struct ft_plugin ft_plugin_gqrx = {
    .name = "gqrx",
    .apps = "gqrx,dk.gqrx.gqrx",
    .init = gqrx_init,
    .focus = gqrx_focus,
    .tick = gqrx_tick,
    .press = gqrx_press,
    .poll = gqrx_poll,
    .deinit = gqrx_deinit,
};