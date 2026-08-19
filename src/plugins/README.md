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
    int         verbose;    /* generic: log to stderr when set   */
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

`struct ft_cfg` is the single shared configuration handed to every plugin's
`init`. It currently contains the gqrx plugin's own fields plus the generic
`verbose` flag. **`gqrx_host`/`gqrx_port` are not part of the plugin contract** —
they are just one plugin's settings that happen to live here. Your plugin does
not use them and must not rely on them; they are populated only because the
gqrx plugin is compiled in. See [Your own settings](#your-own-settings) for
how to add fields that belong to your plugin.

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
with wayland looking glass command: `ALT-F2 lg` -> Windows tab

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

5. **Add your settings**, if any — see [Your own settings](#your-own-settings).

6. **Build** with `make`. Every plugin compiled in is always active;
   matching decides which one owns the knob for the focused window.

## main.c

The core (`src/main.c`) needs no changes for a new plugin: it reads raw evdev
events on stdin, matches the focused window class against the registry, and
dispatches to the active plugin's `tick`/`press`.

### Your own settings

`struct ft_cfg` is a fixed, shared struct, so adding a setting means adding a
field to it and wiring it through `main.c` once. Every plugin compiled in sees
every field; ignore the ones that are not yours.

For a plugin with a host and a port, the changes are:

1. **Add your fields to `struct ft_cfg`** in `src/plugin.h` (never reuse the
   gqrx fields — add your own, named for your plugin):

   ```c
   struct ft_cfg {
       const char *gqrx_host;  /* gqrx plugin: remote-control host  */
       int         gqrx_port;  /* gqrx plugin: remote-control port  */
       const char *myapp_host; /* your plugin: host                 */
       int         myapp_port; /* your plugin: port                 */
       int         verbose;    /* generic: log to stderr when set   */
   };
   ```

2. **Parse them in `main.c`'s `parse_args`** — add entries to `longopts`
   (with a fresh `val` integer) and to the `switch`:

   ```c
   static const struct option longopts[] = {
       { "gqrx-host",     required_argument, NULL, 1 },
       { "gqrx-port",     required_argument, NULL, 2 },
       { "myapp-host",    required_argument, NULL, 4 },
       { "myapp-port",    required_argument, NULL, 5 },
       ...
   };
   ...
   case 4: cfg->myapp_host = optarg; break;
   case 5: cfg->myapp_port = atoi(optarg); break;
   ```

   and set defaults in `parse_args` next to the gqrx ones:

   ```c
   cfg->myapp_host = "127.0.0.1";
   cfg->myapp_port = 0;   /* your default port */
   ```

3. **Read them in your `init`** — copy the values you need into your plugin's
   state, as `gqrx_init` does (gqrx.c:217):

   ```c
   static int myapp_init(const struct ft_cfg *cfg)
   {
       ...
       g.host = cfg->myapp_host;
       g.port = cfg->myapp_port;
       g.verbose = cfg->verbose;
       return 0;
   }
   ```

That is all the core requires. Everything else — the protocol, the matching,
the fail-open behavior — is your plugin's business.

> The `install.sh` config plumbing is gqrx-specific too: `/etc/freq-tuner/config`
> only defines `GQRX_HOST`/`GQRX_PORT`, and the systemd launcher only maps them
> into the `--gqrx-*` arguments. To make your plugin's settings editable from
> `/etc/freq-tuner/config`, mirror that: add `<NAME>_HOST`/`<NAME>_PORT` to the
> config written by `install.sh` and append
> `--myapp-host $MYAPP_HOST --myapp-port $MYAPP_PORT` to the `ARGS` line in the
> generated launcher.

### Fail-open

If no plugin matches, the plugin's `tick`/`press` returns 0, or the focus
cannot be determined, the event is written through to the virtual device
unchanged: the knob always keeps working as a volume knob unless a plugin
explicitly consumes the event.
