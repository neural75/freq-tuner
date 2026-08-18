/*
 * freq-tuner - reports the focused window class to freq-tuner.
 *
 * The gqrx plugin only routes the knob while the radio is focused, so the
 * daemon needs to know which window the user is looking at. This extension
 * watches focus changes and sends the WM_CLASS of the focused window as a
 * single datagram to the daemon's UNIX DGRAM socket.
 *
 * SPDX-License-Identifier: MIT
 */
import Gio from 'gi://Gio';

const SOCKET_PATH = '/run/freq-tuner/focus.sock';

export default class FreqTunerFocus {
    constructor() {
        this._socket = null;
        this._focusId = 0;
        this._last = '';
    }

    enable() {
        this._socket = Gio.Socket.new(Gio.SocketFamily.UNIX,
            Gio.SocketType.DATAGRAM, Gio.SocketProtocol.DEFAULT);
        this._focusId = global.display.connect('notify::focus-window',
            this._onFocusChanged.bind(this));
        this._onFocusChanged();
    }

    disable() {
        if (this._focusId !== 0)
            global.display.disconnect(this._focusId);
        this._focusId = 0;
        if (this._socket !== null) {
            try {
                this._socket.close();
            } catch (e) {
            }
            this._socket = null;
        }
        this._last = '';
    }

    _windowClass(window) {
        if (window === null)
            return '';
        return window.get_wm_class() || window.get_wm_class_instance() || '';
    }

    _onFocusChanged() {
        const cls = this._windowClass(global.display.focus_window);
        if (cls === this._last)
            return;
        this._last = cls;
        this._send(cls);
    }

    _send(cls) {
        if (this._socket === null)
            return;
        const address = Gio.UnixSocketAddress.new(SOCKET_PATH);
        const bytes = new TextEncoder().encode(cls);
        try {
            this._socket.send_to(address, bytes, null);
        } catch (e) {
            // freq-tuner is not running or the socket is gone: nothing to do
        }
    }
}