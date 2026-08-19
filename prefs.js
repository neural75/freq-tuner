import Adw from 'gi://Adw';
import Gio from 'gi://Gio';
import { ExtensionPreferences } from 'resource:///org/gnome/Shell/Extensions/js/extensions/prefs.js';

export default class FreqTunerPreferences extends ExtensionPreferences {
    fillPreferencesWindow(window) {
        window.add(this._buildDebugPage());
    }

    _buildDebugPage() {
        const settings = this.getSettings();
        const page = new Adw.PreferencesPage({
            title: 'Debug',
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