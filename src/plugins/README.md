# Writing a plugin

`freq-tuner` drives an SDR application through a small plugin interface
(`src/plugin.h`). A plugin turns rotary ticks into a command for a specific
program. Only **gqrx** ships today, but any other SDR program that can be
driven from the command line or over the network can be added the same way.
Several plugins can be compiled in at once; the one matching the focused
window handles the knob.

The interface is deliberately small: a plugin declares its name, the
applications it reacts to, and up to six callbacks.

## The interface

```c
struct ft_cfg {
    const char *gqrx_host;  /* gqrx plugin: remote-control host  */
    int         gqrx_port;  /* gqrx plugin: remote-control port  */
    int         verbose;    /* log to stderr when set            */
};

struct ft_plugin {
    const char *name;   /* short id, e.g. "gqrx"                      */
    const char *apps;   /* comma-separated WM_CLASS list it reacts to */

    int  (*init)(const struct ft_cfg *cfg);   /* once at startup      */
    void (*focus)(int active);                /* focus gained/lost    */
    int  (*tick)(int dir);                    /* dir: +1 or -1        */
    int  (*press)(void);                      /* knob push            */
    void (*poll)(void);                       /* every ~1 s, may be NULL */
    void (*deinit)(void);                     /* once at shutdown     */
};
```

- **`init`** — called once at startup with the runtime configuration. Use it
  to reset state and copy whatever the plugin needs out of `cfg`. Return 0 on
  success.
- **`focus`** — called with `active != 0` when one of the plugin's apps gains
  focus, and with `0` when it loses it. Good place to open a connection or
  refresh state.
- **`tick`** — a rotary tick: `dir` is `+1` (up) or `-1` (down). Return
  **nonzero to consume** the event (the knob does not reach the system), or
  **0 to let it fall through** (the knob keeps behaving normally). This is the
  fail-open rule: if the plugin cannot act, return 0.
- **`press`** — the knob is pushed while one of the plugin's apps is focused.
  Same return contract as `tick`.
- **`poll`** — called roughly once a second for background maintenance (e.g.
  refreshing a value). May be `NULL`.
- **`deinit`** — called once at shutdown; free resources, close sockets.

Every callback runs inside the main event loop, so none of them may block for
a long time. Do not call the plugin registry functions from inside a plugin.

## Matching

A plugin reacts to the focused window through its `WM_CLASS`. The `apps`
string is a comma-separated list of window classes; when the focused
application matches, the core sets this plugin active and the knob starts
driving it. The gqrx plugin, for example, matches both the flatpak
(`dk.gqrx.gqrx`) and the regular build (`gqrx`):

```c
.apps = "gqrx,dk.gqrx.gqrx",
```

To find the class of an application, run it, focus it, and check the class
the focus watcher reports (or use `xprop`/`xdotool getactivewindow
getwindowclassname` on an X11 session).

## Adding a plugin, step by step

1. **Create `src/plugins/<name>.c`** — a `.c` file with a header
   `src/plugins/<name>.h` declaring `extern struct ft_plugin ft_plugin_<name>;`.
   Copy `gqrx.c` for a full working example.

2. **Implement the callbacks and a vtable** at the bottom of the file:

   ```c
   struct ft_plugin ft_plugin_<name> = {
       .name   = "<name>",
       .apps   = "com.example.app",
       .init   = <name>_init,
       .focus  = <name>_focus,
       .tick   = <name>_tick,
       .press  = <name>_press,
       .poll   = <name>_poll,      /* optional, may be omitted */
       .deinit = <name>_deinit,
   };
   ```

3. **Register it in `src/plugin.c`** — include the new header and add the
   plugin to the registry array:

   ```c
   #include "plugins/<name>.h"

   static struct ft_plugin *const registry[] = {
       &ft_plugin_gqrx,
       &ft_plugin_<name>,
   };
   ```

4. **Add it to the build** — in `Makefile`, add the new source file to `SRC`:

   ```make
   SRC := src/main.c src/plugin.c src/focus.c \
          src/plugins/gqrx.c src/plugins/<name>.c
   ```

5. **Build** with `make`. Every plugin compiled in is always active;
   matching decides which one owns the knob for the focused window.

## main.c

The core (`src/main.c`) needs no changes for a new plugin: it reads raw evdev
events on stdin, matches the focused window class against the registry, and
dispatches to the active plugin's `tick`/`press`. The only main.c coupling to
a specific plugin is the gqrx-specific CLI options (`--gqrx-host`,
`--gqrx-port`) and their default values, which are passed through in
`struct ft_cfg`. If a new plugin needs its own settings, extend `struct
ft_cfg`, add matching `getopt_long` entries in `parse_args`, and read them
inside the plugin's `init`.

### Fail-open

If no plugin matches, the plugin's `tick`/`press` returns 0, or the focus
cannot be determined, the event is written through to the virtual device
unchanged: the knob always keeps working as a volume knob unless a plugin
explicitly consumes the event.
