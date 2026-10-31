/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sni-watcher.h"

#define WATCHER_BUS_NAME    "org.kde.StatusNotifierWatcher"
#define WATCHER_OBJECT_PATH "/StatusNotifierWatcher"
#define WATCHER_IFACE       "org.kde.StatusNotifierWatcher"

/*
 * Hand-written introspection XML for the Watcher-mode interface, parsed with
 * g_dbus_node_info_new_for_xml() instead of adding gdbus-codegen as a build
 * dependency.
 */
static const char *watcher_introspection_xml =
  "<node>"
  "  <interface name='org.kde.StatusNotifierWatcher'>"
  "    <method name='RegisterStatusNotifierItem'>"
  "      <arg type='s' name='service' direction='in'/>"
  "    </method>"
  "    <method name='RegisterStatusNotifierHost'>"
  "      <arg type='s' name='service' direction='in'/>"
  "    </method>"
  "    <property name='RegisteredStatusNotifierItems' type='as' access='read'/>"
  "    <property name='IsStatusNotifierHostRegistered' type='b' access='read'/>"
  "    <property name='ProtocolVersion' type='i' access='read'/>"
  "    <signal name='StatusNotifierItemRegistered'><arg type='s'/></signal>"
  "    <signal name='StatusNotifierItemUnregistered'><arg type='s'/></signal>"
  "    <signal name='StatusNotifierHostRegistered'/>"
  "    <signal name='StatusNotifierHostUnregistered'/>"
  "  </interface>"
  "</node>";

enum {
  ITEM_ADDED,
  ITEM_REMOVED,
  N_SIGNALS,
};
static guint signals[N_SIGNALS];

typedef struct {
  char *service;   /* bus name the item lives on (often its unique name) */
  char *path;      /* object path exporting org.kde.StatusNotifierItem */
  guint watch_id;  /* g_bus_watch_name on `service`, for cleanup on crash */
} WatchedItem;

struct _PhoshSniWatcher {
  GObject parent;

  GDBusConnection *connection;
  gboolean         is_watcher;      /* TRUE: owns the Watcher name */
  guint            own_name_id;     /* g_bus_own_name() id for this attempt */
  guint            watcher_reg_id;  /* g_dbus_connection_register_object() id, watcher mode */
  GDBusNodeInfo   *introspection;

  GDBusProxy      *watcher_proxy;   /* host-only mode: proxy to the other watcher */

  GHashTable      *items; /* "service\x1fpath" -> WatchedItem*, watcher mode only */
};

G_DEFINE_TYPE (PhoshSniWatcher, phosh_sni_watcher, G_TYPE_OBJECT)


static char *
item_key (const char *service, const char *path)
{
  return g_strdup_printf ("%s\x1f%s", service, path);
}


static void
watched_item_free (WatchedItem *item)
{
  if (item->watch_id)
    g_bus_unwatch_name (item->watch_id);
  g_free (item->service);
  g_free (item->path);
  g_free (item);
}



static void
watcher_emit_registered (PhoshSniWatcher *self, const char *service)
{
  g_dbus_connection_emit_signal (self->connection, NULL, WATCHER_OBJECT_PATH,
                                  WATCHER_IFACE, "StatusNotifierItemRegistered",
                                  g_variant_new ("(s)", service), NULL);
}


static void
watcher_emit_unregistered (PhoshSniWatcher *self, const char *service)
{
  g_dbus_connection_emit_signal (self->connection, NULL, WATCHER_OBJECT_PATH,
                                  WATCHER_IFACE, "StatusNotifierItemUnregistered",
                                  g_variant_new ("(s)", service), NULL);
}


static void
on_watched_item_vanished (GDBusConnection *connection, const char *name, gpointer user_data)
{
  PhoshSniWatcher *self = PHOSH_SNI_WATCHER (user_data);
  GHashTableIter iter;
  gpointer key, value;

  /* `name` is the bus name of a crashed/quit item; find and drop it. */
  g_hash_table_iter_init (&iter, self->items);
  while (g_hash_table_iter_next (&iter, &key, &value)) {
    WatchedItem *item = value;

    if (g_strcmp0 (item->service, name) != 0)
      continue;

    g_debug ("Tray item vanished: %s%s", item->service, item->path);
    g_signal_emit (self, signals[ITEM_REMOVED], 0, item->service, item->path);
    watcher_emit_unregistered (self, item->service);
    g_hash_table_iter_remove (&iter);
    break;
  }
  (void) connection;
}


static void
phosh_sni_watcher_track_item (PhoshSniWatcher *self, const char *service, const char *path)
{
  g_autofree char *key = item_key (service, path);
  WatchedItem *item;

  if (g_hash_table_contains (self->items, key))
    return;

  item = g_new0 (WatchedItem, 1);
  item->service = g_strdup (service);
  item->path = g_strdup (path);
  item->watch_id = g_bus_watch_name_on_connection (self->connection, service,
                                                     G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                     NULL, on_watched_item_vanished,
                                                     self, NULL);
  g_hash_table_insert (self->items, g_strdup (key), item);

  g_debug ("Tray item registered: %s%s", service, path);
  g_signal_emit (self, signals[ITEM_ADDED], 0, service, path);
  watcher_emit_registered (self, service);
}


static void
watcher_handle_register_item (PhoshSniWatcher       *self,
                               const char            *sender,
                               GVariant              *parameters,
                               GDBusMethodInvocation *invocation)
{
  const char *service_arg;
  g_autofree char *service = NULL;
  g_autofree char *path = NULL;

  g_variant_get (parameters, "(&s)", &service_arg);

  /* Spec 0.8+: an item may pass just its object path and rely on the
   * sender's unique name as the bus name. Older items pass a full bus
   * name; the default object path is assumed in that case. */
  if (service_arg[0] == '/') {
    service = g_strdup (sender);
    path = g_strdup (service_arg);
  } else {
    service = g_strdup (service_arg);
    path = g_strdup ("/StatusNotifierItem");
  }

  phosh_sni_watcher_track_item (self, service, path);
  g_dbus_method_invocation_return_value (invocation, NULL);
}


static void
watcher_method_call (GDBusConnection       *connection,
                      const char            *sender,
                      const char            *object_path,
                      const char            *interface_name,
                      const char            *method_name,
                      GVariant              *parameters,
                      GDBusMethodInvocation *invocation,
                      gpointer               user_data)
{
  PhoshSniWatcher *self = PHOSH_SNI_WATCHER (user_data);

  if (g_strcmp0 (method_name, "RegisterStatusNotifierItem") == 0) {
    watcher_handle_register_item (self, sender, parameters, invocation);
  } else if (g_strcmp0 (method_name, "RegisterStatusNotifierHost") == 0) {
    /*
     * This watcher is already its own host; the call is acknowledged and
     * ignored.
     */
    g_dbus_method_invocation_return_value (invocation, NULL);
  } else {
    g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                                            "Unknown method %s", method_name);
  }

  (void) connection;
  (void) object_path;
  (void) interface_name;
}


static GVariant *
watcher_get_property (GDBusConnection  *connection,
                       const char       *sender,
                       const char       *object_path,
                       const char       *interface_name,
                       const char       *property_name,
                       GError          **error,
                       gpointer          user_data)
{
  PhoshSniWatcher *self = PHOSH_SNI_WATCHER (user_data);

  if (g_strcmp0 (property_name, "RegisteredStatusNotifierItems") == 0) {
    GVariantBuilder builder;
    GHashTableIter iter;
    gpointer value;

    g_variant_builder_init (&builder, G_VARIANT_TYPE ("as"));
    g_hash_table_iter_init (&iter, self->items);
    while (g_hash_table_iter_next (&iter, NULL, &value)) {
      WatchedItem *item = value;
      g_variant_builder_add (&builder, "s", item->service);
    }
    return g_variant_builder_end (&builder);
  }

  if (g_strcmp0 (property_name, "IsStatusNotifierHostRegistered") == 0)
    return g_variant_new_boolean (TRUE);

  if (g_strcmp0 (property_name, "ProtocolVersion") == 0)
    return g_variant_new_int32 (0);

  (void) connection;
  (void) sender;
  (void) object_path;
  (void) interface_name;
  (void) error;
  return NULL;
}


static const GDBusInterfaceVTable watcher_vtable = {
  .method_call = watcher_method_call,
  .get_property = watcher_get_property,
  .set_property = NULL,
};



static void
host_add_existing_items (PhoshSniWatcher *self)
{
  g_autoptr (GVariant) items_v = NULL;

  items_v = g_dbus_proxy_get_cached_property (self->watcher_proxy, "RegisteredStatusNotifierItems");
  if (!items_v)
    return;

  GVariantIter iter;
  const char *service;

  g_variant_iter_init (&iter, items_v);
  while (g_variant_iter_next (&iter, "&s", &service)) {
    /*
     * The string is a bus name or an object path (no "bus:path" form is
     * standardised); a plain bus name gets the default object path
     * "/StatusNotifierItem".
     */
    if (service[0] == '/')
      g_signal_emit (self, signals[ITEM_ADDED], 0, "", service);
    else
      g_signal_emit (self, signals[ITEM_ADDED], 0, service, "/StatusNotifierItem");
  }
}


static void
on_watcher_proxy_signal (GDBusProxy *proxy, const char *sender_name, const char *signal_name,
                          GVariant *parameters, gpointer user_data)
{
  PhoshSniWatcher *self = PHOSH_SNI_WATCHER (user_data);
  const char *service;

  if (g_strcmp0 (signal_name, "StatusNotifierItemRegistered") == 0) {
    g_variant_get (parameters, "(&s)", &service);
    if (service[0] == '/')
      g_signal_emit (self, signals[ITEM_ADDED], 0, "", service);
    else
      g_signal_emit (self, signals[ITEM_ADDED], 0, service, "/StatusNotifierItem");
  } else if (g_strcmp0 (signal_name, "StatusNotifierItemUnregistered") == 0) {
    g_variant_get (parameters, "(&s)", &service);
    g_signal_emit (self, signals[ITEM_REMOVED], 0, service, "/StatusNotifierItem");
  }

  (void) proxy;
  (void) sender_name;
}


static void
on_host_proxy_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr (PhoshSniWatcher) self = PHOSH_SNI_WATCHER (user_data); /* releases the ref taken in phosh_sni_watcher_become_host_only() */
  g_autoptr (GError) error = NULL;

  self->watcher_proxy = g_dbus_proxy_new_finish (result, &error);
  if (!self->watcher_proxy) {
    g_warning ("Failed to connect to existing StatusNotifierWatcher: %s", error->message);
    return;
  }

  g_signal_connect_object (self->watcher_proxy, "g-signal",
                            G_CALLBACK (on_watcher_proxy_signal), self, 0);

  g_dbus_proxy_call (self->watcher_proxy, "RegisterStatusNotifierHost",
                      g_variant_new ("(s)", g_dbus_connection_get_unique_name (self->connection)),
                      G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);

  host_add_existing_items (self);
  (void) source;
}


static void
phosh_sni_watcher_become_host_only (PhoshSniWatcher *self)
{
  g_debug ("org.kde.StatusNotifierWatcher already owned elsewhere; falling back to host-only mode");
  /*
   * Extra ref released by on_host_proxy_ready(); keeps self alive until the
   * proxy is created, since it can be destroyed mid-construction (e.g. during a
   * tile rearrangement).
   */
  g_dbus_proxy_new (self->connection, G_DBUS_PROXY_FLAGS_NONE, NULL,
                     WATCHER_BUS_NAME, WATCHER_OBJECT_PATH, WATCHER_IFACE,
                     NULL, on_host_proxy_ready, g_object_ref (self));
}



static void
on_bus_acquired (GDBusConnection *connection, const char *name, gpointer user_data)
{
  PhoshSniWatcher *self = PHOSH_SNI_WATCHER (user_data);
  g_autoptr (GError) error = NULL;

  self->connection = g_object_ref (connection);
  (void) name;
  (void) error;
}


static void
on_name_acquired (GDBusConnection *connection, const char *name, gpointer user_data)
{
  PhoshSniWatcher *self = PHOSH_SNI_WATCHER (user_data);
  g_autoptr (GError) error = NULL;

  self->is_watcher = TRUE;

  self->watcher_reg_id = g_dbus_connection_register_object (connection, WATCHER_OBJECT_PATH,
                                                              self->introspection->interfaces[0],
                                                              &watcher_vtable, self, NULL, &error);
  if (self->watcher_reg_id == 0)
    g_warning ("Failed to export StatusNotifierWatcher: %s", error->message);
  else
    g_debug ("Became org.kde.StatusNotifierWatcher");

  (void) name;
}


static void
on_name_lost (GDBusConnection *connection, const char *name, gpointer user_data)
{
  PhoshSniWatcher *self = PHOSH_SNI_WATCHER (user_data);

  /*
   * Fires if someone else owns the name (connection != NULL) and if the bus
   * connection itself failed (connection == NULL). Only the former falls back
   * to host-only mode.
   */
  if (connection && !self->is_watcher)
    phosh_sni_watcher_become_host_only (self);

  (void) name;
}



static void
phosh_sni_watcher_dispose (GObject *object)
{
  PhoshSniWatcher *self = PHOSH_SNI_WATCHER (object);

  /*
   * Cleanup is in dispose(), with no extra reference guarding the
   * g_bus_own_name() callbacks. They can fire for the subscription's whole
   * life, so a guarding ref would only be released by g_bus_unown_name() in
   * finalize(), which cannot run while that ref keeps the refcount above zero.
   * dispose() runs when the last external reference is dropped, and
   * g_bus_unown_name() guarantees no further callbacks once called.
   */
  if (self->own_name_id) {
    g_bus_unown_name (self->own_name_id);
    self->own_name_id = 0;
  }
  if (self->watcher_reg_id && self->connection) {
    g_dbus_connection_unregister_object (self->connection, self->watcher_reg_id);
    self->watcher_reg_id = 0;
  }

  /*
   * Each tracked item's g_bus_watch_name_on_connection() watch (see
   * phosh_sni_watcher_track_item()) is a repeated-callback subscription like
   * the name ownership above, so it is also cleared in dispose(). Clearing is
   * triggered by self->items's destroy-notify (watched_item_free()).
   */
  g_clear_pointer (&self->items, g_hash_table_unref);

  G_OBJECT_CLASS (phosh_sni_watcher_parent_class)->dispose (object);
}


static void
phosh_sni_watcher_finalize (GObject *object)
{
  PhoshSniWatcher *self = PHOSH_SNI_WATCHER (object);

  g_clear_pointer (&self->introspection, g_dbus_node_info_unref);
  g_clear_object (&self->watcher_proxy);
  g_clear_object (&self->connection);

  G_OBJECT_CLASS (phosh_sni_watcher_parent_class)->finalize (object);
}


static void
phosh_sni_watcher_class_init (PhoshSniWatcherClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = phosh_sni_watcher_dispose;
  object_class->finalize = phosh_sni_watcher_finalize;

  signals[ITEM_ADDED] = g_signal_new ("item-added", G_TYPE_FROM_CLASS (klass),
                                       G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                       G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);
  signals[ITEM_REMOVED] = g_signal_new ("item-removed", G_TYPE_FROM_CLASS (klass),
                                         G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                         G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);
}


static void
phosh_sni_watcher_init (PhoshSniWatcher *self)
{
  g_autoptr (GError) error = NULL;

  self->items = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                        (GDestroyNotify) watched_item_free);
  self->introspection = g_dbus_node_info_new_for_xml (watcher_introspection_xml, &error);
  if (!self->introspection)
    g_error ("Invalid embedded StatusNotifierWatcher introspection XML: %s", error->message);

  /*
   * G_BUS_NAME_OWNER_FLAGS_NONE: no replacement requested or allowed; first
   * come, first served, as a singleton Watcher requires.
   */
  self->own_name_id = g_bus_own_name (G_BUS_TYPE_SESSION, WATCHER_BUS_NAME,
                                       G_BUS_NAME_OWNER_FLAGS_NONE,
                                       on_bus_acquired, on_name_acquired, on_name_lost,
                                       self, NULL);
}


static PhoshSniWatcher *default_watcher = NULL;

PhoshSniWatcher *
phosh_sni_watcher_get_default (void)
{
  if (!default_watcher) {
    default_watcher = g_object_new (PHOSH_TYPE_SNI_WATCHER, NULL);
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
phosh_sni_watcher_replay_existing_items (PhoshSniWatcher *self)
{
  /*
   * self->items is populated only in Watcher mode (see
   * watcher_handle_register_item() -> phosh_sni_watcher_track_item()). In
   * host-only mode it stays empty, since host_add_existing_items() and
   * on_watcher_proxy_signal() emit "item-added" from the other watcher's cached
   * property. Both modes are handled explicitly.
   */
  if (self->is_watcher) {
    GHashTableIter iter;
    gpointer value;

    g_hash_table_iter_init (&iter, self->items);
    while (g_hash_table_iter_next (&iter, NULL, &value)) {
      WatchedItem *item = value;

      g_signal_emit (self, signals[ITEM_ADDED], 0, item->service, item->path);
    }
  } else {
    /*
     * Safe before the host-only proxy connection finishes (or if it never
     * does): a NULL or not-yet-cached property is handled.
     */
    host_add_existing_items (self);
  }
}


GDBusConnection *
phosh_sni_watcher_get_connection (PhoshSniWatcher *self)
{
  return self->connection;
}
