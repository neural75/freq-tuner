/*
 * freq-tuner - rotary input router for per-application tuning.
 *
 * Sits in an interception-tools pipeline:
 *
 *     interception -g $DEVICE | freq-tuner [opts] | uinput -d $DEVICE
 *
 * It reads raw evdev events on stdin and writes them to stdout. Rotary
 * ticks (volume keys and/or wheel relative axes) are routed to the plugin
 * matching the focused application: while that app is focused the events
 * are consumed and the plugin acts instead; otherwise everything passes
 * through unchanged, so the knob keeps its normal behaviour.
 *
 * Fail-open: if focus cannot be determined or the plugin cannot act, the
 * event falls through to the virtual device.
 *
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE

#include "focus.h"
#include "plugin.h"

#include <errno.h>
#include <getopt.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PROG        "freq-tuner"
#define VERSION     "0.1"

#define EV_CAP      256u

#define LOOP_TIMEOUT_MS   250
#define PLUGIN_POLL_MS    1000

static unsigned char    evbuf[EV_CAP];
static size_t           evlen;

static uint8_t          swallow[KEY_CNT];
static struct ft_plugin *active;

static int              verbose;
static volatile sig_atomic_t stop_requested;

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000L);
}

static int write_all(const void *buf, size_t n)
{
    const unsigned char *p = buf;

    while (n > 0) {
        ssize_t w = write(STDOUT_FILENO, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* ------------------------------------------------------------- routing */

static void update_active(struct ft_focus *focus)
{
    struct ft_plugin *p = ft_plugin_match(ft_focus_class(focus));

    if (p != active) {
        if (active != NULL)
            active->focus(0);
        if (p != NULL)
            p->focus(1);
        active = p;
    }
}

static void handle_event(const struct input_event *ev)
{
    unsigned int code = ev->code;

    if (ev->type == EV_KEY && (code == KEY_VOLUMEUP || code == KEY_VOLUMEDOWN)) {
        if (active != NULL) {
            int dir = code == KEY_VOLUMEUP ? 1 : -1;

            if (ev->value == 1) {
                if (active->tick(dir)) {
                    swallow[code] = 1;
                } else {
                    (void)write_all(ev, sizeof(*ev));   /* fail-open */
                }
            } else if (ev->value == 2 && swallow[code]) {
                (void)active->tick(dir);
            } else if (ev->value == 0) {
                if (swallow[code])
                    swallow[code] = 0;
                else
                    (void)write_all(ev, sizeof(*ev));
            } else {
                (void)write_all(ev, sizeof(*ev));
            }
        } else {
            (void)write_all(ev, sizeof(*ev));
        }
        return;
    }

    if (ev->type == EV_KEY && code == KEY_MUTE) {
        if (active != NULL) {
            if (ev->value == 1) {
                if (active->press())
                    swallow[code] = 1;
                else
                    (void)write_all(ev, sizeof(*ev));
            } else if (ev->value == 2 && swallow[code]) {
                /* autorepeat of a swallowed press: keep it swallowed */
            } else if (ev->value == 0) {
                if (swallow[code])
                    swallow[code] = 0;
                else
                    (void)write_all(ev, sizeof(*ev));
            } else {
                (void)write_all(ev, sizeof(*ev));
            }
        } else {
            (void)write_all(ev, sizeof(*ev));
        }
        return;
    }

    if (ev->type == EV_REL &&
        (code == REL_WHEEL || code == REL_HWHEEL || code == REL_DIAL)) {
        if (active != NULL && ev->value != 0) {
            if (active->tick(ev->value > 0 ? 1 : -1))
                return;                     /* consumed */
        }
        (void)write_all(ev, sizeof(*ev));
        return;
    }

    (void)write_all(ev, sizeof(*ev));
}

/* -------------------------------------------------------- stdin reader */

static void process_events(void)
{
    while (evlen >= sizeof(struct input_event)) {
        struct input_event ev;

        memcpy(&ev, evbuf, sizeof(ev));
        handle_event(&ev);
        memmove(evbuf, evbuf + sizeof(ev), evlen - sizeof(ev));
        evlen -= sizeof(ev);
    }
}

static void read_stdin(void)
{
    ssize_t n = read(STDIN_FILENO, evbuf + evlen, sizeof(evbuf) - evlen);

    if (n < 0) {
        if (errno == EINTR)
            return;
        perror(PROG ": read(stdin)");
        exit(1);
    }
    if (n == 0) {
        /* upstream (interception) closed the pipe */
        stop_requested = 1;
        return;
    }
    evlen += (size_t)n;
    process_events();
}

/* ------------------------------------------------------------------ main */

static void handler_stop(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static void usage(FILE *out)
{
    fprintf(out,
        "usage: " PROG " [options]\n"
        "\n"
        "Reads evdev events on stdin and writes them to stdout, routing\n"
        "rotary ticks to the plugin matching the focused application.\n"
        "Run inside an interception pipeline:\n"
        "  interception -g $DEV | " PROG " | uinput -d $DEV\n"
        "\n"
        "options:\n"
        "  --gqrx-host <h>     gqrx remote-control host  (default 127.0.0.1)\n"
        "  --gqrx-port <p>     gqrx remote-control port  (default 7356)\n"
        "  --focus-sock <p>    focus socket path         (default /run/freq-tuner/focus.sock)\n"
        "  --verbose           log to stderr\n"
        "  --version           print version\n"
        "  -h, --help          this help\n");
}

static void parse_args(int argc, char **argv, struct ft_cfg *cfg,
                       const char **sock_path)
{
    static const struct option longopts[] = {
        { "gqrx-host", required_argument, NULL, 1 },
        { "gqrx-port", required_argument, NULL, 2 },
        { "focus-sock", required_argument, NULL, 3 },
        { "verbose",   no_argument,       NULL, 6 },
        { "version",   no_argument,       NULL, 7 },
        { "help",      no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    int c;

    cfg->gqrx_host = "127.0.0.1";
    cfg->gqrx_port = 7356;
    *sock_path = "/run/freq-tuner/focus.sock";

    while ((c = getopt_long(argc, argv, "h", longopts, NULL)) != -1) {
        switch (c) {
        case 1: cfg->gqrx_host = optarg; break;
        case 2: cfg->gqrx_port = atoi(optarg); break;
        case 3: *sock_path = optarg; break;
        case 6: verbose = 1; break;
        case 7:
            printf(PROG " " VERSION "\n");
            exit(0);
        case 'h':
        default:
            usage(c == 'h' ? stdout : stderr);
            exit(c == 'h' ? 0 : 1);
        }
    }
}

int main(int argc, char **argv)
{
    struct ft_cfg cfg;
    struct ft_focus *focus;
    struct pollfd fds[2];
    int nfds;
    const char *sock_path;
    long long last_plugin_poll = 0;

    memset(&cfg, 0, sizeof(cfg));
    memset(swallow, 0, sizeof(swallow));
    parse_args(argc, argv, &cfg, &sock_path);
    cfg.verbose = verbose;

    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, handler_stop);
    signal(SIGINT, handler_stop);

    ft_plugin_init_all(&cfg);

    focus = ft_focus_init(sock_path, verbose);
    if (focus == NULL) {
        fprintf(stderr, PROG ": focus module failed to start\n");
        return 1;
    }

    if (verbose)
        fprintf(stderr, PROG " " VERSION ": started\n");

    fds[0].fd = STDIN_FILENO;
    fds[0].events = POLLIN;
    fds[1].fd = ft_focus_fd(focus);
    fds[1].events = POLLIN;
    nfds = fds[1].fd >= 0 ? 2 : 1;

    update_active(focus);

    while (!stop_requested) {
        int rc;

        rc = poll(fds, (nfds_t)nfds, LOOP_TIMEOUT_MS);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            perror(PROG ": poll");
            break;
        }

        if (fds[0].revents & POLLIN)
            read_stdin();
        if (fds[1].fd >= 0 && (fds[1].revents & POLLIN))
            ft_focus_handle(focus);
        if (fds[1].fd >= 0 && (fds[1].revents & (POLLHUP | POLLERR)))
            fds[1].fd = -1;

        update_active(focus);

        if (now_ms() - last_plugin_poll >= PLUGIN_POLL_MS) {
            ft_plugin_poll_all();
            last_plugin_poll = now_ms();
        }
    }

    ft_focus_free(focus);
    return 0;
}