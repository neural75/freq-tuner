/*
 * freq-tuner - plugin registry and dispatch.
 *
 * All plugins compiled in are always enabled: adding support for another
 * program means adding one .c file under src/plugins/ and one line here.
 *
 * SPDX-License-Identifier: MIT
 */
#include "plugin.h"
#include "plugins/gqrx.h"

#include <string.h>

static struct ft_plugin *const registry[] = {
    &ft_plugin_gqrx,
};

struct ft_plugin *ft_plugin_match(const char *app_class)
{
    size_t i;

    if (app_class == NULL || app_class[0] == '\0')
        return NULL;

    for (i = 0; i < sizeof(registry) / sizeof(registry[0]); i++) {
        struct ft_plugin *p = registry[i];
        const char *s = p->apps;

        while (s != NULL && *s != '\0') {
            const char *comma = strchr(s, ',');
            size_t len = comma != NULL ? (size_t)(comma - s) : strlen(s);

            if (len > 0 && strncmp(s, app_class, len) == 0
                && app_class[len] == '\0')
                return p;
            s = comma != NULL ? comma + 1 : NULL;
        }
    }
    return NULL;
}

void ft_plugin_init_all(const struct ft_cfg *cfg)
{
    size_t i;

    for (i = 0; i < sizeof(registry) / sizeof(registry[0]); i++) {
        struct ft_plugin *p = registry[i];
        if (p->init != NULL)
            p->init(cfg);
    }
}

void ft_plugin_poll_all(void)
{
    size_t i;

    for (i = 0; i < sizeof(registry) / sizeof(registry[0]); i++) {
        struct ft_plugin *p = registry[i];
        if (p->poll != NULL)
            p->poll();
    }
}

void ft_plugin_shutdown_all(void)
{
    size_t i;

    for (i = 0; i < sizeof(registry) / sizeof(registry[0]); i++) {
        struct ft_plugin *p = registry[i];
        if (p->deinit != NULL)
            p->deinit();
    }
}