# freq-tuner

Rotary input router for per-application tuning.

The physical keyboard has a volume knob. With a keyboard grabber the knob
turns into a virtual input device. `freq-tuner` sits in an
[interception-tools](https://gitlab.com/interception/linux/plugins) pipeline,
reads that device, and routes the knob's ticks to the plugin matching the
currently focused application:

    interception -g $DEVICE | freq-tuner | uinput -d $DEVICE

While the radio application (gqrx) is focused the knob is consumed and tunes
the receiver instead. Everywhere else events pass through unchanged, so the
knob keeps its normal behaviour.

**Fail-open**: if focus cannot be determined or the plugin cannot act, the
event falls through to the virtual device.

## Architecture

- **`freq-tuner`** — the router daemon (`src/`). Reads evdev events on stdin,
  writes them on stdout. Built around a plugin registry (`src/plugin.c`,
  `src/plugins/gqrx.c`).
- **`knobprobe`** — a read-only probe that lists virtual event devices
  exposing the volume-knob keys (EVIOCGBIT only, never grabs).
- **GNOME Shell extension** (`extension.js`, `prefs.js`, `schemas/`) — watches
  focus changes and sends the focused window's `WM_CLASS` as a datagram to the
  daemon's UNIX DGRAM socket, so the router knows which window the user is
  looking at.

## Requirements

- [interception-tools](https://gitlab.com/interception/linux/plugins)
  (interception + uinput) and a keyboard grabber that turns your knob into a
  virtual device
- a C compiler and `glib-compile-schemas` (for the extension)
- GNOME Shell 50
- [gqrx](https://gqrx.dk/) with remote control enabled (default `127.0.0.1:7356`)

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
    GQRX_HOST=127.0.0.1
    GQRX_PORT=7356
    FOCUS_SOCK=/run/freq-tuner/focus.sock
    VERBOSE=0

## License

MIT
