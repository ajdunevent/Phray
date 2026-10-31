/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include <gio/gio.h>
#include <glib/gi18n-lib.h>
#include "phosh-plugin.h"

#include "tray-quick-setting.h"
#include "background-portal.h"
#include "sni-watcher.h"
#include "background-app-watcher.h"


char **g_io_phosh_plugin_tray_quick_setting_query (void);

/* Permanent references, taken in g_io_module_load(). */
static PhoshBackgroundPortal *permanent_background_portal_ref = NULL;
static PhoshSniWatcher *permanent_sni_watcher_ref = NULL;
static PhoshBackgroundAppWatcher *permanent_bg_watcher_ref = NULL;


void
g_io_module_load (GIOModule *module)
{
  g_type_module_use (G_TYPE_MODULE (module));

  /*
   * g_io_module_load() also runs in other processes (e.g. phrog, a greetd
   * greeter built on libphosh) and more than once per process. The
   * g_get_prgname() guard and the one-time reference prevent two processes from
   * owning the same D-Bus name.
   */
  if (g_strcmp0 (g_get_prgname (), "phosh") == 0) {
    if (!permanent_background_portal_ref) {
      /*
       * Constructing the background-portal singleton at module load makes the
       * provide-background-portal setting read once, at session start.
       * Constructing it from the tray widget would re-run the D-Bus name
       * ownership and Wayland tracker setup on every tile rearrangement or
       * toggle.
       */
      permanent_background_portal_ref = phosh_background_portal_get_default ();
    }
    if (!permanent_sni_watcher_ref) {
      /*
       * Constructing PhoshSniWatcher at module load makes it become (or connect
       * to) the Watcher once, at session start. A fresh instance per tile
       * rearrangement or toggle could lose the org.kde.StatusNotifierWatcher
       * name-ownership race to its dying predecessor, and SNI apps, which
       * register once, would not re-register with the replacement, leaving the
       * tray empty.
       */
      permanent_sni_watcher_ref = phosh_sni_watcher_get_default ();
    }
    if (!permanent_bg_watcher_ref) {
      /*
       * Avoids repeating D-Bus connection setup and the full re-query of
       * running background apps on each tile rearrangement or toggle.
       */
      permanent_bg_watcher_ref = phosh_background_app_watcher_get_default ();
    }
  }

  /*
   * textdomain() is not called: it sets the process-global default domain, and
   * calling it would redirect Phosh's unqualified gettext() calls to this
   * plugin's domain. _() uses dgettext(GETTEXT_PACKAGE, ...), so
   * bindtextdomain() is sufficient.
   */
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");

  g_io_extension_point_implement (PHOSH_PLUGIN_EXTENSION_POINT_QUICK_SETTING_WIDGET,
                                   PHOSH_TYPE_TRAY_QUICK_SETTING,
                                   PLUGIN_NAME,
                                   10);
}


void
g_io_module_unload (GIOModule *module)
{
}


char **
g_io_phosh_plugin_tray_quick_setting_query (void)
{
  char *extension_points[] = {PHOSH_PLUGIN_EXTENSION_POINT_QUICK_SETTING_WIDGET, NULL};

  return g_strdupv (extension_points);
}
