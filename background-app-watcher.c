/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "background-app-watcher.h"

#define PORTAL_BUS_NAME    "org.freedesktop.background.Monitor"
#define PORTAL_OBJECT_PATH "/org/freedesktop/background/monitor"
#define MONITOR_IFACE      "org.freedesktop.background.Monitor"

enum {
  APP_ADDED,
  APP_REMOVED,
  APP_CHANGED,
  N_SIGNALS,
};
static guint signals[N_SIGNALS];

struct _PhoshBackgroundAppWatcher {
  GObject parent;

  GDBusConnection *connection;
  GDBusProxy      *proxy; /* org.freedesktop.background.Monitor */

  GHashTable *known; /* instance -> app_id (both owned); last reported state, for diffing */
};

G_DEFINE_TYPE (PhoshBackgroundAppWatcher, phosh_background_app_watcher, G_TYPE_OBJECT)


static void
refresh_from_property (PhoshBackgroundAppWatcher *self)
{
  g_autoptr (GVariant) apps_v = NULL;
  GVariantIter iter;
  GVariant *entry;
  g_autoptr (GHashTable) seen = g_hash_table_new (g_str_hash, g_str_equal);

  apps_v = g_dbus_proxy_get_cached_property (self->proxy, "BackgroundApps");
  if (!apps_v)
    return;

  g_variant_iter_init (&iter, apps_v);
  while (g_variant_iter_next (&iter, "@a{sv}", &entry)) {
    const char *app_id = NULL;
    const char *instance = NULL;
    const char *message = NULL;
    g_autoptr (GVariant) owned_entry = entry;

    g_variant_lookup (owned_entry, "app_id", "&s", &app_id);
    g_variant_lookup (owned_entry, "instance", "&s", &instance);
    g_variant_lookup (owned_entry, "message", "&s", &message);

    if (!instance || !app_id)
      continue;

    g_hash_table_add (seen, (gpointer) instance);

    if (!g_hash_table_contains (self->known, instance)) {
      g_hash_table_insert (self->known, g_strdup (instance), g_strdup (app_id));
      g_signal_emit (self, signals[APP_ADDED], 0, instance, app_id);
    } else {
      /* Already known: only the message can change. */
      g_signal_emit (self, signals[APP_CHANGED], 0, instance, message ? message : "");
    }
  }

  /* Anything previously known but not in `seen` has stopped running. */
  {
    GHashTableIter known_iter;
    gpointer key;
    GList *gone = NULL;

    g_hash_table_iter_init (&known_iter, self->known);
    while (g_hash_table_iter_next (&known_iter, &key, NULL)) {
      if (!g_hash_table_contains (seen, key))
        gone = g_list_prepend (gone, g_strdup (key));
    }

    for (GList *l = gone; l; l = l->next) {
      g_signal_emit (self, signals[APP_REMOVED], 0, (const char *) l->data);
      g_hash_table_remove (self->known, l->data);
    }
    g_list_free_full (gone, g_free);
  }
}


static void
on_monitor_properties_changed (GDBusProxy *proxy, GVariant *changed_properties,
                                GStrv invalidated_properties, gpointer user_data)
{
  PhoshBackgroundAppWatcher *self = PHOSH_BACKGROUND_APP_WATCHER (user_data);

  refresh_from_property (self);
  (void) proxy;
  (void) changed_properties;
  (void) invalidated_properties;
}


static void
on_monitor_proxy_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr (PhoshBackgroundAppWatcher) self = PHOSH_BACKGROUND_APP_WATCHER (user_data); /* releases the ref taken in on_bus_ready() */
  g_autoptr (GError) error = NULL;

  self->proxy = g_dbus_proxy_new_finish (result, &error);
  if (!self->proxy) {
    /*
     * Not fatal: some systems have no Background portal backend. The watcher
     * then never reports anything, as with an empty list.
     */
    g_debug ("No org.freedesktop.background.Monitor available: %s", error->message);
    return;
  }

  g_signal_connect_object (self->proxy, "g-properties-changed",
                            G_CALLBACK (on_monitor_properties_changed), self, 0);
  refresh_from_property (self);
  (void) source;
}


static void
on_bus_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr (PhoshBackgroundAppWatcher) self = PHOSH_BACKGROUND_APP_WATCHER (user_data); /* releases the ref taken in phosh_background_app_watcher_init() */
  g_autoptr (GError) error = NULL;

  self->connection = g_bus_get_finish (result, &error);
  if (!self->connection) {
    g_warning ("Failed to get session bus for background-apps watcher: %s", error->message);
    return;
  }

  /*
   * Extra ref released by on_monitor_proxy_ready(); keeps the watcher alive
   * until the proxy is created.
   */
  g_dbus_proxy_new (self->connection, G_DBUS_PROXY_FLAGS_NONE, NULL,
                     PORTAL_BUS_NAME, PORTAL_OBJECT_PATH, MONITOR_IFACE,
                     NULL, on_monitor_proxy_ready, g_object_ref (self));
  (void) source;
}


GDBusConnection *
phosh_background_app_watcher_get_connection (PhoshBackgroundAppWatcher *self)
{
  return self->connection;
}


static void
phosh_background_app_watcher_finalize (GObject *object)
{
  PhoshBackgroundAppWatcher *self = PHOSH_BACKGROUND_APP_WATCHER (object);

  g_clear_pointer (&self->known, g_hash_table_unref);
  g_clear_object (&self->proxy);
  g_clear_object (&self->connection);

  G_OBJECT_CLASS (phosh_background_app_watcher_parent_class)->finalize (object);
}


static void
phosh_background_app_watcher_class_init (PhoshBackgroundAppWatcherClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = phosh_background_app_watcher_finalize;

  signals[APP_ADDED] = g_signal_new ("app-added", G_TYPE_FROM_CLASS (klass),
                                      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                      G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);
  signals[APP_REMOVED] = g_signal_new ("app-removed", G_TYPE_FROM_CLASS (klass),
                                        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                        G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[APP_CHANGED] = g_signal_new ("app-changed", G_TYPE_FROM_CLASS (klass),
                                        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                        G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);
}


static void
phosh_background_app_watcher_init (PhoshBackgroundAppWatcher *self)
{
  self->known = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  /*
   * Extra ref released by on_bus_ready(); keeps the watcher alive until the
   * async chain (g_bus_get -> g_dbus_proxy_new) completes, since it can be
   * destroyed mid-chain (e.g. during a tile rearrangement).
   */
  g_bus_get (G_BUS_TYPE_SESSION, NULL, on_bus_ready, g_object_ref (self));
}


static PhoshBackgroundAppWatcher *default_watcher = NULL;

PhoshBackgroundAppWatcher *
phosh_background_app_watcher_get_default (void)
{
  if (!default_watcher) {
    default_watcher = g_object_new (PHOSH_TYPE_BACKGROUND_APP_WATCHER, NULL);
    /*
     * Resets default_watcher to NULL if the object is ever finalized, so a
     * later call constructs a fresh instance instead of returning a stale
     * pointer.
     */
    g_object_add_weak_pointer (G_OBJECT (default_watcher), (gpointer *) &default_watcher);
  }

  return g_object_ref (default_watcher);
}


void
phosh_background_app_watcher_replay_existing_apps (PhoshBackgroundAppWatcher *self)
{
  g_autoptr (GVariant) apps_v = NULL;
  GVariantIter iter;
  GVariant *entry;

  /*
   * Re-queries the property (as refresh_from_property() does) instead of
   * iterating self->known: known holds only app_id per instance, not the
   * message, and construct_and_track_bg_item() takes no message parameter (it
   * is set in response to "app-changed"). Replaying only "app-added" would
   * leave reconstructed items without a subtitle until the next property
   * change.
   */
  if (!self->proxy)
    return;

  apps_v = g_dbus_proxy_get_cached_property (self->proxy, "BackgroundApps");
  if (!apps_v)
    return;

  g_variant_iter_init (&iter, apps_v);
  while (g_variant_iter_next (&iter, "@a{sv}", &entry)) {
    const char *app_id = NULL;
    const char *instance = NULL;
    const char *message = NULL;
    g_autoptr (GVariant) owned_entry = entry;

    g_variant_lookup (owned_entry, "app_id", "&s", &app_id);
    g_variant_lookup (owned_entry, "instance", "&s", &instance);
    g_variant_lookup (owned_entry, "message", "&s", &message);

    if (!instance || !app_id)
      continue;

    g_signal_emit (self, signals[APP_ADDED], 0, instance, app_id);
    if (message && *message)
      g_signal_emit (self, signals[APP_CHANGED], 0, instance, message);
  }
}
