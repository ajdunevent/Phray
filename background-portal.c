/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "background-portal.h"
#include "wl-toplevel-tracker.h"

#define DBUS_NAME    "org.freedesktop.impl.portal.desktop.phray"
#define OBJECT_PATH  "/org/freedesktop/portal/desktop"
#define IFACE_NAME   "org.freedesktop.impl.portal.Background"

#define SETTINGS_SCHEMA "mobi.phosh.plugins.tray-quick-setting"
#define PROVIDE_BACKGROUND_PORTAL_KEY "provide-background-portal"

/*
 * Hand-written introspection XML, parsed with g_dbus_node_info_new_for_xml()
 * instead of gdbus-codegen, as in sni-watcher.c. Matches
 * org.freedesktop.impl.portal.Background.xml from xdg-desktop-portal.
 */
static const char *introspection_xml =
  "<node>"
  "  <interface name='org.freedesktop.impl.portal.Background'>"
  "    <method name='GetAppState'>"
  "      <arg type='a{sv}' name='apps' direction='out'/>"
  "    </method>"
  "    <method name='NotifyBackground'>"
  "      <arg type='o' name='handle' direction='in'/>"
  "      <arg type='s' name='app_id' direction='in'/>"
  "      <arg type='s' name='name' direction='in'/>"
  "      <arg type='u' name='response' direction='out'/>"
  "      <arg type='a{sv}' name='results' direction='out'/>"
  "    </method>"
  "    <method name='EnableAutostart'>"
  "      <arg type='s' name='app_id' direction='in'/>"
  "      <arg type='b' name='enable' direction='in'/>"
  "      <arg type='as' name='commandline' direction='in'/>"
  "      <arg type='u' name='flags' direction='in'/>"
  "      <arg type='b' name='result' direction='out'/>"
  "    </method>"
  "    <signal name='RunningApplicationsChanged'/>"
  "    <property name='version' type='u' access='read'/>"
  "  </interface>"
  "</node>";

struct _PhoshBackgroundPortal {
  GObject parent;

  GDBusConnection *connection;
  GDBusNodeInfo   *introspection;
  guint            own_name_id;
  guint            object_reg_id;

  PhoshWlToplevelTracker *toplevel_tracker;

  GHashTable *known_apps; /* app_ids reported via NotifyBackground */
};

G_DEFINE_TYPE (PhoshBackgroundPortal, phosh_background_portal, G_TYPE_OBJECT)


static GVariant *
build_app_state (PhoshBackgroundPortal *self)
{
  GVariantBuilder builder;
  GHashTableIter iter;
  gpointer key;
  g_autoptr (GPtrArray) tracked_app_ids =
    phosh_wl_toplevel_tracker_get_all_app_ids (self->toplevel_tracker);
  g_autoptr (GHashTable) seen = g_hash_table_new (g_str_hash, g_str_equal);

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));

  /*
   * Every app with a window is reported, whether or not it called
   * NotifyBackground. xdg-desktop-portal computes B = I - (I ∩ R), where R is
   * what this method reports; omitting a windowed app misclassifies it as an
   * unauthorized background app, which can lead to xdg-desktop-portal killing
   * it.
   */
  for (guint i = 0; i < tracked_app_ids->len; i++) {
    const char *app_id = g_ptr_array_index (tracked_app_ids, i);
    gboolean activated = FALSE;
    gboolean has_window = phosh_wl_toplevel_tracker_get_app_window_state (self->toplevel_tracker,
                                                                           app_id, &activated);
    guint32 state = has_window ? (activated ? 2 : 1) : 0;

    g_variant_builder_add (&builder, "{sv}", app_id, g_variant_new_uint32 (state));
    g_hash_table_add (seen, (gpointer) app_id);
  }

  /* Remaining NotifyBackground apps are running without a window. */
  g_hash_table_iter_init (&iter, self->known_apps);
  while (g_hash_table_iter_next (&iter, &key, NULL)) {
    const char *app_id = key;

    if (!g_hash_table_contains (seen, app_id))
      g_variant_builder_add (&builder, "{sv}", app_id, g_variant_new_uint32 (0));
  }

  return g_variant_builder_end (&builder);
}


static void
emit_running_applications_changed (PhoshBackgroundPortal *self)
{
  if (!self->connection)
    return;

  g_dbus_connection_emit_signal (self->connection, NULL, OBJECT_PATH, IFACE_NAME,
                                  "RunningApplicationsChanged", NULL, NULL);
}


static void
on_toplevel_changed (PhoshWlToplevelTracker *tracker, gpointer user_data)
{
  emit_running_applications_changed (PHOSH_BACKGROUND_PORTAL (user_data));
  (void) tracker;
}


static void
on_method_call (GDBusConnection       *connection,
                 const char            *sender,
                 const char            *object_path,
                 const char            *interface_name,
                 const char            *method_name,
                 GVariant              *parameters,
                 GDBusMethodInvocation *invocation,
                 gpointer               user_data)
{
  PhoshBackgroundPortal *self = PHOSH_BACKGROUND_PORTAL (user_data);

  if (g_strcmp0 (method_name, "GetAppState") == 0) {
    /*
     * Not g_autoptr: build_app_state() returns a floating GVariant, and
     * "@a{sv}" consumes it. g_autoptr would unref it a second time, causing
     * g_atomic_ref_count_dec assertion failures on every GetAppState call.
     */
    GVariant *apps = build_app_state (self);

    g_dbus_method_invocation_return_value (invocation, g_variant_new ("(@a{sv})", apps));
  } else if (g_strcmp0 (method_name, "NotifyBackground") == 0) {
    const char *handle, *app_id, *name;
    GVariantBuilder results;

    g_variant_get (parameters, "(&o&s&s)", &handle, &app_id, &name);

    if (!g_hash_table_contains (self->known_apps, app_id))
      g_hash_table_add (self->known_apps, g_strdup (app_id));

    g_variant_builder_init (&results, G_VARIANT_TYPE ("a{sv}"));
    g_dbus_method_invocation_return_value (invocation,
                                            g_variant_new ("(u@a{sv})", 1,
                                                            g_variant_builder_end (&results)));

    emit_running_applications_changed (self);
    (void) handle;
    (void) name;
  } else if (g_strcmp0 (method_name, "EnableAutostart") == 0) {
    /*
     * Deprecated: xdg-desktop-portal no longer calls it. Stubbed for interface
     * completeness.
     */
    g_dbus_method_invocation_return_value (invocation, g_variant_new ("(b)", TRUE));
  } else {
    g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                                            "Unknown method %s", method_name);
  }

  (void) connection;
  (void) sender;
  (void) object_path;
  (void) interface_name;
}


static GVariant *
on_get_property (GDBusConnection  *connection,
                  const char       *sender,
                  const char       *object_path,
                  const char       *interface_name,
                  const char       *property_name,
                  GError          **error,
                  gpointer          user_data)
{
  if (g_strcmp0 (property_name, "version") == 0)
    return g_variant_new_uint32 (1);

  (void) connection;
  (void) sender;
  (void) object_path;
  (void) interface_name;
  (void) error;
  (void) user_data;
  return NULL;
}


static const GDBusInterfaceVTable portal_vtable = {
  .method_call = on_method_call,
  .get_property = on_get_property,
  .set_property = NULL,
};


static void
on_bus_acquired (GDBusConnection *connection, const char *name, gpointer user_data)
{
  PhoshBackgroundPortal *self = PHOSH_BACKGROUND_PORTAL (user_data);
  g_autoptr (GError) error = NULL;

  self->connection = g_object_ref (connection);

  self->object_reg_id = g_dbus_connection_register_object (connection, OBJECT_PATH,
                                                             self->introspection->interfaces[0],
                                                             &portal_vtable, self, NULL, &error);
  if (self->object_reg_id == 0)
    g_warning ("Failed to export %s: %s", IFACE_NAME, error->message);

  (void) name;
}


static void
on_name_acquired (GDBusConnection *connection, const char *name, gpointer user_data)
{
  g_debug ("Owning %s for the Background portal backend", name);
  (void) connection;
  (void) user_data;
}


static void
on_name_lost (GDBusConnection *connection, const char *name, gpointer user_data)
{
  /*
   * No fallback role: unlike sni-watcher.c's Watcher/Host duality, a portal
   * backend that cannot own its name has no client-only mode, so the Background
   * portal stays unavailable.
   */
  g_warning ("Failed to own %s -- Background portal backend unavailable "
             "(another implementation may already own this name)", name);
  (void) connection;
  (void) user_data;
}


static void
phosh_background_portal_dispose (GObject *object)
{
  PhoshBackgroundPortal *self = PHOSH_BACKGROUND_PORTAL (object);

  /*
   * g_bus_own_name() callbacks can fire for the subscription's whole life, so
   * name ownership is released in dispose(), as soon as the last external
   * reference is dropped. finalize() essentially never runs because of the
   * singleton's permanent reference.
   */
  if (self->own_name_id) {
    g_bus_unown_name (self->own_name_id);
    self->own_name_id = 0;
  }
  if (self->object_reg_id && self->connection) {
    g_dbus_connection_unregister_object (self->connection, self->object_reg_id);
    self->object_reg_id = 0;
  }

  G_OBJECT_CLASS (phosh_background_portal_parent_class)->dispose (object);
}


static void
phosh_background_portal_finalize (GObject *object)
{
  PhoshBackgroundPortal *self = PHOSH_BACKGROUND_PORTAL (object);

  g_clear_pointer (&self->known_apps, g_hash_table_unref);
  g_clear_pointer (&self->introspection, g_dbus_node_info_unref);
  g_clear_object (&self->toplevel_tracker);
  g_clear_object (&self->connection);

  G_OBJECT_CLASS (phosh_background_portal_parent_class)->finalize (object);
}


static void
phosh_background_portal_class_init (PhoshBackgroundPortalClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = phosh_background_portal_dispose;
  object_class->finalize = phosh_background_portal_finalize;
}


static void
phosh_background_portal_init (PhoshBackgroundPortal *self)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GSettings) settings = g_settings_new (SETTINGS_SCHEMA);
  const char *current_desktop = g_getenv ("XDG_CURRENT_DESKTOP");

  self->known_apps = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  /*
   * XDG_CURRENT_DESKTOP is "Phosh:GNOME" in a Phosh session; the "Phosh" prefix
   * check tolerates changes to the rest of the value. Providing a Background
   * backend outside Phosh would be wrong. The check never writes to the saved
   * provide-background-portal setting.
   */
  if (!current_desktop || !g_str_has_prefix (current_desktop, "Phosh")) {
    g_debug ("XDG_CURRENT_DESKTOP (%s) doesn't start with \"Phosh\" -- not providing the "
             "Background portal backend regardless of the setting", current_desktop ? current_desktop : "(unset)");
    return;
  }

  /*
   * Read once at startup: toggling the D-Bus name ownership and Wayland
   * listener mid-session risks crashes. "Change requires Phosh restart" (see
   * the setting's subtitle in prefs.ui) is the intended behavior; a "changed"
   * watcher for this key must not be added.
   */
  if (!g_settings_get_boolean (settings, PROVIDE_BACKGROUND_PORTAL_KEY)) {
    g_debug ("Background portal backend disabled by setting");
    return;
  }

  /*
   * Other .portal files are not scanned. xdg-desktop-portal-gnome can be
   * installed and own its D-Bus name, but its Background implementation depends
   * on GNOME Shell window tracking, which is absent here, so a live backend
   * does not imply a working one. Phray registers its uniquely named service
   * and declares UseIn=phosh in phray.portal, leaving backend selection to
   * xdg-desktop-portal.
   */

  self->introspection = g_dbus_node_info_new_for_xml (introspection_xml, &error);
  if (!self->introspection)
    g_error ("Invalid embedded %s introspection XML: %s", IFACE_NAME, error->message);

  self->toplevel_tracker = phosh_wl_toplevel_tracker_new ();
  g_signal_connect_object (self->toplevel_tracker, "changed",
                            G_CALLBACK (on_toplevel_changed), self, 0);

  self->own_name_id = g_bus_own_name (G_BUS_TYPE_SESSION, DBUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
                                       on_bus_acquired, on_name_acquired, on_name_lost, self, NULL);
}


static PhoshBackgroundPortal *default_portal = NULL;

PhoshBackgroundPortal *
phosh_background_portal_get_default (void)
{
  if (!default_portal) {
    default_portal = g_object_new (PHOSH_TYPE_BACKGROUND_PORTAL, NULL);
    /* Resets default_portal to NULL if the object is ever finalized
     * (e.g. a module unload, which essentially never happens in normal
     * operation), so a later call here constructs a fresh instance
     * instead of returning a stale pointer. */
    g_object_add_weak_pointer (G_OBJECT (default_portal), (gpointer *) &default_portal);
  }

  return g_object_ref (default_portal);
}
