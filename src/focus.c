/*
 * freq-tuner - focus tracking.
 *
 * Receives the WM_CLASS of the currently focused window from the GNOME
 * Shell extension as datagrams on a UNIX DGRAM socket. The socket is world
 * writable so the extension (running in the user session) can reach the
 * daemon.
 *
 * SPDX-License-Identifier: MIT
 */

#define _DEFAULT_SOURCE

#include "focus.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define CLASS_CAP 64

struct ft_focus {
    int         sockfd;
    char        class[CLASS_CAP];
    int         have_class;
    const char *sock_path;
    int         verbose;
};

static void set_class(struct ft_focus *f, const char *cls)
{
    if (cls == NULL)
        cls = "";
    snprintf(f->class, sizeof(f->class), "%s", cls);
    f->have_class = 1;
}

static int bind_socket(struct ft_focus *f)
{
    struct sockaddr_un addr;
    struct stat st;
    size_t n;
    const char *slash;

    if (lstat(f->sock_path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFSOCK)
        (void)unlink(f->sock_path);

    slash = strrchr(f->sock_path, '/');
    if (slash != NULL && slash != f->sock_path) {
        size_t dlen = (size_t)(slash - f->sock_path);
        char dir[512];

        if (dlen >= sizeof(dir))
            return -1;
        memcpy(dir, f->sock_path, dlen);
        dir[dlen] = '\0';
        if (mkdir(dir, 0755) != 0 && errno != EEXIST)
            return -1;
    }

    f->sockfd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (f->sockfd < 0)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    n = strlen(f->sock_path);
    if (n >= sizeof(addr.sun_path)) {
        close(f->sockfd);
        f->sockfd = -1;
        return -1;
    }
    memcpy(addr.sun_path, f->sock_path, n + 1);

    if (bind(f->sockfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(f->sockfd);
        f->sockfd = -1;
        return -1;
    }
    (void)chmod(f->sock_path, 0666);
    return 0;
}

/* ------------------------------------------------------------ public API */

struct ft_focus *ft_focus_init(const char *sock_path, int verbose)
{
    struct ft_focus *f = calloc(1, sizeof(*f));

    if (f == NULL)
        return NULL;
    f->sockfd = -1;
    f->sock_path = sock_path != NULL ? sock_path : "";
    f->verbose = verbose;

    if (f->sock_path[0] != '\0') {
        if (bind_socket(f) == 0) {
            if (verbose)
                fprintf(stderr, "freq-tuner: focus socket %s ready\n",
                        f->sock_path);
        } else {
            fprintf(stderr, "freq-tuner: cannot bind focus socket %s: %s\n",
                    f->sock_path, strerror(errno));
        }
    }
    return f;
}

int ft_focus_fd(struct ft_focus *f)
{
    return f != NULL ? f->sockfd : -1;
}

void ft_focus_handle(struct ft_focus *f)
{
    char buf[CLASS_CAP];
    ssize_t n;

    if (f == NULL || f->sockfd < 0)
        return;
    while ((n = recv(f->sockfd, buf, sizeof(buf) - 1, MSG_DONTWAIT)) >= 0) {
        buf[n] = '\0';
        set_class(f, buf);
    }
}

const char *ft_focus_class(struct ft_focus *f)
{
    if (f == NULL || !f->have_class)
        return "";
    return f->class;
}

void ft_focus_free(struct ft_focus *f)
{
    if (f == NULL)
        return;
    if (f->sockfd >= 0) {
        close(f->sockfd);
        if (f->sock_path[0] != '\0')
            unlink(f->sock_path);
    }
    free(f);
}