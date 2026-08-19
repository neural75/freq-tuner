import Adw from 'gi://Adw';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Gtk from 'gi://Gtk';
import { ExtensionPreferences } from 'resource:///org/gnome/Shell/Extensions/js/extensions/prefs.js';

const CFG_PATH = '/etc/freq-tuner/config';

export default class FreqTunerPreferences extends ExtensionPreferences {
    fillPreferencesWindow(window) {
        window.add(this._buildServicePage());
        window.add(this._buildDebugPage());
    }

    _buildServicePage() {
        const page = new Adw.PreferencesPage({
            title: 'Service',
            iconName: 'input-keyboard-symbolic',
        });
        const group = new Adw.PreferencesGroup({
            description: 'Routes the dedicated keyboard\'s knob to tune the gqrx radio when the application has focus',
        });

        this._serviceRow = new Adw.ActionRow({ title: 'freq-tuner.service' });
        this._serviceRow.add_suffix(this._buildStatusWidget());
        this._deviceRow = new Adw.ActionRow({ subtitleLines: 1 });
        this._deviceRow.add_prefix(new Gtk.Image({
            iconName: 'input-keyboard-symbolic',
            iconSize: Gtk.IconSize.LARGE,
            valign: Gtk.Align.CENTER,
        }));
        this._notInstalledRow = new Adw.ActionRow({ title: 'Service not installed' });
        this._notInstalledRow.visible = false;

        group.add(this._serviceRow);
        group.add(this._deviceRow);
        group.add(this._notInstalledRow);

        const refresh = new Adw.ButtonRow({
            title: 'Refresh',
        });
        refresh.connect('activated', () => this._refresh());
        group.add(refresh);

        page.add(group);

        this._infoGroup = new Adw.PreferencesGroup();
        page.add(this._infoGroup);
        this._refresh();
        return page;
    }

    _refresh() {
        const cfg = this._readConfig();

        if (cfg === null) {
            this._infoGroup.description = GLib.markup_escape_text(
                'The freq-tuner service is not installed. To install it:\n\n' +
                'git clone https://github.com/neural75/freq-tuner\n' +
                'cd freq-tuner && make\n' +
                'sudo ./install.sh', -1);
            this._serviceRow.visible = false;
            this._deviceRow.visible = false;
            this._notInstalledRow.visible = true;
            return;
        }

        const active = this._serviceActive();
        if (active) {
            this._infoGroup.description = '';
        } else {
            this._infoGroup.description = GLib.markup_escape_text(
                'The freq-tuner service is not running. To start and enable it:\n\n' +
                'sudo systemctl start freq-tuner.service\n' +
                'sudo systemctl enable freq-tuner.service', -1);
        }
        this._serviceRow.visible = true;
        this._deviceRow.visible = true;
        this._deviceRow.title = cfg.DEVNAME || '(unknown keyboard)';
        this._deviceRow.subtitle = cfg.DEVICE || '(unknown device)';
        this._updateStatus(active);
        this._notInstalledRow.visible = false;
    }

    _buildStatusWidget() {
        const box = new Gtk.Box({
            spacing: 6,
            valign: Gtk.Align.CENTER,
        });
        this._statusDot = new Gtk.Label({ useMarkup: true });
        this._statusLabel = new Gtk.Label();
        box.append(this._statusDot);
        box.append(this._statusLabel);
        return box;
    }

    _updateStatus(active) {
        const color = active ? '#2ec27e' : '#9a9996';
        this._statusDot.label = `<span foreground="${color}">●</span>`;
        this._statusLabel.label = active ? 'Active' : 'Stopped';
    }

    _readConfig() {
        try {
            const file = Gio.File.new_for_path(CFG_PATH);
            const [ok, contents] = file.load_contents(null);
            if (!ok)
                return null;
            const text = new TextDecoder().decode(contents);
            const cfg = {};
            for (const line of text.split('\n')) {
                const match = line.match(/^([A-Z_]+)="?(.*?)"?\s*$/);
                if (match)
                    cfg[match[1]] = match[2];
            }
            return cfg;
        } catch (e) {
            return null;
        }
    }

    _serviceActive() {
        const [ok, stdout] = GLib.spawn_command_line_sync(
            'systemctl is-active freq-tuner.service');
        if (!ok)
            return false;
        const state = new TextDecoder().decode(stdout).trim();
        return state === 'active' || state === 'activating';
    }

    _buildDebugPage() {
        const settings = this.getSettings();
        const page = new Adw.PreferencesPage({
            title: 'Debug',
            iconName: 'view-list-symbolic',
        });
        const group = new Adw.PreferencesGroup({
            title: 'Debugging',
            description: 'Log the focused window class of every window to the journal',
        });
        const row = new Adw.SwitchRow({
            title: 'Log window classes',
            subtitle: 'View output with: journalctl --user -f | grep freq-tuner',
        });
        settings.bind('log-window-classes', row, 'active',
            Gio.SettingsBindFlags.DEFAULT);
        group.add(row);
        page.add(group);
        return page;
    }
}