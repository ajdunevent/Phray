# Phray

A Phosh quick-settings plugin that combines StatusNotifierItem
(KStatusNotifierItem/AppIndicator) tray icons and the GNOME Background Apps
portal into a single tile, bringing desktop-style system tray functionality
to Phosh-based mobile Linux.

Developed and tested on FuriOS (Debian forky, aarch64).

## Features

- Shows StatusNotifierItem tray icons, including their DBusMenu menus.
- Shows apps reported by `org.freedesktop.background.Monitor`, with Show and
  Quit actions.
- Optionally provides `org.freedesktop.impl.portal.Background` itself, for
  systems where no other backend implements it (off by default; applies on
  Phosh restart).
- Settings are available in Mobile Settings.

## Screenshots

<table>
  <tr>
    <td valign="top"><img src="screenshots/background-apps.png" alt="Phray tile expanded, listing background apps" width="250"></td>
    <td valign="top"><img src="screenshots/tray-item-menu.png" alt="Tray item menu expanded, with Settings, Pause / Resume, Sync Now, and Quit" width="250"></td>
    <td valign="top"><img src="screenshots/portal-app-menu.png" alt="Background Portal app expanded, with Show and Quit" width="250"></td>
  </tr>
  <tr>
    <td valign="top"><img src="screenshots/mobile-settings.png" alt="Phray in Mobile Settings, Top Bar" width="250"></td>
    <td valign="top"><img src="screenshots/preferences.png" alt="Phray Preferences dialog" width="250"></td>
  </tr>
</table>

## Requirements

- Phosh providing `libphosh-0.45` (built and tested against Phosh 0.58.0).
- Mobile Settings, to open the settings dialog.
- `xdg-desktop-portal`, for the Background Apps source, which reads
  `org.freedesktop.background.Monitor`.

## Building

### Prerequisites

- `libphosh-0.45` and the `phosh-plugins` pkg-config package (Debian:
  `libphosh-0.45-dev`, `phosh-dev`)
- GTK 3 (>= 3.22) and libhandy-1 for the main plugin; GTK 4 and
  libadwaita-1 for the settings plugin
- GLib/GIO (>= 2.68) and gsettings-desktop-schemas (>= 42)
- `wayland-client` and `wayland-scanner`
- meson (>= 0.59), ninja, pkg-config, gettext

`debian/control` lists the Debian build dependencies.

### Compiling

```sh
meson setup builddir
ninja -C builddir
sudo ninja -C builddir install
```

A Debian package can be built with `dpkg-buildpackage`.

Project page: <https://dev.dunevent.net/aj/Phray>

## AI Disclaimer

This project has relied extensively on AI-assisted development, primarily Anthropic's Claude (various models), under human direction and manual testing.

### Contribution Profile

```
Phase                    Human █                  ░ AI
───────────────────────────────────────────────────────
Requirements & Scope      100% ████████████████████  0%
Architecture & Design      60% ████████████░░░░░░░░ 40%
Implementation             15% ███░░░░░░░░░░░░░░░░░ 85%
Testing                   100% ████████████████████  0%
Documentation              25% █████░░░░░░░░░░░░░░░ 75%
```

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
