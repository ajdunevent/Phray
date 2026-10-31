/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sni-item.h"

#include <handy.h>

#define ITEM_IFACE "org.kde.StatusNotifierItem"
#define MENU_IFACE "com.canonical.dbusmenu"
#define ICON_PIXEL_SIZE 24

/**
 * PhoshSniMenuEntry:
 *
 * Cached DBusMenu data for one row, as a dedicated type so updates can be
 * pushed to its built row widget, if one exists (via setter functions and a
 * weak pointer, since each entry has one row).
 *
 * DBusMenu items change dynamically (Play/Pause toggling, "Sync Now"
 * enabling/disabling) and the protocol pushes updates: "ItemsPropertiesUpdated"
 * (label/enabled/toggle-state changes) updates the entry in place and pushes to
 * its row if built, with no model mutation; "LayoutUpdated" (structural
 * changes) refetches and splices the whole set.
 */

#define PHOSH_TYPE_SNI_MENU_ENTRY (phosh_sni_menu_entry_get_type ())
G_DECLARE_FINAL_TYPE (PhoshSniMenuEntry, phosh_sni_menu_entry, PHOSH, SNI_MENU_ENTRY, GObject)

struct _PhoshSniMenuEntry {
  GObject parent;

  PhoshSniItem *owner; /* unowned; the owner outlives its entries */
  gint32        id;
  gboolean      is_separator;
  char         *label;     /* already tree-indented/mnemonic-stripped display text */
  gboolean      sensitive;
  gboolean      checked;

  GtkWidget *row; /* weak pointer, NULL until phosh_sni_menu_entry_build_row() runs; auto-cleared when the row dies */
};

G_DEFINE_TYPE (PhoshSniMenuEntry, phosh_sni_menu_entry, G_TYPE_OBJECT)

struct _PhoshSniItem {
  GObject parent;

  GDBusConnection *connection;
  char            *service;
  char            *path;

  GDBusProxy *proxy;      /* org.kde.StatusNotifierItem, on (service, path) */
  GDBusProxy *menu_proxy; /* com.canonical.dbusmenu, lazily created */
  char       *menu_path;  /* cached "Menu" property, NULL if item has none */
  gboolean    fetch_started;
  gboolean    expanded;

  GtkWidget *row;     /* HdyActionRow; owner inserts this into its list */
  GtkWidget *icon;    /* prefix image, covers both IconName and decoded IconPixmap */
  GtkWidget *chevron; /* suffix disclosure indicator, added once a menu is found */

  /*
   * Set by the owner (untrack_sni_item() in tray-quick-setting.c) when it
   * splices this item's row out of flat_model, if the row was in the model. See
   * the row_destroyed_by_list_box comment in background-app-item.c.
   */
  gboolean row_destroyed_by_list_box;

  char *title; /* cached row title, for SNI-vs-background dedup matching (see tray-quick-setting.c) */
  char *id;    /* cached raw Id property; stable identifier for per-app filtering and the prefs dialog (title can change) */

  GPtrArray *menu_entries; /* PhoshSniMenuEntry*, cached once fetched; NULL until then */
};

enum {
  MENU_EXPANDED,
  MENU_READY,
  MENU_COLLAPSED,
  MENU_STRUCTURE_CHANGED,
  TITLE_CHANGED,
  N_SIGNALS,
};
static guint signals[N_SIGNALS];

G_DEFINE_TYPE (PhoshSniItem, phosh_sni_item, G_TYPE_OBJECT)


/* PhoshSniMenuEntry */

static void
phosh_sni_menu_entry_finalize (GObject *object)
{
  PhoshSniMenuEntry *self = PHOSH_SNI_MENU_ENTRY (object);

  if (self->row)
    g_object_remove_weak_pointer (G_OBJECT (self->row), (gpointer *) &self->row);
  g_free (self->label);

  G_OBJECT_CLASS (phosh_sni_menu_entry_parent_class)->finalize (object);
}


static void
phosh_sni_menu_entry_class_init (PhoshSniMenuEntryClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = phosh_sni_menu_entry_finalize;
}


static void
phosh_sni_menu_entry_init (PhoshSniMenuEntry *self)
{
  (void) self;
}


static PhoshSniMenuEntry *
phosh_sni_menu_entry_new (PhoshSniItem *owner, gboolean is_separator, gint32 id,
                           const char *label, gboolean sensitive, gboolean checked)
{
  PhoshSniMenuEntry *self = g_object_new (PHOSH_TYPE_SNI_MENU_ENTRY, NULL);

  self->owner = owner;
  self->id = id;
  self->is_separator = is_separator;
  self->label = g_strdup (label ? label : "");
  self->sensitive = sensitive;
  self->checked = checked;

  return self;
}


static void
phosh_sni_menu_entry_set_label (PhoshSniMenuEntry *self, const char *label)
{
  g_free (self->label);
  self->label = g_strdup (label ? label : "");
  if (self->row)
    g_object_set (self->row, "title", self->label, NULL);
}


static void
phosh_sni_menu_entry_set_sensitive (PhoshSniMenuEntry *self, gboolean sensitive)
{
  self->sensitive = sensitive;
  if (self->row)
    gtk_widget_set_sensitive (self->row, sensitive);
}


static void
phosh_sni_menu_entry_set_checked (PhoshSniMenuEntry *self, gboolean checked)
{
  self->checked = checked;
  if (self->row) {
    GtkWidget *check = g_object_get_data (G_OBJECT (self->row), "sni-check-image");

    if (check)
      gtk_widget_set_visible (check, checked);
  }
}



/*
 * IconPixmap is "a(iiay)": (width, height, ARGB32 pixel data in network byte
 * order). Network byte order lays each pixel out as A,R,G,B while GdkPixbuf
 * wants R,G,B,A, so each pixel is byte-shuffled, not just endian-flipped. The
 * largest offered size is picked and GTK downscales it to ICON_PIXEL_SIZE.
 */
static GdkPixbuf *
decode_icon_pixmap (GVariant *pixmap_array)
{
  GVariantIter iter;
  gint32 width, height;
  GVariant *bytes_v;
  GdkPixbuf *best = NULL;
  gint32 best_width = -1;

  g_variant_iter_init (&iter, pixmap_array);
  while (g_variant_iter_next (&iter, "(ii@ay)", &width, &height, &bytes_v)) {
    gsize len = 0;
    const guint8 *data;

    if (width <= best_width || width <= 0 || height <= 0) {
      g_variant_unref (bytes_v);
      continue;
    }

    data = g_variant_get_fixed_array (bytes_v, &len, 1);
    if (len >= (gsize) (width * height * 4)) {
      guint8 *rgba = g_malloc ((gsize) width * height * 4);

      for (gsize i = 0; i < (gsize) width * height; i++) {
        const guint8 *px = data + i * 4;
        guint8 *out = rgba + i * 4;

        out[0] = px[1]; /* R */
        out[1] = px[2]; /* G */
        out[2] = px[3]; /* B */
        out[3] = px[0]; /* A */
      }

      g_clear_object (&best);
      best = gdk_pixbuf_new_from_data (rgba, GDK_COLORSPACE_RGB, TRUE, 8,
                                        width, height, width * 4,
                                        (GdkPixbufDestroyNotify) g_free, NULL);
      best_width = width;
    }
    g_variant_unref (bytes_v);
  }

  return best;
}


/*
 * `props`, if non-NULL, is a full properties dict (from an explicit
 * Properties.GetAll) read instead of the proxy's cache: many StatusNotifierItem
 * implementations never emit standard PropertiesChanged and use legacy New*
 * signals (see on_item_legacy_signal()), which GDBusProxy's cache does not
 * react to. NULL is only safe right after construction, when the cache from the
 * construction-time GetAll is still accurate.
 */
static void
phosh_sni_item_update_icon (PhoshSniItem *self, GVariant *props)
{
  g_autoptr (GVariant) icon_name_v = NULL;
  const char *icon_name = NULL;

  icon_name_v = props
    ? g_variant_lookup_value (props, "IconName", G_VARIANT_TYPE_STRING)
    : g_dbus_proxy_get_cached_property (self->proxy, "IconName");
  if (icon_name_v)
    icon_name = g_variant_get_string (icon_name_v, NULL);

  if (icon_name && *icon_name) {
    gtk_image_set_from_icon_name (GTK_IMAGE (self->icon), icon_name, GTK_ICON_SIZE_BUTTON);
    gtk_image_set_pixel_size (GTK_IMAGE (self->icon), ICON_PIXEL_SIZE);
    return;
  }

  {
    g_autoptr (GVariant) pixmap_v = props
      ? g_variant_lookup_value (props, "IconPixmap", NULL)
      : g_dbus_proxy_get_cached_property (self->proxy, "IconPixmap");

    if (pixmap_v) {
      g_autoptr (GdkPixbuf) pixbuf = decode_icon_pixmap (pixmap_v);

      if (pixbuf) {
        g_autoptr (GdkPixbuf) scaled = gdk_pixbuf_scale_simple (pixbuf, ICON_PIXEL_SIZE,
                                                                  ICON_PIXEL_SIZE, GDK_INTERP_BILINEAR);
        gtk_image_set_from_pixbuf (GTK_IMAGE (self->icon), scaled);
        return;
      }
    }
  }

  gtk_image_set_from_icon_name (GTK_IMAGE (self->icon), "image-missing-symbolic",
                                 GTK_ICON_SIZE_BUTTON);
  gtk_image_set_pixel_size (GTK_IMAGE (self->icon), ICON_PIXEL_SIZE);
}


static void
phosh_sni_item_update_title (PhoshSniItem *self, GVariant *props)
{
  g_autoptr (GVariant) title_v = props
    ? g_variant_lookup_value (props, "Title", G_VARIANT_TYPE_STRING)
    : g_dbus_proxy_get_cached_property (self->proxy, "Title");
  g_autoptr (GVariant) id_v = props
    ? g_variant_lookup_value (props, "Id", G_VARIANT_TYPE_STRING)
    : g_dbus_proxy_get_cached_property (self->proxy, "Id");
  const char *text = NULL;

  if (title_v && g_variant_get_string (title_v, NULL)[0] != '\0')
    text = g_variant_get_string (title_v, NULL);
  else if (id_v)
    text = g_variant_get_string (id_v, NULL);

  g_free (self->title);
  self->title = g_strdup (text ? text : "");
  g_object_set (G_OBJECT (self->row), "title", self->title, NULL);

  g_free (self->id);
  self->id = g_strdup (id_v ? g_variant_get_string (id_v, NULL) : "");

  g_signal_emit (self, signals[TITLE_CHANGED], 0);
}


const char *
phosh_sni_item_get_id (PhoshSniItem *self)
{
  return self->id;
}


/*
 * service and path let the owner's destroy-and-rebuild hide/show mechanism (see
 * tray-quick-setting.c) reconstruct an equivalent item via
 * phosh_sni_item_new().
 */
const char *
phosh_sni_item_get_service (PhoshSniItem *self)
{
  return self->service;
}


const char *
phosh_sni_item_get_path (PhoshSniItem *self)
{
  return self->path;
}


const char *
phosh_sni_item_get_title (PhoshSniItem *self)
{
  return self->title;
}



static void
phosh_sni_item_activate (PhoshSniItem *self)
{
  g_dbus_proxy_call (self->proxy, "Activate", g_variant_new ("(ii)", 0, 0),
                      G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}


static void
phosh_sni_item_update_chevron (PhoshSniItem *self)
{
  gtk_image_set_from_icon_name (GTK_IMAGE (self->chevron),
                                 self->expanded ? "pan-down-symbolic" : "pan-end-symbolic",
                                 GTK_ICON_SIZE_MENU);
}


gboolean
phosh_sni_item_get_expanded (PhoshSniItem *self)
{
  return self->expanded;
}


void
phosh_sni_item_collapse_menu (PhoshSniItem *self)
{
  if (!self->expanded)
    return;

  self->expanded = FALSE;
  phosh_sni_item_update_chevron (self);
  g_signal_emit (self, signals[MENU_COLLAPSED], 0);
}


GPtrArray *
phosh_sni_item_get_menu_entries (PhoshSniItem *self)
{
  return self->menu_entries;
}


void
phosh_sni_item_note_row_removed_from_list_box (PhoshSniItem *self)
{
  self->row_destroyed_by_list_box = TRUE;
}



static char *
strip_mnemonic (const char *label)
{
  GString *out = g_string_new (NULL);

  for (const char *p = label; *p; p++) {
    if (*p == '_' && *(p + 1) != '\0')
      continue;
    g_string_append_c (out, *p);
  }
  return g_string_free (out, FALSE);
}


static void
phosh_sni_item_menu_event (PhoshSniItem *self, gint32 id)
{
  if (!self->menu_proxy)
    return;

  g_dbus_proxy_call (self->menu_proxy, "Event",
                      g_variant_new ("(isvu)", id, "clicked", g_variant_new_string (""), 0),
                      G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}


/*
 * Not every app's DBusMenu ends with a separator; one is added if missing, so
 * there is always a visual break between an app's menu entries and whatever
 * follows in the tray list.
 */
static void
ensure_trailing_separator (PhoshSniItem *self, GPtrArray *entries)
{
  PhoshSniMenuEntry *last;

  if (entries->len == 0)
    return;

  last = g_ptr_array_index (entries, entries->len - 1);
  if (last->is_separator)
    return;

  g_ptr_array_add (entries, phosh_sni_menu_entry_new (self, TRUE, -1, NULL, FALSE, FALSE));
}


static void
build_menu_entry_tokens (PhoshSniItem *self, GVariant *layout_tuple, int depth, GPtrArray *out)
{
  gint32 id;
  g_autoptr (GVariant) props = NULL;
  g_autoptr (GVariant) children = NULL;
  const char *type = NULL;
  const char *label = NULL;
  gboolean visible = TRUE;
  gboolean enabled = TRUE;
  const char *toggle_type = NULL;
  gint32 toggle_state = -1;
  /*
   * Indentation is two spaces per depth level, padded into the label text
   * instead of a row margin.
   */
  g_autofree char *pad = g_strnfill (depth < 0 ? 0 : depth * 2, ' ');

  /*
   * Each child in the parent's "av" array is a variant wrapping this (ia{sv}av)
   * tuple and must be unboxed before use.
   */
  g_variant_get (layout_tuple, "(i@a{sv}@av)", &id, &props, &children);

  if (depth >= 0) {
    g_variant_lookup (props, "visible", "b", &visible);
    if (visible) {
      g_variant_lookup (props, "type", "&s", &type);
      g_variant_lookup (props, "label", "&s", &label);
      g_variant_lookup (props, "enabled", "b", &enabled);
      g_variant_lookup (props, "toggle-type", "&s", &toggle_type);
      g_variant_lookup (props, "toggle-state", "i", &toggle_state);

      if (g_strcmp0 (type, "separator") == 0) {
        g_ptr_array_add (out, phosh_sni_menu_entry_new (self, TRUE, id, NULL, FALSE, FALSE));
      } else if (label) {
        g_autofree char *clean_label = strip_mnemonic (label);
        g_autofree char *rendered = g_strconcat (pad, clean_label, NULL);
        gboolean checked = toggle_type && *toggle_type && toggle_state == 1;

        g_ptr_array_add (out, phosh_sni_menu_entry_new (self, FALSE, id, rendered, enabled, checked));
      }
    }
  }

  {
    GVariantIter iter;
    GVariant *child_boxed;

    g_variant_iter_init (&iter, children);
    while (g_variant_iter_next (&iter, "v", &child_boxed)) {
      build_menu_entry_tokens (self, child_boxed, depth + 1, out);
      g_variant_unref (child_boxed);
    }
  }
}


GtkWidget *
phosh_sni_menu_entry_build_row (GObject *entry_obj)
{
  PhoshSniMenuEntry *entry = PHOSH_SNI_MENU_ENTRY (entry_obj);

  if (entry->is_separator) {
    GtkWidget *row = gtk_list_box_row_new ();
    GtkWidget *spacer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

    /* Separators are a thin blank gap, not a label-driven row. */
    gtk_widget_set_size_request (spacer, -1, 3);
    gtk_container_add (GTK_CONTAINER (row), spacer);
    gtk_widget_set_sensitive (row, FALSE);
    gtk_style_context_add_class (gtk_widget_get_style_context (row), "phosh-tray-menu-row");
    gtk_widget_show_all (row);

    g_object_set_data (G_OBJECT (row), "sni-owner", entry->owner);
    g_object_set_data (G_OBJECT (row), "sni-is-menu-entry", GINT_TO_POINTER (TRUE));

    entry->row = row;
    g_object_add_weak_pointer (G_OBJECT (row), (gpointer *) &entry->row);
    return row;
  }

  {
    GtkWidget *row = hdy_action_row_new ();
    GtkWidget *check = gtk_image_new_from_icon_name ("object-select-symbolic", GTK_ICON_SIZE_MENU);

    g_object_set (G_OBJECT (row), "title", entry->label, NULL);
    g_object_set (G_OBJECT (row), "activatable", TRUE, NULL);
    gtk_widget_set_sensitive (row, entry->sensitive);
    gtk_style_context_add_class (gtk_widget_get_style_context (row), "phosh-tray-menu-row");

    /*
     * The checkmark is always created, visible or not per current state, so
     * phosh_sni_menu_entry_set_checked() can look it up by this data key and
     * update it without rebuilding the row.
     */
    gtk_widget_set_visible (check, entry->checked);
    hdy_action_row_add_prefix (HDY_ACTION_ROW (row), check);
    g_object_set_data (G_OBJECT (row), "sni-check-image", check);

    g_object_set_data (G_OBJECT (row), "sni-owner", entry->owner);
    g_object_set_data (G_OBJECT (row), "sni-menu-id", GINT_TO_POINTER (entry->id));
    g_object_set_data (G_OBJECT (row), "sni-is-menu-entry", GINT_TO_POINTER (TRUE));
    gtk_widget_show (row);

    entry->row = row;
    g_object_add_weak_pointer (G_OBJECT (row), (gpointer *) &entry->row);
    return row;
  }
}


gboolean
phosh_sni_menu_entry_is (GObject *obj)
{
  return PHOSH_IS_SNI_MENU_ENTRY (obj);
}


static PhoshSniMenuEntry *
find_entry_by_id (PhoshSniItem *self, gint32 id)
{
  if (!self->menu_entries)
    return NULL;

  for (guint i = 0; i < self->menu_entries->len; i++) {
    PhoshSniMenuEntry *entry = g_ptr_array_index (self->menu_entries, i);

    if (!entry->is_separator && entry->id == id)
      return entry;
  }
  return NULL;
}



static void
on_items_properties_updated (PhoshSniItem *self, GVariant *parameters)
{
  GVariantIter *updated_iter = NULL;
  GVariantIter *removed_iter = NULL;
  gint32 id;
  GVariant *props;
  const char **removed_names;

  g_variant_get (parameters, "(a(ia{sv})a(ias))", &updated_iter, &removed_iter);

  while (g_variant_iter_next (updated_iter, "(i@a{sv})", &id, &props)) {
    PhoshSniMenuEntry *entry = find_entry_by_id (self, id);

    if (entry) {
      const char *label = NULL;
      gboolean enabled;
      gint32 toggle_state;

      if (g_variant_lookup (props, "label", "&s", &label)) {
        g_autofree char *clean_label = strip_mnemonic (label);
        phosh_sni_menu_entry_set_label (entry, clean_label);
      }
      if (g_variant_lookup (props, "enabled", "b", &enabled))
        phosh_sni_menu_entry_set_sensitive (entry, enabled);
      if (g_variant_lookup (props, "toggle-state", "i", &toggle_state))
        phosh_sni_menu_entry_set_checked (entry, toggle_state == 1);
    }
    g_variant_unref (props);
  }
  g_variant_iter_free (updated_iter);

  /* Removed properties revert to their spec-defined defaults. */
  while (g_variant_iter_next (removed_iter, "(i^a&s)", &id, &removed_names)) {
    PhoshSniMenuEntry *entry = find_entry_by_id (self, id);

    if (entry) {
      for (guint i = 0; removed_names[i]; i++) {
        if (g_strcmp0 (removed_names[i], "enabled") == 0)
          phosh_sni_menu_entry_set_sensitive (entry, TRUE);
        else if (g_strcmp0 (removed_names[i], "toggle-state") == 0)
          phosh_sni_menu_entry_set_checked (entry, FALSE);
      }
    }
    g_free (removed_names);
  }
  g_variant_iter_free (removed_iter);
}


static void on_layout_refetch_ready (GObject *source, GAsyncResult *result, gpointer user_data);


static void
on_layout_updated (PhoshSniItem *self)
{
  GVariantBuilder empty_names;

  g_variant_builder_init (&empty_names, G_VARIANT_TYPE ("as"));
  /*
   * Extra ref released by on_layout_refetch_ready(). g_signal_connect_object()
   * only guarantees this handler is not called after self is destroyed; it does
   * not protect the async operation started here.
   */
  g_dbus_proxy_call (self->menu_proxy, "GetLayout",
                      g_variant_new ("(iias)", 0, -1, &empty_names),
                      G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_layout_refetch_ready, g_object_ref (self));
}


static void
on_layout_refetch_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr (PhoshSniItem) self = PHOSH_SNI_ITEM (user_data); /* releases the ref taken in on_layout_updated() */
  guint old_count = self->menu_entries ? self->menu_entries->len : 0;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) reply = NULL;
  g_autoptr (GVariant) layout = NULL;
  guint32 revision;

  reply = g_dbus_proxy_call_finish (self->menu_proxy, result, &error);
  if (!reply) {
    g_warning ("GetLayout refetch failed for %s%s: %s", self->service, self->path, error->message);
    return;
  }

  g_variant_get (reply, "(u@(ia{sv}av))", &revision, &layout);

  g_clear_pointer (&self->menu_entries, g_ptr_array_unref);
  self->menu_entries = g_ptr_array_new_with_free_func (g_object_unref);
  build_menu_entry_tokens (self, layout, -1, self->menu_entries);
  ensure_trailing_separator (self, self->menu_entries);

  /*
   * Structural change (LayoutUpdated): the owner splice-replaces its model;
   * old_count is the number of old rows to remove before inserting the new set.
   */
  g_signal_emit (self, signals[MENU_STRUCTURE_CHANGED], 0, old_count);
  (void) source;
  (void) revision;
}


static void
on_menu_proxy_signal (GDBusProxy *proxy, const char *sender_name, const char *signal_name,
                       GVariant *parameters, gpointer user_data)
{
  PhoshSniItem *self = PHOSH_SNI_ITEM (user_data);

  if (g_strcmp0 (signal_name, "ItemsPropertiesUpdated") == 0)
    on_items_properties_updated (self, parameters);
  else if (g_strcmp0 (signal_name, "LayoutUpdated") == 0)
    on_layout_updated (self);

  (void) proxy;
  (void) sender_name;
}


static void
on_get_layout_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr (PhoshSniItem) self = PHOSH_SNI_ITEM (user_data); /* releases the ref taken in call_get_layout() */
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) reply = NULL;
  g_autoptr (GVariant) layout = NULL;
  guint32 revision;

  reply = g_dbus_proxy_call_finish (self->menu_proxy, result, &error);
  if (!reply) {
    g_warning ("GetLayout failed for %s%s: %s", self->service, self->path, error->message);
    self->fetch_started = FALSE; /* allow a later retry, e.g. next properties-changed */
    return;
  }

  g_variant_get (reply, "(u@(ia{sv}av))", &revision, &layout);

  self->menu_entries = g_ptr_array_new_with_free_func (g_object_unref);
  build_menu_entry_tokens (self, layout, -1, self->menu_entries);
  ensure_trailing_separator (self, self->menu_entries);

  /*
   * Spliced into the owner's model once, here; later changes arrive via
   * ItemsPropertiesUpdated (in place) or LayoutUpdated (MENU_STRUCTURE_CHANGED
   * splice-replace).
   */
  g_signal_emit (self, signals[MENU_READY], 0);
  (void) source;
  (void) revision;
}


static void
call_get_layout (PhoshSniItem *self)
{
  GVariantBuilder empty_names;

  g_variant_builder_init (&empty_names, G_VARIANT_TYPE ("as"));
  /*
   * Extra ref released by on_get_layout_ready(); keeps self alive until the
   * async D-Bus call completes.
   */
  g_dbus_proxy_call (self->menu_proxy, "GetLayout",
                      g_variant_new ("(iias)", 0, -1, &empty_names),
                      G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_get_layout_ready, g_object_ref (self));
}


static void
on_menu_proxy_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr (PhoshSniItem) self = PHOSH_SNI_ITEM (user_data); /* releases the ref taken in phosh_sni_item_ensure_menu_fetched() */
  g_autoptr (GError) error = NULL;

  self->menu_proxy = g_dbus_proxy_new_finish (result, &error);
  if (!self->menu_proxy) {
    g_warning ("Failed to connect to DBusMenu for %s%s: %s", self->service, self->path,
               error->message);
    self->fetch_started = FALSE; /* allow a later retry, e.g. next properties-changed */
    return;
  }

  g_signal_connect_object (self->menu_proxy, "g-signal",
                            G_CALLBACK (on_menu_proxy_signal), self, 0);

  call_get_layout (self);
  (void) source;
}


static void
phosh_sni_item_ensure_menu_fetched (PhoshSniItem *self)
{
  if (self->fetch_started)
    return;
  self->fetch_started = TRUE;

  if (!self->menu_proxy) {
    /*
     * Async, since constructing the proxy synchronously would block the
     * compositor's main loop on a slow or unresponsive app. The extra ref is
     * released by on_menu_proxy_ready(); it keeps self alive if the item is
     * hidden/destroyed (e.g. by a settings toggle) before this completes.
     */
    g_dbus_proxy_new (self->connection, G_DBUS_PROXY_FLAGS_NONE, NULL,
                       self->service, self->menu_path, MENU_IFACE,
                       NULL, on_menu_proxy_ready, g_object_ref (self));
    return;
  }

  call_get_layout (self);
}


static void
phosh_sni_item_expand (PhoshSniItem *self)
{
  self->expanded = TRUE;
  phosh_sni_item_update_chevron (self);
  g_signal_emit (self, signals[MENU_EXPANDED], 0);
}


static void
phosh_sni_item_update_menu_path (PhoshSniItem *self, GVariant *props)
{
  g_autoptr (GVariant) menu_v = props
    ? g_variant_lookup_value (props, "Menu", G_VARIANT_TYPE_OBJECT_PATH)
    : g_dbus_proxy_get_cached_property (self->proxy, "Menu");

  g_clear_pointer (&self->menu_path, g_free);
  if (menu_v) {
    const char *path = g_variant_get_string (menu_v, NULL);
    if (path[0] == '/')
      self->menu_path = g_strdup (path);
  }

  if (self->menu_path) {
    if (!self->chevron) {
      self->chevron = gtk_image_new ();
      gtk_widget_show (self->chevron);
      gtk_container_add (GTK_CONTAINER (self->row), self->chevron);
      phosh_sni_item_update_chevron (self);
    }
    /*
     * Fetched eagerly instead of on tap, and subscribed to live updates (see
     * phosh_sni_item_ensure_menu_fetched()), so no polling is needed.
     */
    phosh_sni_item_ensure_menu_fetched (self);
  }
}


static void
on_item_properties_changed (GDBusProxy *proxy, GVariant *changed_properties,
                             GStrv invalidated_properties, gpointer user_data)
{
  PhoshSniItem *self = PHOSH_SNI_ITEM (user_data);

  /*
   * NULL instead of changed_properties: it is a partial dict and the update
   * functions treat a missing key as "property gone", so passing it would reset
   * unchanged fields. NULL uses the proxy's cache, into which GDBusProxy has
   * already merged the partial update for the standard PropertiesChanged path.
   */
  phosh_sni_item_update_icon (self, NULL);
  phosh_sni_item_update_title (self, NULL);
  phosh_sni_item_update_menu_path (self, NULL);

  (void) proxy;
  (void) changed_properties;
  (void) invalidated_properties;
}


static void
on_get_all_properties_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr (PhoshSniItem) self = PHOSH_SNI_ITEM (user_data); /* releases the ref taken in on_item_legacy_signal() */
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) reply = NULL;
  g_autoptr (GVariant) props = NULL;

  reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);
  if (!reply) {
    g_warning ("Failed to refresh properties for %s%s: %s", self->service, self->path,
               error->message);
    return;
  }

  g_variant_get (reply, "(@a{sv})", &props);

  /*
   * Unlike on_item_properties_changed()'s partial dict, this full GetAll
   * response is safe to use for all three.
   */
  phosh_sni_item_update_icon (self, props);
  phosh_sni_item_update_title (self, props);
  phosh_sni_item_update_menu_path (self, props);
}


static void
on_item_legacy_signal (GDBusProxy *proxy, const char *sender_name, const char *signal_name,
                        GVariant *parameters, gpointer user_data)
{
  PhoshSniItem *self = PHOSH_SNI_ITEM (user_data);

  /*
   * Many StatusNotifierItem implementations never emit standard
   * org.freedesktop.DBus.Properties.PropertiesChanged and use the SNI spec's
   * "something changed, re-fetch" signals instead. GDBusProxy's cache only
   * updates on the standard signal, so an explicit Properties.GetAll is done
   * rather than trusting the stale cache.
   */
  if (g_strcmp0 (signal_name, "NewIcon") == 0 ||
      g_strcmp0 (signal_name, "NewAttentionIcon") == 0 ||
      g_strcmp0 (signal_name, "NewOverlayIcon") == 0 ||
      g_strcmp0 (signal_name, "NewTitle") == 0 ||
      g_strcmp0 (signal_name, "NewStatus") == 0 ||
      g_strcmp0 (signal_name, "NewToolTip") == 0) {
    /*
     * Extra ref released by on_get_all_properties_ready().
     * g_signal_connect_object() only guarantees this handler is not called
     * after self is destroyed; it does not protect the async operation started
     * here.
     */
    g_dbus_connection_call (self->connection, self->service, self->path,
                             "org.freedesktop.DBus.Properties", "GetAll",
                             g_variant_new ("(s)", ITEM_IFACE),
                             G_VARIANT_TYPE ("(a{sv})"),
                             G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                             on_get_all_properties_ready, g_object_ref (self));
  }

  (void) proxy;
  (void) sender_name;
  (void) parameters;
}



/*
 * Taps arrive via the list box's "row-activated" signal (dispatched here by the
 * owner), not a signal on the row.
 */
void
phosh_sni_item_row_activated (PhoshSniItem *self, GtkWidget *row, GtkListBox *list_box)
{
  if (row == self->row) {
    if (self->expanded)
      phosh_sni_item_collapse_menu (self);
    else if (self->menu_path)
      phosh_sni_item_expand (self);
    else
      phosh_sni_item_activate (self);
    return;
  }

  {
    gpointer id_p = g_object_get_data (G_OBJECT (row), "sni-menu-id");

    if (id_p)
      phosh_sni_item_menu_event (self, GPOINTER_TO_INT (id_p));
  }

  /*
   * Tapping inside the expanded menu (an action or a separator) collapses it.
   */
  phosh_sni_item_collapse_menu (self);
  (void) list_box;
}



static void
on_item_proxy_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr (PhoshSniItem) self = PHOSH_SNI_ITEM (user_data); /* releases the ref taken in phosh_sni_item_new() */
  g_autoptr (GError) error = NULL;

  self->proxy = g_dbus_proxy_new_finish (result, &error);
  if (!self->proxy) {
    g_warning ("Failed to connect to StatusNotifierItem %s%s: %s",
               self->service, self->path, error->message);
    return;
  }

  g_signal_connect_object (self->proxy, "g-properties-changed",
                            G_CALLBACK (on_item_properties_changed), self, 0);
  g_signal_connect_object (self->proxy, "g-signal",
                            G_CALLBACK (on_item_legacy_signal), self, 0);

  /*
   * NULL: the proxy's initial cache (from the construction-time GetAll) is
   * accurate here.
   */
  phosh_sni_item_update_icon (self, NULL);
  phosh_sni_item_update_title (self, NULL);
  phosh_sni_item_update_menu_path (self, NULL);

  (void) source;
}


GtkWidget *
phosh_sni_item_get_row (PhoshSniItem *self)
{
  return self->row;
}


static void
phosh_sni_item_finalize (GObject *object)
{
  PhoshSniItem *self = PHOSH_SNI_ITEM (object);

  g_clear_object (&self->proxy);
  g_clear_object (&self->menu_proxy);
  g_clear_object (&self->connection);
  g_clear_pointer (&self->menu_path, g_free);
  g_clear_pointer (&self->menu_entries, g_ptr_array_unref);
  g_free (self->title);
  g_free (self->id);
  g_free (self->service);
  g_free (self->path);

  if (self->row_destroyed_by_list_box) {
    /*
     * gtk_list_box_bind_model() already destroyed this row when the owner
     * spliced flat_model, regardless of this item's reference; releasing it
     * again would unref freed memory. Only the pointer is dropped. See
     * row_destroyed_by_list_box in background-app-item.c.
     */
    self->row = NULL;
  } else {
    g_clear_object (&self->row);
  }

  G_OBJECT_CLASS (phosh_sni_item_parent_class)->finalize (object);
}


static void
phosh_sni_item_class_init (PhoshSniItemClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = phosh_sni_item_finalize;

  signals[MENU_EXPANDED] = g_signal_new ("menu-expanded", G_TYPE_FROM_CLASS (klass),
                                          G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                          G_TYPE_NONE, 0);
  signals[MENU_READY] = g_signal_new ("menu-ready", G_TYPE_FROM_CLASS (klass),
                                       G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                       G_TYPE_NONE, 0);
  signals[MENU_COLLAPSED] = g_signal_new ("menu-collapsed", G_TYPE_FROM_CLASS (klass),
                                           G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                           G_TYPE_NONE, 0);
  /*
   * uint arg: number of previously spliced-in rows (starting right after this
   * item's row) to remove before splicing in the refetched set.
   */
  signals[MENU_STRUCTURE_CHANGED] = g_signal_new ("menu-structure-changed", G_TYPE_FROM_CLASS (klass),
                                                   G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                                   G_TYPE_NONE, 1, G_TYPE_UINT);
  /*
   * Fires whenever the resolved title changes, including the first resolution,
   * since Title/Id are fetched asynchronously after construction. The owner
   * re-runs SNI-vs-background dedup matching on it, which cannot happen at
   * add-time because the title is not yet known.
   */
  signals[TITLE_CHANGED] = g_signal_new ("title-changed", G_TYPE_FROM_CLASS (klass),
                                          G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                          G_TYPE_NONE, 0);
}


static void
phosh_sni_item_init (PhoshSniItem *self)
{
  /*
   * g_object_ref_sink(): self->row is a persistent widget reused across the
   * item's lifetime (expand/collapse, hide/show via resync_visibility), not
   * created fresh by the bind_model create-function. Without sinking the
   * floating reference, the list box is the only owner once the row is
   * inserted, and resync_visibility() removing the row during a hide cycle
   * destroys it, leaving self->row dangling (use-after-free on re-insertion).
   * Released in finalize().
   */
  self->row = g_object_ref_sink (hdy_action_row_new ());
  gtk_widget_set_visible (self->row, TRUE);
  g_object_set (G_OBJECT (self->row), "activatable", TRUE, NULL);

  self->icon = gtk_image_new ();
  gtk_widget_show (self->icon);
  hdy_action_row_add_prefix (HDY_ACTION_ROW (self->row), self->icon);

  /*
   * self->chevron is added lazily in phosh_sni_item_update_menu_path() once a
   * Menu property is discovered.
   */
}


PhoshSniItem *
phosh_sni_item_new (GDBusConnection *connection, const char *service, const char *path)
{
  PhoshSniItem *self = g_object_new (PHOSH_TYPE_SNI_ITEM, NULL);

  self->connection = g_object_ref (connection);
  self->service = g_strdup (service);
  self->path = g_strdup (path);

  g_object_set_data (G_OBJECT (self->row), "sni-owner", self);

  /*
   * Extra ref released by on_item_proxy_ready(); keeps self alive until the
   * async call completes, since the item can be hidden/destroyed (e.g. by a
   * settings toggle) before the initial proxy construction finishes.
   */
  g_dbus_proxy_new (connection, G_DBUS_PROXY_FLAGS_NONE, NULL, service, path, ITEM_IFACE,
                     NULL, on_item_proxy_ready, g_object_ref (self));

  return self;
}
