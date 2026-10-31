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

## Requirements

- Phosh with `libphosh-0.45`
- GTK 3 and libhandy (main plugin), GTK 4 and libadwaita (settings plugin)
- `wayland-client`, `wayland-scanner`
- meson, ninja, gettext

`debian/control` lists the Debian package names.

## Building

```sh
meson setup builddir
ninja -C builddir
sudo ninja -C builddir install
```

A Debian package can be built with `dpkg-buildpackage`.

Project page: <https://dev.dunevent.net/aj/Phray>

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
