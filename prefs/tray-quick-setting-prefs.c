/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include "tray-quick-setting-prefs.h"
#include "../portals-conf.h"

#include <gio/gdesktopappinfo.h>
#include <glib/gi18n-lib.h>
#include <string.h>

#define TRAY_QUICK_SETTING_SCHEMA "mobi.phosh.plugins.tray-quick-setting"
#define DISABLED_APPS_KEY         "disabled-apps"

/*
 * Duplicated from sni-watcher.c and background-app-watcher.c: this dialog
 * watches the same two D-Bus services independently, read-only, and does not
 * talk to the running plugin.
 */
#define SNI_WATCHER_BUS_NAME    "org.kde.StatusNotifierWatcher"
#define SNI_WATCHER_OBJECT_PATH "/StatusNotifierWatcher"
#define SNI_WATCHER_IFACE       "org.kde.StatusNotifierWatcher"
#define SNI_ITEM_IFACE          "org.kde.StatusNotifierItem"

#define BG_MONITOR_BUS_NAME    "org.freedesktop.background.Monitor"
#define BG_MONITOR_OBJECT_PATH "/org/freedesktop/background/monitor"
#define BG_MONITOR_IFACE       "org.freedesktop.background.Monitor"

/**
 * PhoshTrayQuickSettingPrefs:
 *
 * Independently watches the same two D-Bus services as the main plugin (SNI
 * watcher, background-portal Monitor) to build a live, per-source list of apps
 * that can be individually hidden from the tray. The whole visible list is
 * rebuilt on any relevant change (an app appearing or disappearing, or the
 * disabled-apps setting changing) instead of diffed, since the lists are small.
 *
 * self->known_apps persists across rebuilds (key "sni:<Id>" or "bg:<app_id>" ->
 * display name) and is refreshed as each source's data arrives. SNI Id/Title
 * resolution needs an extra D-Bus round trip per item, so entries appear
 * progressively.
 */

typedef struct {
  char  *display_name;
  GIcon *icon; /* owned reference */
} AppRowInfo;

static void
app_row_info_free (AppRowInfo *info)
{
  g_free (info->display_name);
  g_clear_object (&info->icon);
  g_free (info);
}


struct _PhoshTrayQuickSettingPrefs {
  AdwPreferencesDialog parent;

  AdwSwitchRow *background_portal_row;
  AdwSwitchRow *enable_sni_row;
  AdwSwitchRow *enable_background_apps_row;
  AdwSwitchRow *prefer_full_color_icons_row;
  GtkListBox   *apps_listbox;
  GSettings    *settings;

  GDBusProxy *sni_watcher_proxy;
  GDBusProxy *bg_monitor_proxy;

  GHashTable *known_apps; /* "sni:<Id>" or "bg:<app_id>" -> AppRowInfo*, owned */
};

G_DEFINE_TYPE (PhoshTrayQuickSettingPrefs, phosh_tray_quick_setting_prefs, ADW_TYPE_PREFERENCES_DIALOG)



static gboolean
is_key_disabled (PhoshTrayQuickSettingPrefs *self, const char *key)
{
  g_auto (GStrv) disabled = g_settings_get_strv (self->settings, DISABLED_APPS_KEY);

  return disabled && g_strv_contains ((const char *const *) disabled, key);
}


static void
set_key_disabled (PhoshTrayQuickSettingPrefs *self, const char *key, gboolean disabled)
{
  g_auto (GStrv) old = g_settings_get_strv (self->settings, DISABLED_APPS_KEY);
  GPtrArray *new_list = g_ptr_array_new_with_free_func (g_free);
  gboolean already_present = FALSE;

  for (guint i = 0; old && old[i]; i++) {
    if (g_strcmp0 (old[i], key) == 0) {
      already_present = TRUE;
      if (disabled)
        g_ptr_array_add (new_list, g_strdup (old[i]));
      /* else: drop it, this is the re-enable case */
    } else {
      g_ptr_array_add (new_list, g_strdup (old[i]));
    }
  }

  if (disabled && !already_present)
    g_ptr_array_add (new_list, g_strdup (key));

  g_ptr_array_add (new_list, NULL);
  g_settings_set_strv (self->settings, DISABLED_APPS_KEY, (const char *const *) new_list->pdata);
  g_ptr_array_unref (new_list);
}



static void
on_app_row_eye_clicked (GtkButton *button, gpointer user_data)
{
  PhoshTrayQuickSettingPrefs *self = PHOSH_TRAY_QUICK_SETTING_PREFS (user_data);
  const char *key = g_object_get_data (G_OBJECT (button), "app-key");

  /*
   * Toggle: visible -> hide, hidden -> show. The icon swap happens on the next
   * render_app_list() rebuild, triggered by the disabled-apps "changed" signal
   * this causes.
   */
  set_key_disabled (self, key, !is_key_disabled (self, key));
}


static GtkWidget *
create_app_row (PhoshTrayQuickSettingPrefs *self, const char *key, const char *display_name,
                const char *source_label, GIcon *icon)
{
  GtkWidget *row = adw_action_row_new ();
  GtkWidget *app_icon = gtk_image_new_from_gicon (icon);
  GtkWidget *eye_button = gtk_button_new ();
  GtkWidget *eye_image;
  gboolean disabled = is_key_disabled (self, key);

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), display_name);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), source_label);

  gtk_image_set_pixel_size (GTK_IMAGE (app_icon), 32);
  adw_action_row_add_prefix (ADW_ACTION_ROW (row), app_icon);

  /*
   * Icon-name lookup is required for symbolic recoloring: an icon loaded from a
   * resource path stays a fixed color regardless of theme (a dark eye icon in
   * dark mode). init() registers the resource path so the name resolves without
   * a filesystem icon-theme install.
   */
  eye_image = gtk_image_new_from_icon_name (disabled
    ? "eye-not-looking-symbolic"
    : "eye-open-negative-filled-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (eye_image), 16);
  gtk_button_set_child (GTK_BUTTON (eye_button), eye_image);
  gtk_widget_add_css_class (eye_button, "flat");
  gtk_widget_set_valign (eye_button, GTK_ALIGN_CENTER);
  g_object_set_data_full (G_OBJECT (eye_button), "app-key", g_strdup (key), g_free);
  g_signal_connect (eye_button, "clicked", G_CALLBACK (on_app_row_eye_clicked), self);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), eye_button);
  adw_action_row_set_activatable_widget (ADW_ACTION_ROW (row), eye_button);

  return row;
}


static int
sort_app_rows (GtkListBoxRow *r1, GtkListBoxRow *r2, gpointer user_data)
{
  const char *t1 = adw_preferences_row_get_title (ADW_PREFERENCES_ROW (r1));
  const char *t2 = adw_preferences_row_get_title (ADW_PREFERENCES_ROW (r2));

  (void) user_data;
  return g_strcmp0 (t1, t2);
}


static void
render_app_list (PhoshTrayQuickSettingPrefs *self)
{
  GHashTableIter iter;
  gpointer key_p, value_p;
  g_auto (GStrv) disabled = g_settings_get_strv (self->settings, DISABLED_APPS_KEY);
  g_autoptr (GHashTable) seen = g_hash_table_new (g_str_hash, g_str_equal);
  gboolean sni_enabled = g_settings_get_boolean (self->settings, "enable-sni");
  gboolean bg_enabled = g_settings_get_boolean (self->settings, "enable-background-apps");

  gtk_list_box_remove_all (self->apps_listbox);

  /* Currently-known (running) apps from either source. */
  g_hash_table_iter_init (&iter, self->known_apps);
  while (g_hash_table_iter_next (&iter, &key_p, &value_p)) {
    const char *key = key_p;
    AppRowInfo *info = value_p;
    const char *source_label = g_str_has_prefix (key, "sni:")
                                  ? _("StatusNotifierItem") : _("Background Portal");

    gtk_list_box_append (self->apps_listbox,
                         create_app_row (self, key, info->display_name, source_label, info->icon));
    g_hash_table_add (seen, (gpointer) key);
  }

  /*
   * Disabled apps that are not running stay listed so they can be re-enabled.
   * "bg:" entries resolve a display name and icon locally from the .desktop
   * file; "sni:" entries have no local resolution, so the raw Id and a generic
   * icon are shown.
   */
  for (guint i = 0; disabled && disabled[i]; i++) {
    const char *key = disabled[i];
    g_autofree char *display_name = NULL;
    g_autoptr (GIcon) icon = NULL;
    const char *source_label;

    if (g_hash_table_contains (seen, key))
      continue;

    if (g_str_has_prefix (key, "bg:")) {
      const char *app_id;

      if (!bg_enabled)
        continue;

      app_id = key + strlen ("bg:");
      {
        g_autofree char *desktop_id = g_strdup_printf ("%s.desktop", app_id);
        g_autoptr (GDesktopAppInfo) desktop_info = g_desktop_app_info_new (desktop_id);
        GIcon *app_icon = desktop_info ? g_app_info_get_icon (G_APP_INFO (desktop_info)) : NULL;

        display_name = g_strdup (desktop_info
                                    ? g_app_info_get_display_name (G_APP_INFO (desktop_info))
                                    : app_id);
        icon = app_icon ? g_object_ref (app_icon)
                         : g_themed_icon_new ("application-x-executable-symbolic");
      }
      source_label = _("Background Portal (not running)");
    } else if (g_str_has_prefix (key, "sni:")) {
      if (!sni_enabled)
        continue;

      display_name = g_strdup (key + strlen ("sni:"));
      icon = g_themed_icon_new ("application-x-executable-symbolic");
      source_label = _("StatusNotifierItem (not running)");
    } else {
      continue; /* unrecognized prefix: ignored */
    }

    gtk_list_box_append (self->apps_listbox,
                         create_app_row (self, key, display_name, source_label, icon));
  }
}



static void
on_sni_item_info_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr (PhoshTrayQuickSettingPrefs) self = PHOSH_TRAY_QUICK_SETTING_PREFS (user_data); /* releases the ref taken in refresh_sni_items(); keeps self alive if the dialog closes mid-fetch */
  g_autoptr (GDBusProxy) item_proxy = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) id_v = NULL;
  g_autoptr (GVariant) title_v = NULL;
  g_autoptr (GVariant) icon_v = NULL;
  const char *id = NULL;
  const char *title = NULL;
  const char *icon_name = NULL;
  g_autofree char *key = NULL;
  AppRowInfo *info;

  item_proxy = g_dbus_proxy_new_finish (result, &error);
  if (!item_proxy)
    return; /* item may have already gone away; not worth warning about */

  id_v = g_dbus_proxy_get_cached_property (item_proxy, "Id");
  title_v = g_dbus_proxy_get_cached_property (item_proxy, "Title");
  icon_v = g_dbus_proxy_get_cached_property (item_proxy, "IconName");
  if (id_v)
    id = g_variant_get_string (id_v, NULL);
  if (!id || !*id)
    return; /* no stable identifier, nothing to key on */
  if (title_v)
    title = g_variant_get_string (title_v, NULL);
  if (icon_v)
    icon_name = g_variant_get_string (icon_v, NULL);

  key = g_strdup_printf ("sni:%s", id);
  info = g_new0 (AppRowInfo, 1);
  info->display_name = g_strdup (title && *title ? title : id);
  info->icon = (icon_name && *icon_name)
                 ? g_themed_icon_new (icon_name)
                 : g_themed_icon_new ("application-x-executable-symbolic");
  g_hash_table_insert (self->known_apps, g_steal_pointer (&key), info);
  render_app_list (self);
  (void) source;
}


static void
refresh_sni_items (PhoshTrayQuickSettingPrefs *self)
{
  g_autoptr (GVariant) items_v = NULL;
  GVariantIter iter;
  const char *entry;
  GHashTableIter table_iter;
  gpointer key_p;
  GList *stale_keys = NULL;

  /*
   * Clear previous "sni:" entries; the async fetches below repopulate whatever
   * is still registered.
   */
  g_hash_table_iter_init (&table_iter, self->known_apps);
  while (g_hash_table_iter_next (&table_iter, &key_p, NULL)) {
    if (g_str_has_prefix ((const char *) key_p, "sni:"))
      stale_keys = g_list_prepend (stale_keys, key_p);
  }
  for (GList *l = stale_keys; l; l = l->next)
    g_hash_table_remove (self->known_apps, l->data);
  g_list_free (stale_keys);

  /*
   * Gated by the master toggle: nothing is queried (or individually excludable)
   * if the whole source is off.
   */
  if (!g_settings_get_boolean (self->settings, "enable-sni"))
    return;

  if (!self->sni_watcher_proxy)
    return;

  items_v = g_dbus_proxy_get_cached_property (self->sni_watcher_proxy,
                                               "RegisteredStatusNotifierItems");
  if (!items_v)
    return;

  /*
   * Same parsing as sni-watcher.c: entries starting with '/' are host-only bare
   * paths (rare); others are bus names with the default object path.
   */
  g_variant_iter_init (&iter, items_v);
  while (g_variant_iter_next (&iter, "&s", &entry)) {
    const char *service = entry[0] == '/' ? "" : entry;
    const char *path = entry[0] == '/' ? entry : "/StatusNotifierItem";

    /*
     * Extra ref released by on_sni_item_info_ready(); keeps self alive if the
     * dialog closes mid-fetch.
     */
    g_dbus_proxy_new_for_bus (G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL,
                              service, path, SNI_ITEM_IFACE, NULL,
                              on_sni_item_info_ready, g_object_ref (self));
  }
}


static void
on_sni_watcher_signal (GDBusProxy *proxy, const char *sender_name, const char *signal_name,
                       GVariant *parameters, gpointer user_data)
{
  PhoshTrayQuickSettingPrefs *self = PHOSH_TRAY_QUICK_SETTING_PREFS (user_data);

  if (g_strcmp0 (signal_name, "StatusNotifierItemRegistered") == 0 ||
      g_strcmp0 (signal_name, "StatusNotifierItemUnregistered") == 0) {
    refresh_sni_items (self);
    render_app_list (self);
  }
  (void) proxy;
  (void) sender_name;
  (void) parameters;
}


static void
on_sni_watcher_proxy_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr (PhoshTrayQuickSettingPrefs) self = PHOSH_TRAY_QUICK_SETTING_PREFS (user_data); /* releases the ref taken where this proxy construction was started */
  g_autoptr (GError) error = NULL;

  self->sni_watcher_proxy = g_dbus_proxy_new_for_bus_finish (result, &error);
  if (!self->sni_watcher_proxy) {
    g_debug ("No SNI watcher available: %s", error->message);
    return;
  }

  g_signal_connect_object (self->sni_watcher_proxy, "g-signal",
                           G_CALLBACK (on_sni_watcher_signal), self, 0);
  refresh_sni_items (self);
  render_app_list (self);
  (void) source;
}



static void
refresh_bg_apps (PhoshTrayQuickSettingPrefs *self)
{
  g_autoptr (GVariant) apps_v = NULL;
  GVariantIter iter;
  GVariant *entry;
  GHashTableIter table_iter;
  gpointer key_p;
  GList *stale_keys = NULL;

  g_hash_table_iter_init (&table_iter, self->known_apps);
  while (g_hash_table_iter_next (&table_iter, &key_p, NULL)) {
    if (g_str_has_prefix ((const char *) key_p, "bg:"))
      stale_keys = g_list_prepend (stale_keys, key_p);
  }
  for (GList *l = stale_keys; l; l = l->next)
    g_hash_table_remove (self->known_apps, l->data);
  g_list_free (stale_keys);

  /* Gated by the master toggle, as in refresh_sni_items(). */
  if (!g_settings_get_boolean (self->settings, "enable-background-apps"))
    return;

  if (!self->bg_monitor_proxy)
    return;

  apps_v = g_dbus_proxy_get_cached_property (self->bg_monitor_proxy, "BackgroundApps");
  if (!apps_v)
    return;

  g_variant_iter_init (&iter, apps_v);
  while (g_variant_iter_next (&iter, "@a{sv}", &entry)) {
    g_autoptr (GVariant) owned_entry = entry;
    const char *app_id = NULL;

    g_variant_lookup (owned_entry, "app_id", "&s", &app_id);
    if (app_id && *app_id) {
      g_autofree char *desktop_id = g_strdup_printf ("%s.desktop", app_id);
      g_autoptr (GDesktopAppInfo) desktop_info = g_desktop_app_info_new (desktop_id);
      g_autofree char *key = g_strdup_printf ("bg:%s", app_id);
      AppRowInfo *row_info = g_new0 (AppRowInfo, 1);
      GIcon *app_icon = desktop_info ? g_app_info_get_icon (G_APP_INFO (desktop_info)) : NULL;

      row_info->display_name = g_strdup (desktop_info
                                            ? g_app_info_get_display_name (G_APP_INFO (desktop_info))
                                            : app_id);
      /*
       * g_app_info_get_icon() is (transfer none): ref it to own it, as
       * app_row_info_free() releases it.
       */
      row_info->icon = app_icon ? g_object_ref (app_icon)
                                 : g_themed_icon_new ("application-x-executable-symbolic");

      g_hash_table_insert (self->known_apps, g_steal_pointer (&key), row_info);
    }
  }
}


static void
on_bg_monitor_properties_changed (GDBusProxy *proxy, GVariant *changed_properties,
                                  GStrv invalidated_properties, gpointer user_data)
{
  PhoshTrayQuickSettingPrefs *self = PHOSH_TRAY_QUICK_SETTING_PREFS (user_data);

  refresh_bg_apps (self);
  render_app_list (self);
  (void) proxy;
  (void) changed_properties;
  (void) invalidated_properties;
}


static void
on_bg_monitor_proxy_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr (PhoshTrayQuickSettingPrefs) self = PHOSH_TRAY_QUICK_SETTING_PREFS (user_data); /* releases the ref taken where this proxy construction was started */
  g_autoptr (GError) error = NULL;

  self->bg_monitor_proxy = g_dbus_proxy_new_for_bus_finish (result, &error);
  if (!self->bg_monitor_proxy) {
    g_debug ("No background-portal Monitor available: %s", error->message);
    return;
  }

  g_signal_connect_object (self->bg_monitor_proxy, "g-properties-changed",
                           G_CALLBACK (on_bg_monitor_properties_changed), self, 0);
  refresh_bg_apps (self);
  render_app_list (self);
  (void) source;
}



static void
phosh_tray_quick_setting_prefs_finalize (GObject *object)
{
  PhoshTrayQuickSettingPrefs *self = PHOSH_TRAY_QUICK_SETTING_PREFS (object);

  g_clear_object (&self->settings);
  g_clear_object (&self->sni_watcher_proxy);
  g_clear_object (&self->bg_monitor_proxy);
  g_clear_pointer (&self->known_apps, g_hash_table_unref);

  G_OBJECT_CLASS (phosh_tray_quick_setting_prefs_parent_class)->finalize (object);
}


static void
phosh_tray_quick_setting_prefs_class_init (PhoshTrayQuickSettingPrefsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->finalize = phosh_tray_quick_setting_prefs_finalize;

  gtk_widget_class_set_template_from_resource (widget_class,
                                               "/mobi/phosh/plugins/"
                                               "tray-quick-setting-prefs/prefs.ui");

  gtk_widget_class_bind_template_child (widget_class, PhoshTrayQuickSettingPrefs,
                                        background_portal_row);
  gtk_widget_class_bind_template_child (widget_class, PhoshTrayQuickSettingPrefs,
                                        enable_sni_row);
  gtk_widget_class_bind_template_child (widget_class, PhoshTrayQuickSettingPrefs,
                                        enable_background_apps_row);
  gtk_widget_class_bind_template_child (widget_class, PhoshTrayQuickSettingPrefs,
                                        prefer_full_color_icons_row);
  gtk_widget_class_bind_template_child (widget_class, PhoshTrayQuickSettingPrefs,
                                        apps_listbox);
}


static void
on_disabled_apps_changed (GSettings *settings, const char *key, gpointer user_data)
{
  render_app_list (PHOSH_TRAY_QUICK_SETTING_PREFS (user_data));
  (void) settings;
  (void) key;
}


static void
on_provide_background_portal_changed (GSettings *settings, const char *key, gpointer user_data)
{
  /*
   * Updates portals.conf along with the setting, so xdg-desktop-portal routes
   * Background requests to Phray.
   */
  if (g_settings_get_boolean (settings, key))
    phosh_portals_conf_enable_background_backend ();
  else
    phosh_portals_conf_disable_background_backend ();

  (void) user_data;
}


static void
on_enable_sni_changed (GSettings *settings, const char *key, gpointer user_data)
{
  PhoshTrayQuickSettingPrefs *self = PHOSH_TRAY_QUICK_SETTING_PREFS (user_data);

  refresh_sni_items (self);
  render_app_list (self);
  (void) settings;
  (void) key;
}


static void
on_enable_background_apps_changed (GSettings *settings, const char *key, gpointer user_data)
{
  PhoshTrayQuickSettingPrefs *self = PHOSH_TRAY_QUICK_SETTING_PREFS (user_data);

  refresh_bg_apps (self);
  render_app_list (self);
  (void) settings;
  (void) key;
}


static void
phosh_tray_quick_setting_prefs_init (PhoshTrayQuickSettingPrefs *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  /*
   * Registers the resource path (with the icons/scalable/actions/ structure
   * from the gresources.xml) so the icon-name lookups in create_app_row() find
   * the bundled icons and get symbolic recoloring.
   */
  gtk_icon_theme_add_resource_path (gtk_icon_theme_get_for_display (gdk_display_get_default ()),
                                     "/mobi/phosh/plugins/tray-quick-setting-prefs/icons");

  self->settings = g_settings_new (TRAY_QUICK_SETTING_SCHEMA);
  self->known_apps = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                            (GDestroyNotify) app_row_info_free);

  g_settings_bind (self->settings, "provide-background-portal",
                   self->background_portal_row, "active",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (self->settings, "enable-sni",
                   self->enable_sni_row, "active",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (self->settings, "enable-background-apps",
                   self->enable_background_apps_row, "active",
                   G_SETTINGS_BIND_DEFAULT);
  /*
   * Inverted: the UI says "Prefer full-color icons" while the schema key is
   * "prefer-symbolic-icons", so the toggle ON means the key is FALSE.
   */
  g_settings_bind (self->settings, "prefer-symbolic-icons",
                   self->prefer_full_color_icons_row, "active",
                   G_SETTINGS_BIND_DEFAULT | G_SETTINGS_BIND_INVERT_BOOLEAN);

  gtk_list_box_set_sort_func (self->apps_listbox, sort_app_rows, NULL, NULL);

  /*
   * Re-render when the disabled-apps set changes, from this dialog or another
   * client of the schema. A master-toggle change re-queries instead, since it
   * changes what is queried, not just what is displayed.
   */
  /*
   * g_signal_connect_object() ties disconnection to self's lifetime;
   * g_signal_connect() would only disconnect when self->settings is destroyed,
   * which assumes self->settings never outlives self.
   */
  g_signal_connect_object (self->settings, "changed::" DISABLED_APPS_KEY,
                           G_CALLBACK (on_disabled_apps_changed), self, 0);
  g_signal_connect_object (self->settings, "changed::enable-sni",
                           G_CALLBACK (on_enable_sni_changed), self, 0);
  g_signal_connect_object (self->settings, "changed::enable-background-apps",
                           G_CALLBACK (on_enable_background_apps_changed), self, 0);
  g_signal_connect_object (self->settings, "changed::provide-background-portal",
                           G_CALLBACK (on_provide_background_portal_changed), self, 0);

  /*
   * Extra refs released by on_sni_watcher_proxy_ready() and
   * on_bg_monitor_proxy_ready(); keep self alive if the dialog closes before
   * the initial proxy constructions finish.
   */
  g_dbus_proxy_new_for_bus (G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL,
                            SNI_WATCHER_BUS_NAME, SNI_WATCHER_OBJECT_PATH, SNI_WATCHER_IFACE,
                            NULL, on_sni_watcher_proxy_ready, g_object_ref (self));
  g_dbus_proxy_new_for_bus (G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL,
                            BG_MONITOR_BUS_NAME, BG_MONITOR_OBJECT_PATH, BG_MONITOR_IFACE,
                            NULL, on_bg_monitor_proxy_ready, g_object_ref (self));
}
