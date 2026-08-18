/*
 * freq-tuner - plugin interface.
 *
 * The core routes rotary ticks to whichever plugin matches the currently
 * focused application. Every plugin compiled into the registry is always
 * active; matching is done on the window class of the focused app.
 *
 * A plugin must not block for a long time: tick()/press() run in the
 * main event loop. Return 0 from tick()/press() to let the event fall
 * through to the virtual device (fail-open), or nonzero to consume it.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef FT_PLUGIN_H
#define FT_PLUGIN_H

#include <stddef.h>

/* Runtime settings handed to every plugin at init(). */
struct ft_cfg {
    const char *gqrx_host;  /* gqrx plugin: remote-control host  */
    int         gqrx_port;  /* gqrx plugin: remote-control port  */
    int         verbose;
};

struct ft_plugin {
    const char *name;   /* short id, e.g. "gqrx"                       */
    const char *apps;   /* comma-separated WM_CLASS list it reacts to  */

    /* Called once at startup. */
    int (*init)(const struct ft_cfg *cfg);
    /* Notified when this plugin's app gains (active != 0) or loses focus. */
    void (*focus)(int active);
    /* A rotary tick. dir is +1 (up) or -1 (down). Return nonzero if handled. */
    int (*tick)(int dir);
    /* Knob push while this plugin's app is focused. Return nonzero if handled. */
    int (*press)(void);
    /* Periodic maintenance (every ~1 s), may be NULL. */
    void (*poll)(void);
    /* Called once at shutdown. */
    void (*deinit)(void);
};

/* Registry / dispatch helpers (plugin.c). */
void ft_plugin_init_all(const struct ft_cfg *cfg);
struct ft_plugin *ft_plugin_match(const char *app_class);
void ft_plugin_poll_all(void);
void ft_plugin_shutdown_all(void);

#endif /* FT_PLUGIN_H */