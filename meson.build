project('phosh-tray-quick-setting', 'c',
  version: '0.1.0',
  meson_version: '>=0.59.0',
  default_options: ['warning_level=1'],
)

gnome = import('gnome')
i18n = import('i18n')

# GETTEXT_PACKAGE is the project name, a gettext domain separate from
# Phosh's, since this plugin ships its translations independently.
config_h = configuration_data()
config_h.set_quoted('GETTEXT_PACKAGE', meson.project_name())
config_h.set_quoted('LOCALEDIR', get_option('prefix') / get_option('localedir'))
configure_file(output: 'config.h', configuration: config_h)

gtk_dep      = dependency('gtk+-3.0', version: '>=3.22')
glib_dep     = dependency('glib-2.0', version: '>=2.68')
gobject_dep  = dependency('gobject-2.0')
gio_dep      = dependency('gio-2.0')
gio_unix_dep = dependency('gio-unix-2.0')
libphosh_dep = dependency('libphosh-0.45')
gsettings_dep = dependency('gsettings-desktop-schemas', version: '>=42')
libhandy_dep = dependency('libhandy-1')
wayland_client_dep = dependency('wayland-client')

# Generates C bindings from the wlr-foreign-toplevel-management protocol
# XML in protocol/, since GTK/GDK doesn't expose the protocol itself.
#
# 'private-code' replaces the deprecated 'code' subcommand; older
# wayland-scanner versions only understand 'code'.
wayland_scanner = find_program('wayland-scanner', native: true)
toplevel_protocol_xml = 'protocol/wlr-foreign-toplevel-management-unstable-v1.xml'

toplevel_protocol_header = custom_target('toplevel-protocol-header',
  input: toplevel_protocol_xml,
  output: 'wlr-foreign-toplevel-management-unstable-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

toplevel_protocol_source = custom_target('toplevel-protocol-source',
  input: toplevel_protocol_xml,
  output: 'wlr-foreign-toplevel-management-unstable-v1-client-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

# The phosh-plugins pkg-config package's variables give the directory
# Phosh scans, which hand-computing ${prefix}/${libdir}/phosh/plugins
# can get wrong.
phosh_plugins_dep = dependency('phosh-plugins')
plugins_dir = phosh_plugins_dep.get_variable(pkgconfig: 'quick_setting_plugins_dir')
plugin_prefs_dir = phosh_plugins_dep.get_variable(pkgconfig: 'quick_setting_prefs_dir')

# The prefs dialog is a separate GTK4 + libadwaita shared module, loaded
# by Mobile Settings (not Phosh) via a dedicated extension point. These
# two dependencies are only needed for prefs/; the rest of the project
# is GTK3 + libhandy.
gtk4_dep = dependency('gtk4')
libadwaita_dep = dependency('libadwaita-1')
plugin_prefs_dep = declare_dependency (
  dependencies: [gtk4_dep, glib_dep, gobject_dep, gio_dep, gio_unix_dep, libadwaita_dep,
                 phosh_plugins_dep],
)

plugin_dep = declare_dependency(
  dependencies: [gtk_dep, glib_dep, gobject_dep, gio_dep, gio_unix_dep, libphosh_dep, libhandy_dep,
                 wayland_client_dep],
)

name = 'tray-quick-setting'

resources = gnome.compile_resources(
  'phosh-plugin-tray-quick-setting-resources',
  'phosh-plugin-tray-quick-setting.gresources.xml',
  c_name: 'phosh_plugin_tray_quick_setting',
)

sources = files(
  'phosh-plugin-tray-quick-setting.c',
  'tray-quick-setting.c',
  'sni-watcher.c',
  'sni-item.c',
  'background-app-watcher.c',
  'background-app-item.c',
  'wl-toplevel-tracker.c',
  'background-portal.c',
) + [toplevel_protocol_source, toplevel_protocol_header]

shared_module(
  'phosh-plugin-' + name,
  sources,
  resources,
  c_args: [
    '-DG_LOG_DOMAIN="phosh-plugin-@0@"'.format(name),
    '-DPLUGIN_NAME="@0@"'.format(name),
  ],
  dependencies: [plugin_dep, gsettings_dep],
  install: true,
  install_dir: plugins_dir,
)

pluginconf = configuration_data()
pluginconf.set('name', name)
pluginconf.set('plugins_dir', plugins_dir)
pluginconf.set('plugin_prefs_dir', plugin_prefs_dir)

# msgfmt --desktop recognizes the standard .desktop keys (Name, Comment,
# etc.) as translatable without an underscore prefix, and drops
# _Name=/_Comment= lines. The input therefore uses plain Name=/Comment=.
desktop_in = configure_file(
  input: name + '.desktop.in.in',
  output: name + '.desktop.in',
  configuration: pluginconf,
)

i18n.merge_file(
  input: desktop_in,
  output: name + '.plugin',
  type: 'desktop',
  po_dir: 'po',
  install: true,
  install_dir: plugins_dir,
)

# Registers Phray's in-process org.freedesktop.impl.portal.Background
# implementation (background-portal.c) with xdg-desktop-portal via
# UseIn=phosh. xdg-desktop-portal-phosh does not implement the
# Background portal, so this fills a gap instead of competing with an
# existing implementation.
install_data('phray.portal', install_dir: get_option('datadir') / 'xdg-desktop-portal' / 'portals')

# Shared between this GTK3 plugin and the GTK4/libadwaita prefs plugin
# in prefs/, which communicate only through this schema.
tray_quick_setting_schema = 'mobi.phosh.plugins.tray-quick-setting.gschema.xml'
compiled_schema = gnome.compile_schemas(depend_files: tray_quick_setting_schema)
compile_schemas = find_program('glib-compile-schemas', required: false)
if compile_schemas.found()
  test(
    'Validate @0@ schema file'.format(tray_quick_setting_schema),
    compile_schemas,
    args: ['--strict', '--dry-run', meson.current_source_dir()],
  )
endif
install_data(
  tray_quick_setting_schema,
  install_dir: 'share/glib-2.0/schemas',
)

# install_data() above only copies the XML; GSettings reads the compiled
# cache (gschemas.compiled) at runtime. Without it, g_settings_new()
# aborts with "Settings schema ... is not installed".
#
# gnome.post_install() replaces a hand-rolled add_install_script() call
# to glib-compile-schemas, which hardcodes /usr/share/glib-2.0/schemas
# and fails under a Debian package build, where dh_auto_install stages
# into debian/phray/usr/... . It skips itself when DESTDIR is set,
# leaving schema compilation to debhelper's dh_installgsettings at
# package-install time.
gnome.post_install(glib_compile_schemas: true)

subdir('po')
subdir('prefs')
