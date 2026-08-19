# freq-tuner

Tune **gqrx** with the volume knob on your keyboard.

When gqrx is the focused application, turning the knob on your keyboard tunes
the radio receiver instead of changing the system volume. In every other
window the knob behaves normally.

## How it works

1. A keyboard grabber (e.g. a [KMonad](https://github.com/kmonad/kmonad)-style
   setup or any interception grabber) turns the physical knob into a virtual
   input device.
2. `freq-tuner` sits in an
   [interception-tools](https://gitlab.com/interception/linux/plugins)
   pipeline, reading that device and routing knob ticks to gqrx:

       interception -g $DEVICE | freq-tuner | uinput -d $DEVICE

3. A GNOME Shell extension watches focus changes and tells `freq-tuner`
   which window is focused. Only when **gqrx** has focus are the knob ticks
   consumed and sent to the receiver (via gqrx's remote-control port).

**Fail-open**: if focus cannot be determined or gqrx is not running, the
event falls through to the virtual device, so the knob never "breaks".

## Components

- **`freq-tuner`** — the router daemon (`src/`). Reads evdev events on stdin,
  writes them on stdout, and routes ticks to the gqrx plugin
  (`src/plugins/gqrx.c`) when gqrx is focused.
- **`knobprobe`** — a read-only probe that lists virtual event devices
  exposing the volume-knob keys (EVIOCGBIT only, never grabs).
- **GNOME Shell extension** (`extension.js`, `prefs.js`, `schemas/`) — watches
  focus changes and sends the focused window's `WM_CLASS` as a datagram to the
  daemon's UNIX DGRAM socket.

## Requirements

- [interception-tools](https://gitlab.com/interception/linux/plugins)
  (interception + uinput) and a keyboard grabber that turns your knob into a
  virtual device
- [gqrx](https://gqrx.dk/) with remote control enabled
- a C compiler and `glib-compile-schemas` (for the extension)
- GNOME Shell 50

## Build

    make

Produces `freq-tuner` and `knobprobe`.

## Install the service

    sudo ./install.sh [gqrx_host] [gqrx_port]

The installer detects the virtual knob device, writes
`/etc/freq-tuner/config`, installs the binaries and the systemd unit
`freq-tuner.service`, and starts it.

> The service is started but **not enabled** at boot: a reboot restores the
> original configuration.

Status: `./status.sh`

Uninstall: `sudo ./uninstall.sh`

## Install the GNOME Shell extension

    ./extension-install.sh

Then enable it (a session restart picks up the new extension):

    gnome-extensions enable freq-tuner@neural75.github.com

The extension's preferences show the service status, the grabbed knob device,
and where to find the debug log (`journalctl --user -f | grep freq-tuner`).

## Configuration

Edit `/etc/freq-tuner/config` and restart the service to apply:

    DEVICE=/dev/input/eventX     # virtual knob device to grab
    DEVNAME="..."                # human-readable device name
    GQRX_HOST=127.0.0.1          # gqrx remote-control host
    GQRX_PORT=7356               # gqrx remote-control port
    FOCUS_SOCK=/run/freq-tuner/focus.sock
    VERBOSE=0

## License

MIT