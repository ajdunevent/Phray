tray_quick_setting_prefs_resources = gnome.compile_resources(
  'phosh-plugin-prefs-tray-quick-setting-resources',
  'phosh-plugin-prefs-tray-quick-setting.gresources.xml',
  c_name: 'phosh_plugin_prefs_tray_quick_setting',
)

tray_quick_setting_plugin_prefs_sources = files(
  'tray-quick-setting-prefs.c',
  'tray-quick-setting-prefs.h',
  'phosh-plugin-prefs-tray-quick-setting.c',
  '../portals-conf.c',
  '../portals-conf.h',
)

shared_module(
  'phosh-plugin-prefs-' + name,
  tray_quick_setting_plugin_prefs_sources,
  tray_quick_setting_prefs_resources,
  c_args: [
    '-DG_LOG_DOMAIN="phosh-plugin-prefs-@0@"'.format(name),
    '-DPLUGIN_PREFS_NAME="@0@-prefs"'.format(name),
  ],
  include_directories: include_directories('..'), # for config.h, generated at the project root
  dependencies: [plugin_prefs_dep],
  install: true,
  install_dir: plugin_prefs_dir,
)
