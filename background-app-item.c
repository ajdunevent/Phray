/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include "background-app-item.h"

#include <handy.h>
#include <glib/gi18n-lib.h>
#include <gio/gdesktopappinfo.h>

#define QUIT_FALLBACK_DELAY_SECONDS 3
#define ICON_PIXEL_SIZE 24 /* matches sni-item.c's ICON_PIXEL_SIZE; both icon types share the tray list */

/**
 * PhoshBackgroundAppMenuEntry:
 *
 * The fixed Show / Quit tokens for one PhoshBackgroundAppItem, plus a trailing
 * separator, built once and reused across expand/collapse cycles (nothing here
 * is fetched or changes). The separator matches SNI's
 * ensure_trailing_separator().
 */
#define PHOSH_TYPE_BACKGROUND_APP_MENU_ENTRY (phosh_background_app_menu_entry_get_type ())
G_DECLARE_FINAL_TYPE (PhoshBackgroundAppMenuEntry, phosh_background_app_menu_entry,
                       PHOSH, BACKGROUND_APP_MENU_ENTRY, GObject)

struct _PhoshBackgroundAppMenuEntry {
  GObject parent;

  PhoshBackgroundAppItem *owner; /* unowned; the owner outlives its entries */
  gboolean                is_quit; /* FALSE == Show; meaningless if is_separator */
  gboolean                is_separator;
};

G_DEFINE_TYPE (PhoshBackgroundAppMenuEntry, phosh_background_app_menu_entry, G_TYPE_OBJECT)

static void
phosh_background_app_menu_entry_class_init (PhoshBackgroundAppMenuEntryClass *klass)
{
  (void) klass;
}

static void
phosh_background_app_menu_entry_init (PhoshBackgroundAppMenuEntry *self)
{
  (void) self;
}


struct _PhoshBackgroundAppItem {
  GObject parent;

  GDBusConnection *connection;
  char            *instance;
  char            *app_id;
  GDesktopAppInfo *app_info; /* NULL if no matching .desktop file was found */
  char            *display_name; /* cached resolved title, for SNI-vs-background dedup matching */
  GSettings       *settings; /* mobi.phosh.plugins.tray-quick-setting; cached because update_display() runs for many items and on every icon-mode rebuild */

  gboolean expanded;

  GtkWidget *row;
  GtkWidget *icon;
  GtkWidget *chevron;

  /*
   * Set by the owner (untrack_bg_item() in tray-quick-setting.c) when it
   * splices this item's row out of flat_model, if the row was in the model:
   * gtk_list_box_bind_model() destroys the row widget as a side effect,
   * regardless of this item's reference. Without the flag, finalize() would
   * g_clear_object() the row again, a double release of freed memory.
   */
  gboolean row_destroyed_by_list_box;

  GPtrArray *menu_entries; /* [Show, Quit, separator], built once in init */
};

enum {
  MENU_EXPANDED,
  MENU_COLLAPSED,
  N_SIGNALS,
};
static guint signals[N_SIGNALS];

G_DEFINE_TYPE (PhoshBackgroundAppItem, phosh_background_app_item, G_TYPE_OBJECT)


static void
phosh_background_app_item_update_chevron (PhoshBackgroundAppItem *self)
{
  gtk_image_set_from_icon_name (GTK_IMAGE (self->chevron),
                                 self->expanded ? "pan-down-symbolic" : "pan-end-symbolic",
                                 GTK_ICON_SIZE_MENU);
}


gboolean
phosh_background_app_item_get_expanded (PhoshBackgroundAppItem *self)
{
  return self->expanded;
}


void
phosh_background_app_item_collapse_menu (PhoshBackgroundAppItem *self)
{
  if (!self->expanded)
    return;

  self->expanded = FALSE;
  phosh_background_app_item_update_chevron (self);
  g_signal_emit (self, signals[MENU_COLLAPSED], 0);
}


GPtrArray *
phosh_background_app_item_get_menu_entries (PhoshBackgroundAppItem *self)
{
  return self->menu_entries;
}


void
phosh_background_app_item_note_row_removed_from_list_box (PhoshBackgroundAppItem *self)
{
  self->row_destroyed_by_list_box = TRUE;
}


const char *
phosh_background_app_item_get_app_id (PhoshBackgroundAppItem *self)
{
  return self->app_id;
}



static void
phosh_background_app_item_show (PhoshBackgroundAppItem *self)
{
  g_autoptr (GError) error = NULL;

  if (!self->app_info)
    return;

  if (!g_app_info_launch (G_APP_INFO (self->app_info), NULL, NULL, &error))
    g_warning ("Failed to launch/raise %s: %s", self->app_id, error->message);
}



static gboolean
on_quit_fallback_timeout (gpointer user_data)
{
  g_autoptr (PhoshBackgroundAppItem) self = PHOSH_BACKGROUND_APP_ITEM (user_data); /* releases the ref taken where this timeout was scheduled */
  g_autoptr (GSubprocess) proc = NULL;
  g_autoptr (GError) error = NULL;

  /*
   * Unconditional: if the D-Bus quit already worked, `flatpak kill` against the
   * gone instance is a harmless no-op. Detecting tier-1 success would need
   * coordination with the watcher's "app-removed" timing.
   */
  proc = g_subprocess_new (G_SUBPROCESS_FLAGS_NONE, &error, "flatpak", "kill", self->instance, NULL);
  if (!proc)
    g_warning ("Failed to run flatpak kill %s: %s", self->instance, error->message);

  return G_SOURCE_REMOVE;
}


static void
phosh_background_app_item_quit (PhoshBackgroundAppItem *self)
{
  /*
   * Tier 1: best-effort graceful D-Bus quit. org.freedesktop.Application
   * specifies no universal Quit method; many GTK apps expose an app-level
   * "quit" GAction reachable this way, but not all do. The object path follows
   * the GApplication convention: app_id with '.' replaced by '/', prefixed with
   * '/'.
   */
  {
    g_autofree char *object_path = NULL;
    g_autofree char *dotted = g_strdup (self->app_id);
    GVariantBuilder empty_parameter;
    GVariantBuilder empty_platform_data;

    for (char *p = dotted; *p; p++) {
      if (*p == '.')
        *p = '/';
    }
    object_path = g_strdup_printf ("/%s", dotted);

    /*
     * The "av" and "a{sv}" arguments need builder-constructed values passed
     * with @-prefixed format specifiers; bare NULL is invalid GVariant
     * construction.
     */
    g_variant_builder_init (&empty_parameter, G_VARIANT_TYPE ("av"));
    g_variant_builder_init (&empty_platform_data, G_VARIANT_TYPE ("a{sv}"));

    g_dbus_connection_call (self->connection, self->app_id, object_path,
                             "org.freedesktop.Application", "ActivateAction",
                             g_variant_new ("(s@av@a{sv})", "quit",
                                            g_variant_builder_end (&empty_parameter),
                                            g_variant_builder_end (&empty_platform_data)),
                             NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
  }

  /*
   * Tier 2: `flatpak kill <instance>` instead of resolving a PID and signaling
   * it, since Flatpak handles its own process/cgroup management. The extra ref
   * is released by on_quit_fallback_timeout(): self could be destroyed (e.g. by
   * a settings-triggered hide/reconstruct) within the 3-second window.
   */
  g_timeout_add_seconds (QUIT_FALLBACK_DELAY_SECONDS, on_quit_fallback_timeout, g_object_ref (self));
}



GtkWidget *
phosh_background_app_menu_entry_build_row (GObject *entry_obj)
{
  PhoshBackgroundAppMenuEntry *entry = PHOSH_BACKGROUND_APP_MENU_ENTRY (entry_obj);
  GtkWidget *row;
  GtkWidget *indent_spacer;

  if (entry->is_separator) {
    /*
     * Matches sni-item.c's separator row (same thin blank gap, disabled row,
     * CSS class).
     */
    GtkWidget *spacer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

    row = gtk_list_box_row_new ();
    gtk_widget_set_size_request (spacer, -1, 3);
    gtk_container_add (GTK_CONTAINER (row), spacer);
    gtk_widget_set_sensitive (row, FALSE);
    gtk_style_context_add_class (gtk_widget_get_style_context (row), "phosh-tray-menu-row");
    gtk_widget_show_all (row);

    g_object_set_data (G_OBJECT (row), "bg-owner", entry->owner);
    g_object_set_data (G_OBJECT (row), "bg-is-menu-entry", GINT_TO_POINTER (TRUE));
    return row;
  }

  row = hdy_action_row_new ();
  indent_spacer = gtk_image_new_from_icon_name ("object-select-symbolic", GTK_ICON_SIZE_MENU);

  g_object_set (G_OBJECT (row), "title", entry->is_quit ? _("Quit") : _("Show"), NULL);
  g_object_set (G_OBJECT (row), "activatable", TRUE, NULL);
  gtk_style_context_add_class (gtk_widget_get_style_context (row), "phosh-tray-menu-row");

  /*
   * Always invisible (Show/Quit have no checked state), but added as a prefix
   * so the row reserves the same slot width as SNI's checkmark icon and indents
   * under its parent app like SNI's menu entries.
   */
  gtk_widget_set_visible (indent_spacer, FALSE);
  hdy_action_row_add_prefix (HDY_ACTION_ROW (row), indent_spacer);

  if (!entry->is_quit && !entry->owner->app_info)
    gtk_widget_set_sensitive (row, FALSE); /* nothing to launch without a resolved .desktop file */

  g_object_set_data (G_OBJECT (row), "bg-owner", entry->owner);
  g_object_set_data (G_OBJECT (row), "bg-is-quit", GINT_TO_POINTER (entry->is_quit));
  g_object_set_data (G_OBJECT (row), "bg-is-menu-entry", GINT_TO_POINTER (TRUE));
  gtk_widget_show (row);

  return row;
}


gboolean
phosh_background_app_menu_entry_is (GObject *obj)
{
  return PHOSH_IS_BACKGROUND_APP_MENU_ENTRY (obj);
}


static PhoshBackgroundAppMenuEntry *
make_entry (PhoshBackgroundAppItem *self, gboolean is_quit)
{
  PhoshBackgroundAppMenuEntry *entry = g_object_new (PHOSH_TYPE_BACKGROUND_APP_MENU_ENTRY, NULL);

  entry->owner = self;
  entry->is_quit = is_quit;
  return entry;
}


static PhoshBackgroundAppMenuEntry *
make_separator_entry (PhoshBackgroundAppItem *self)
{
  PhoshBackgroundAppMenuEntry *entry = g_object_new (PHOSH_TYPE_BACKGROUND_APP_MENU_ENTRY, NULL);

  entry->owner = self;
  entry->is_separator = TRUE;
  return entry;
}



/*
 * Apps' .desktop Icon= entries are typically a GThemedIcon with the app's
 * full-color name(s) as its fallback chain; using it unmodified is "full color"
 * (the default). "Symbolic" builds a new GThemedIcon that tries each name with
 * "-symbolic" appended first, falling back to the original names if the icon
 * theme has no symbolic variant.
 */
static GIcon *
build_icon_for_mode (GIcon *original, gboolean prefer_symbolic)
{
  const char * const *names;
  g_autoptr (GPtrArray) candidates = NULL;

  if (!prefer_symbolic || !original || !G_IS_THEMED_ICON (original))
    return original ? g_object_ref (original) : NULL;

  names = g_themed_icon_get_names (G_THEMED_ICON (original));
  if (!names)
    return g_object_ref (original);

  candidates = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; names[i]; i++) {
    if (!g_str_has_suffix (names[i], "-symbolic"))
      g_ptr_array_add (candidates, g_strdup_printf ("%s-symbolic", names[i]));
    g_ptr_array_add (candidates, g_strdup (names[i])); /* full-color fallback */
  }
  g_ptr_array_add (candidates, NULL);

  return g_themed_icon_new_from_names ((char **) candidates->pdata, -1);
}


static void
phosh_background_app_item_update_display (PhoshBackgroundAppItem *self)
{
  g_autofree char *desktop_id = g_strdup_printf ("%s.desktop", self->app_id);
  const char *title;

  g_clear_object (&self->app_info);
  self->app_info = g_desktop_app_info_new (desktop_id);

  if (self->app_info) {
    GIcon *raw_icon = g_app_info_get_icon (G_APP_INFO (self->app_info)); /* (transfer none) */

    if (raw_icon) {
      gboolean prefer_symbolic = g_settings_get_boolean (self->settings, "prefer-symbolic-icons");
      g_autoptr (GIcon) icon = build_icon_for_mode (raw_icon, prefer_symbolic);
      GtkStyleContext *icon_style = gtk_widget_get_style_context (self->icon);

      /*
       * Remove both classes first (this can run more than once per item, e.g.
       * on icon-mode toggling), then add the one matching the current
       * preference. See the CSS in tray-quick-setting.c for why the class is
       * dynamic.
       */
      gtk_style_context_remove_class (icon_style, "phosh-tray-app-icon-regular");
      gtk_style_context_remove_class (icon_style, "phosh-tray-app-icon-symbolic");
      gtk_style_context_add_class (icon_style, prefer_symbolic
                                                  ? "phosh-tray-app-icon-symbolic"
                                                  : "phosh-tray-app-icon-regular");

      gtk_image_set_from_gicon (GTK_IMAGE (self->icon), icon, GTK_ICON_SIZE_BUTTON);
      gtk_image_set_pixel_size (GTK_IMAGE (self->icon), ICON_PIXEL_SIZE);
    }
    title = g_app_info_get_display_name (G_APP_INFO (self->app_info));
  } else {
    /*
     * No matching .desktop file: fall back to the raw app_id instead of an
     * empty row.
     */
    gtk_image_set_from_icon_name (GTK_IMAGE (self->icon), "application-x-executable-symbolic",
                                   GTK_ICON_SIZE_BUTTON);
    gtk_image_set_pixel_size (GTK_IMAGE (self->icon), ICON_PIXEL_SIZE);
    title = self->app_id;
  }

  g_object_set (G_OBJECT (self->row), "title", title, NULL);
  g_free (self->display_name);
  self->display_name = g_strdup (title);
}


const char *
phosh_background_app_item_get_display_name (PhoshBackgroundAppItem *self)
{
  return self->display_name;
}


const char *
phosh_background_app_item_get_instance (PhoshBackgroundAppItem *self)
{
  return self->instance;
}


void
phosh_background_app_item_set_message (PhoshBackgroundAppItem *self, const char *message)
{
  hdy_action_row_set_subtitle (HDY_ACTION_ROW (self->row), message && *message ? message : NULL);
}



void
phosh_background_app_item_row_activated (PhoshBackgroundAppItem *self, GtkWidget *row,
                                          GtkListBox *list_box)
{
  if (row == self->row) {
    if (self->expanded)
      phosh_background_app_item_collapse_menu (self);
    else {
      self->expanded = TRUE;
      phosh_background_app_item_update_chevron (self);
      g_signal_emit (self, signals[MENU_EXPANDED], 0);
    }
    return;
  }

  {
    gboolean is_quit = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "bg-is-quit"));

    if (is_quit)
      phosh_background_app_item_quit (self);
    else
      phosh_background_app_item_show (self);
  }

  phosh_background_app_item_collapse_menu (self);
  (void) list_box;
}



GtkWidget *
phosh_background_app_item_get_row (PhoshBackgroundAppItem *self)
{
  return self->row;
}


static void
phosh_background_app_item_finalize (GObject *object)
{
  PhoshBackgroundAppItem *self = PHOSH_BACKGROUND_APP_ITEM (object);

  g_clear_object (&self->app_info);
  g_clear_object (&self->connection);
  g_clear_object (&self->settings);
  g_clear_pointer (&self->menu_entries, g_ptr_array_unref);
  g_free (self->display_name);
  g_free (self->instance);
  g_free (self->app_id);

  if (self->row_destroyed_by_list_box) {
    /*
     * gtk_list_box_bind_model() already destroyed this row when the owner
     * spliced flat_model, regardless of this item's reference; releasing it
     * again would unref freed memory. Only the pointer is dropped.
     */
    self->row = NULL;
  } else {
    g_clear_object (&self->row);
  }

  G_OBJECT_CLASS (phosh_background_app_item_parent_class)->finalize (object);
}


static void
phosh_background_app_item_class_init (PhoshBackgroundAppItemClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = phosh_background_app_item_finalize;

  signals[MENU_EXPANDED] = g_signal_new ("menu-expanded", G_TYPE_FROM_CLASS (klass),
                                          G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                          G_TYPE_NONE, 0);
  signals[MENU_COLLAPSED] = g_signal_new ("menu-collapsed", G_TYPE_FROM_CLASS (klass),
                                           G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                           G_TYPE_NONE, 0);
}


static void
phosh_background_app_item_init (PhoshBackgroundAppItem *self)
{
  /*
   * g_object_ref_sink() is required for the same
   * persistent-widget/floating-reference reason as in phosh_sni_item_init() in
   * sni-item.c; released in finalize().
   */
  self->row = g_object_ref_sink (hdy_action_row_new ());
  gtk_widget_set_visible (self->row, TRUE);
  g_object_set (G_OBJECT (self->row), "activatable", TRUE, NULL);

  self->icon = gtk_image_new ();
  gtk_widget_show (self->icon);
  hdy_action_row_add_prefix (HDY_ACTION_ROW (self->row), self->icon);

  self->chevron = gtk_image_new ();
  gtk_widget_show (self->chevron);
  gtk_container_add (GTK_CONTAINER (self->row), self->chevron);

  self->menu_entries = g_ptr_array_new_with_free_func (g_object_unref);
  g_ptr_array_add (self->menu_entries, make_entry (self, FALSE));
  g_ptr_array_add (self->menu_entries, make_entry (self, TRUE));
  g_ptr_array_add (self->menu_entries, make_separator_entry (self));

  self->settings = g_settings_new ("mobi.phosh.plugins.tray-quick-setting");

  phosh_background_app_item_update_chevron (self);
}


PhoshBackgroundAppItem *
phosh_background_app_item_new (GDBusConnection *connection, const char *instance,
                                const char *app_id)
{
  PhoshBackgroundAppItem *self = g_object_new (PHOSH_TYPE_BACKGROUND_APP_ITEM, NULL);

  self->connection = g_object_ref (connection);
  self->instance = g_strdup (instance);
  self->app_id = g_strdup (app_id);

  g_object_set_data (G_OBJECT (self->row), "bg-owner", self);

  phosh_background_app_item_update_display (self);

  return self;
}
