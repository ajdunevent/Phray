/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include "tray-quick-setting-prefs.h"

#include <glib/gi18n-lib.h>
#include "phosh-plugin.h"

/*
 * Registers PHOSH_PLUGIN_EXTENSION_POINT_QUICK_SETTING_WIDGET_PREFS, the
 * extension point loaded by Mobile Settings (a separate GTK4 process), not the
 * one registered by phosh-plugin-tray-quick-setting.c. The macro is defined in
 * phosh-plugin.h.
 */

char **g_io_phosh_plugin_prefs_tray_quick_setting_query (void);

void
g_io_module_load (GIOModule *module)
{
  g_type_module_use (G_TYPE_MODULE (module));

  /*
   * No textdomain() call (see phosh-plugin-tray-quick-setting.c);
   * bindtextdomain() is sufficient for _().
   */
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");

  g_io_extension_point_implement (PHOSH_PLUGIN_EXTENSION_POINT_QUICK_SETTING_WIDGET_PREFS,
                                  PHOSH_TYPE_TRAY_QUICK_SETTING_PREFS,
                                  PLUGIN_PREFS_NAME,
                                  10);
}


void
g_io_module_unload (GIOModule *module)
{
}


char **
g_io_phosh_plugin_prefs_tray_quick_setting_query (void)
{
  char *extension_points[] = { PHOSH_PLUGIN_EXTENSION_POINT_QUICK_SETTING_WIDGET_PREFS, NULL };

  return g_strdupv (extension_points);
}
