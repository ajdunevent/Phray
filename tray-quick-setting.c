/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include "tray-quick-setting.h"
#include "sni-watcher.h"
#include "sni-item.h"
#include "background-app-watcher.h"
#include "background-app-item.h"
#include "background-portal.h"

#include <glib/gi18n-lib.h>
#include <string.h>
#include <handy.h>


/**
 * PhoshTrayQuickSetting:
 *
 * Hosts KStatusNotifierItem tray icons and apps reported by
 * org.freedesktop.background.Monitor as one quick-settings tile. The tile (icon
 * + "Tray" label from qs.ui's PhoshStatusIcon) is display-only; all interaction
 * happens in the status page.
 *
 * A single flat GListStore holds every SNI item (plus, spliced in on expand,
 * its menu entries), followed by every background-portal app (plus, on expand,
 * its Show/Quit entries) that is not a duplicate of a known SNI item. SNI wins
 * because it offers menu functionality. gtk_list_box_bind_model() drives the
 * GtkListBox from this model.
 *
 * "Hiding" an item (enable-sni/enable-background-apps, per-app disabled-apps,
 * or SNI-vs-background dedup) destroys the live
 * PhoshSniItem/PhoshBackgroundAppItem and caches just enough info
 * (hidden_sni_items/hidden_bg_items) to reconstruct an equivalent one later.
 * Row widgets are not reused across a remove-then-reinsert cycle:
 * gtk_list_box_bind_model()'s removal path can destroy a row regardless of
 * external references, and reusing that widget reads freed memory (a garbage
 * allocation size or a Phosh crash). resync_visibility() runs two phases per
 * item kind: revive (hidden entries that should no longer be hidden are
 * reconstructed) then sync (tracked items are inserted into flat_model if newly
 * visible, or destroyed and cached as hidden if they should no longer show).
 *
 * Dedup is a heuristic (case-insensitive substring match between an SNI item's
 * title and a background app's app_id or display name, in either direction),
 * since SNI items carry no .desktop app-id. It re-runs whenever anything
 * relevant changes (an SNI title resolves or changes, an item of either kind is
 * added or removed), since SNI titles resolve asynchronously and may be unknown
 * at add-time.
 */

struct _PhoshTrayQuickSetting {
  PhoshQuickSetting parent;

  PhoshSniWatcher           *watcher;
  PhoshBackgroundAppWatcher *bg_watcher;
  PhoshBackgroundPortal     *background_portal; /* Phray's org.freedesktop.impl.portal.Background implementation (singleton) */
  GtkWidget                 *list_box;
  GtkWidget                 *stack; /* switches between list_box and the empty-state placeholder; see init() */
  GListStore                *flat_model; /* G_TYPE_OBJECT items: PhoshSniItem, PhoshBackgroundAppItem or either kind's menu-entry tokens; see create_row_widget() */
  PhoshStatusPage            *status_page;
  PhoshStatusIcon            *status_icon; /* bound from qs.ui's tray_status_icon; phosh_quick_setting_get_status_icon() is declared in the header but missing from the installed .so */

  GHashTable *items;    /* "service\x1fpath" -> PhoshSniItem*, owns a ref */
  GHashTable *bg_items; /* instance -> PhoshBackgroundAppItem*, owns a ref */

  /*
   * Caches just enough info to reconstruct a hidden item; see "Hiding" in the
   * type's doc comment.
   */
  GHashTable *hidden_sni_items; /* "service\x1fpath" -> HiddenSniEntry*, owned */
  GHashTable *hidden_bg_items;  /* instance -> HiddenBgEntry*, owned */

  /*
   * instance -> GUINT_TO_POINTER(source_id) for background apps that have
   * appeared but are not yet shown; see on_bg_item_added(). Distinct from
   * hidden_bg_items, which holds items hidden after being shown.
   */
  GHashTable *pending_bg_items;

  GSettings *settings; /* mobi.phosh.plugins.tray-quick-setting; watched so prefs changes apply without a restart */
};

G_DEFINE_TYPE (PhoshTrayQuickSetting, phosh_tray_quick_setting, PHOSH_TYPE_QUICK_SETTING)


typedef struct {
  char *service;
  char *path;
  char *cached_id; /* SNI Id at hiding time; NULL if hidden before it resolved (rare). Lets revival re-check per-app disabled-apps without a live item */
} HiddenSniEntry;

static void
hidden_sni_entry_free (HiddenSniEntry *entry)
{
  g_free (entry->service);
  g_free (entry->path);
  g_free (entry->cached_id);
  g_free (entry);
}


typedef struct {
  char *instance;
  char *app_id;
  char *cached_display_name; /* for has_sni_match_for() during revival, without a live item */
} HiddenBgEntry;

static void
hidden_bg_entry_free (HiddenBgEntry *entry)
{
  g_free (entry->instance);
  g_free (entry->app_id);
  g_free (entry->cached_display_name);
  g_free (entry);
}


/* Slightly longer than xdg-desktop-portal's ~5-second GetAppState polling
 * interval, so a fresh poll can correct a just-launched app's momentary
 * misreport as backgrounded before it is shown. */
#define BG_ITEM_DEBOUNCE_SECONDS 6

typedef struct {
  PhoshTrayQuickSetting *self; /* owned ref */
  char                  *instance;
  char                  *app_id;
} PendingBgItem;

/*
 * GDestroyNotify for g_timeout_add_seconds_full(), which runs it once whether
 * the timeout fires or is cancelled via g_source_remove(). Cancelling early
 * (the app disappeared again before the delay elapsed, the point of the
 * debounce) would otherwise leak the struct and its reference on self.
 */
static void
pending_bg_item_free (gpointer data)
{
  PendingBgItem *pending = data;

  g_clear_object (&pending->self);
  g_free (pending->instance);
  g_free (pending->app_id);
  g_free (pending);
}


static char *
item_key (const char *service, const char *path)
{
  return g_strdup_printf ("%s\x1f%s", service, path);
}


static void
phosh_tray_quick_setting_update_tile (PhoshTrayQuickSetting *self)
{
  /*
   * Counts flat_model items, not the tracked item tables: tracked items can be
   * hidden (settings, dedup) while staying tracked, so the table sizes do not
   * reflect what is shown.
   */
  guint visible_count = g_list_model_get_n_items (G_LIST_MODEL (self->flat_model));

  phosh_status_icon_set_info (self->status_icon, _("System Tray"));
  phosh_quick_setting_set_active (PHOSH_QUICK_SETTING (self), visible_count > 0);

  gtk_stack_set_visible_child_name (GTK_STACK (self->stack), visible_count == 0 ? "empty" : "list");
}


static GtkWidget *
create_row_widget (gpointer item, gpointer user_data)
{
  GtkWidget *row;

  (void) user_data;

  if (PHOSH_IS_SNI_ITEM (item)) {
    row = phosh_sni_item_get_row (PHOSH_SNI_ITEM (item));
    gtk_widget_show (row);
    return row;
  }
  if (PHOSH_IS_BACKGROUND_APP_ITEM (item)) {
    row = phosh_background_app_item_get_row (PHOSH_BACKGROUND_APP_ITEM (item));
    gtk_widget_show (row);
    return row;
  }
  if (phosh_sni_menu_entry_is (G_OBJECT (item)))
    return phosh_sni_menu_entry_build_row (G_OBJECT (item));
  return phosh_background_app_menu_entry_build_row (G_OBJECT (item));
}


/*
 * Items are inserted one at a time instead of with one g_list_store_splice():
 * gtk_list_box_bind_model()'s :last-child recompute does not work for every row
 * when a batch is spliced in at the tail.
 */
static void
insert_entries_one_at_a_time (GListStore *model, guint start_position, GPtrArray *entries)
{
  for (guint i = 0; i < entries->len; i++)
    g_list_store_insert (model, start_position + i, g_ptr_array_index (entries, i));
}


/*
 * Insertion point for a new top-level SNI item: before the first
 * background-portal entry (app or menu token), so SNI items stay ahead of
 * background ones regardless of arrival order. Background items always append
 * at the end (see on_bg_item_added()), which keeps the list ordered.
 */
static guint
find_sni_insert_position (PhoshTrayQuickSetting *self)
{
  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->flat_model));

  for (guint i = 0; i < n; i++) {
    g_autoptr (GObject) item = g_list_model_get_item (G_LIST_MODEL (self->flat_model), i);

    if (PHOSH_IS_BACKGROUND_APP_ITEM (item) || phosh_background_app_menu_entry_is (item))
      return i;
  }
  return n;
}



static gboolean
is_app_disabled (PhoshTrayQuickSetting *self, const char *prefix, const char *id)
{
  g_auto (GStrv) disabled = g_settings_get_strv (self->settings, "disabled-apps");
  g_autofree char *key = NULL;

  if (!id || !*id || !disabled)
    return FALSE;

  key = g_strdup_printf ("%s:%s", prefix, id);
  return g_strv_contains ((const char *const *) disabled, key);
}


static gboolean
sni_item_is_disabled (PhoshTrayQuickSetting *self, PhoshSniItem *item)
{
  if (!g_settings_get_boolean (self->settings, "enable-sni"))
    return TRUE;

  return is_app_disabled (self, "sni", phosh_sni_item_get_id (item));
}


static gboolean
bg_item_is_disabled (PhoshTrayQuickSetting *self, PhoshBackgroundAppItem *item)
{
  if (!g_settings_get_boolean (self->settings, "enable-background-apps"))
    return TRUE;

  return is_app_disabled (self, "bg", phosh_background_app_item_get_app_id (item));
}


static gboolean
titles_match (const char *a, const char *b)
{
  g_autofree char *la = NULL;
  g_autofree char *lb = NULL;

  if (!a || !b || !*a || !*b)
    return FALSE;

  la = g_utf8_strdown (a, -1);
  lb = g_utf8_strdown (b, -1);

  return strstr (la, lb) != NULL || strstr (lb, la) != NULL;
}


static gboolean
has_sni_match_for (PhoshTrayQuickSetting *self, const char *app_id, const char *display_name)
{
  GHashTableIter iter;
  gpointer value;

  g_hash_table_iter_init (&iter, self->items);
  while (g_hash_table_iter_next (&iter, NULL, &value)) {
    PhoshSniItem *sni_item = value;
    const char *sni_title;

    /*
     * A disabled SNI item does not mask its background-portal counterpart: if
     * the user turned off the SNI version, the background one can show.
     */
    if (sni_item_is_disabled (self, sni_item))
      continue;

    sni_title = phosh_sni_item_get_title (sni_item);
    if (titles_match (sni_title, app_id) || titles_match (sni_title, display_name))
      return TRUE;
  }
  return FALSE;
}


static gboolean
bg_item_has_sni_match (PhoshTrayQuickSetting *self, PhoshBackgroundAppItem *bg_item)
{
  return has_sni_match_for (self, phosh_background_app_item_get_app_id (bg_item),
                            phosh_background_app_item_get_display_name (bg_item));
}


static gboolean
bg_item_has_message (PhoshBackgroundAppItem *item)
{
  const char *subtitle = hdy_action_row_get_subtitle (
    HDY_ACTION_ROW (phosh_background_app_item_get_row (item)));

  return subtitle && *subtitle;
}


/*
 * Picks the winning instance when the same app_id registers several
 * background-portal instances (e.g. NewsFlash twice, one with a status
 * message): the one with a message wins, then the lower instance value, for a
 * deterministic choice.
 */
static gboolean
bg_item_is_preferred_over (PhoshBackgroundAppItem *a, PhoshBackgroundAppItem *b)
{
  gboolean a_has_message = bg_item_has_message (a);
  gboolean b_has_message = bg_item_has_message (b);

  if (a_has_message != b_has_message)
    return a_has_message;

  {
    guint64 a_num = g_ascii_strtoull (phosh_background_app_item_get_instance (a), NULL, 10);
    guint64 b_num = g_ascii_strtoull (phosh_background_app_item_get_instance (b), NULL, 10);

    if (a_num != b_num)
      return a_num < b_num;

    /* Tied or unparseable: string compare, for a deterministic result. */
    return g_strcmp0 (phosh_background_app_item_get_instance (a),
                       phosh_background_app_item_get_instance (b)) < 0;
  }
}


static gboolean
bg_item_is_masked_by_sibling (PhoshTrayQuickSetting *self, PhoshBackgroundAppItem *bg_item)
{
  GHashTableIter iter;
  gpointer value;
  const char *app_id = phosh_background_app_item_get_app_id (bg_item);

  g_hash_table_iter_init (&iter, self->bg_items);
  while (g_hash_table_iter_next (&iter, NULL, &value)) {
    PhoshBackgroundAppItem *other = value;

    if (other == bg_item)
      continue;
    if (g_strcmp0 (phosh_background_app_item_get_app_id (other), app_id) != 0)
      continue;

    if (bg_item_is_preferred_over (other, bg_item))
      return TRUE;
  }
  return FALSE;
}


static void resync_visibility (PhoshTrayQuickSetting *self);
static void phosh_tray_quick_setting_load_css (void);
static void construct_and_track_sni_item (PhoshTrayQuickSetting *self, GDBusConnection *connection,
                                           const char *service, const char *path);
static void untrack_sni_item (PhoshTrayQuickSetting *self, const char *key, PhoshSniItem *item);
static void construct_and_track_bg_item (PhoshTrayQuickSetting *self, GDBusConnection *connection,
                                          const char *instance, const char *app_id);
static void untrack_bg_item (PhoshTrayQuickSetting *self, const char *instance,
                              PhoshBackgroundAppItem *item);

static void
on_relevant_settings_changed (GSettings *settings, const char *key, gpointer user_data)
{
  resync_visibility (PHOSH_TRAY_QUICK_SETTING (user_data));
  (void) settings;
  (void) key;
}


static void
rebuild_all_bg_items (PhoshTrayQuickSetting *self)
{
  GHashTableIter iter;
  gpointer key_p;
  g_autoptr (GPtrArray) to_rebuild = g_ptr_array_new_with_free_func (g_free);

  g_hash_table_iter_init (&iter, self->bg_items);
  while (g_hash_table_iter_next (&iter, &key_p, NULL))
    g_ptr_array_add (to_rebuild, g_strdup ((const char *) key_p));

  for (guint i = 0; i < to_rebuild->len; i++) {
    const char *instance = g_ptr_array_index (to_rebuild, i);
    PhoshBackgroundAppItem *item = g_hash_table_lookup (self->bg_items, instance);
    g_autofree char *app_id = NULL;

    if (!item)
      continue;

    app_id = g_strdup (phosh_background_app_item_get_app_id (item));
    untrack_bg_item (self, instance, item);
    construct_and_track_bg_item (self, phosh_background_app_watcher_get_connection (self->bg_watcher),
                                 instance, app_id);
  }

  resync_visibility (self); /* inserts the freshly (re)constructed items into flat_model */
}


static void
on_icon_mode_changed (GSettings *settings, const char *key, gpointer user_data)
{
  /*
   * Only background items read this setting (see build_icon_for_mode() in
   * background-app-item.c); SNI items show the app's own icon. update_display()
   * runs once at construction, so a visible item would not repaint on a change.
   * Every tracked background item goes through the destroy-then-reconstruct
   * cycle of resync_visibility() instead of an in-place icon update.
   */
  rebuild_all_bg_items (PHOSH_TRAY_QUICK_SETTING (user_data));
  (void) settings;
  (void) key;
}


static void
on_gtk_theme_changed (GObject *gtk_settings, GParamSpec *pspec, gpointer user_data)
{
  /*
   * Re-adds the CSS provider (see phosh_tray_quick_setting_load_css()), which a
   * GTK3 theme switch can drop, and re-runs the destroy/reconstruct rebuild
   * used for the icon-mode setting so every background item's icon is
   * re-resolved against the new theme.
   */
  phosh_tray_quick_setting_load_css ();
  rebuild_all_bg_items (PHOSH_TRAY_QUICK_SETTING (user_data));
  (void) gtk_settings;
  (void) pspec;
}


static void
resync_visibility (PhoshTrayQuickSetting *self)
{
  {
    GHashTableIter iter;
    gpointer key_p, value_p;
    g_autoptr (GPtrArray) to_revive = g_ptr_array_new (); /* HiddenSniEntry*, not owned here */

    g_hash_table_iter_init (&iter, self->hidden_sni_items);
    while (g_hash_table_iter_next (&iter, &key_p, &value_p)) {
      HiddenSniEntry *entry = value_p;
      gboolean blocked_globally = !g_settings_get_boolean (self->settings, "enable-sni");
      gboolean blocked_individually = entry->cached_id &&
                                       is_app_disabled (self, "sni", entry->cached_id);

      if (!blocked_globally && !blocked_individually)
        g_ptr_array_add (to_revive, entry);
    }

    for (guint i = 0; i < to_revive->len; i++) {
      HiddenSniEntry *entry = g_ptr_array_index (to_revive, i);
      g_autofree char *key = item_key (entry->service, entry->path);

      construct_and_track_sni_item (self, phosh_sni_watcher_get_connection (self->watcher),
                                    entry->service, entry->path);
      g_hash_table_remove (self->hidden_sni_items, key);
    }
  }

  /*
   * SNI: sync flat_model membership for tracked items, in both directions: a
   * newly tracked item (registered or just revived) that should be visible gets
   * inserted; a tracked item that should no longer be visible is destroyed and
   * cached for revival (see the type's doc comment).
   */
  {
    GHashTableIter iter;
    gpointer key_p, value_p;
    g_autoptr (GPtrArray) to_hide = g_ptr_array_new_with_free_func (g_free);
    g_autoptr (GPtrArray) to_show = g_ptr_array_new (); /* PhoshSniItem*, not owned here */

    g_hash_table_iter_init (&iter, self->items);
    while (g_hash_table_iter_next (&iter, &key_p, &value_p)) {
      PhoshSniItem *item = value_p;
      gboolean should_hide = sni_item_is_disabled (self, item);
      gboolean is_visible = g_list_store_find (self->flat_model, item, NULL);

      if (should_hide && is_visible)
        g_ptr_array_add (to_hide, g_strdup ((const char *) key_p));
      else if (!should_hide && !is_visible)
        g_ptr_array_add (to_show, item);
    }

    for (guint i = 0; i < to_show->len; i++) {
      PhoshSniItem *item = g_ptr_array_index (to_show, i);

      g_list_store_insert (self->flat_model, find_sni_insert_position (self), item);
    }

    for (guint i = 0; i < to_hide->len; i++) {
      const char *key = g_ptr_array_index (to_hide, i);
      PhoshSniItem *item = g_hash_table_lookup (self->items, key);
      HiddenSniEntry *entry;

      if (!item)
        continue;

      entry = g_new0 (HiddenSniEntry, 1);
      entry->service = g_strdup (phosh_sni_item_get_service (item));
      entry->path = g_strdup (phosh_sni_item_get_path (item));
      entry->cached_id = g_strdup (phosh_sni_item_get_id (item));

      untrack_sni_item (self, key, item); /* destroys item, releases the row */
      g_hash_table_insert (self->hidden_sni_items, g_strdup (key), entry);
    }
  }

  /*
   * Background: revive phase. Only the global toggle, per-app disabled-apps and
   * SNI-vs-background dedup (has_sni_match_for(), which works from cached
   * strings) are checked. Sibling-instance masking
   * (bg_item_is_masked_by_sibling()) needs a live item's message state and is
   * not cached, so an item may be revived here and re-hidden by the hide phase
   * that follows in the same call: wasteful, not incorrect.
   */
  {
    GHashTableIter iter;
    gpointer key_p, value_p;
    g_autoptr (GPtrArray) to_revive = g_ptr_array_new ();
    gboolean enabled = g_settings_get_boolean (self->settings, "enable-background-apps");

    g_hash_table_iter_init (&iter, self->hidden_bg_items);
    while (g_hash_table_iter_next (&iter, &key_p, &value_p)) {
      HiddenBgEntry *entry = value_p;

      if (enabled && !is_app_disabled (self, "bg", entry->app_id) &&
          !has_sni_match_for (self, entry->app_id, entry->cached_display_name))
        g_ptr_array_add (to_revive, entry);
    }

    for (guint i = 0; i < to_revive->len; i++) {
      HiddenBgEntry *entry = g_ptr_array_index (to_revive, i);

      construct_and_track_bg_item (self,
                                   phosh_background_app_watcher_get_connection (self->bg_watcher),
                                   entry->instance, entry->app_id);
      g_hash_table_remove (self->hidden_bg_items, entry->instance);
    }
  }

  /*
   * Background: sync flat_model membership for tracked items, in both
   * directions as in the SNI phase above.
   */
  {
    GHashTableIter iter;
    gpointer key_p, value_p;
    g_autoptr (GPtrArray) to_hide = g_ptr_array_new_with_free_func (g_free);
    g_autoptr (GPtrArray) to_show = g_ptr_array_new (); /* PhoshBackgroundAppItem*, not owned here */

    g_hash_table_iter_init (&iter, self->bg_items);
    while (g_hash_table_iter_next (&iter, &key_p, &value_p)) {
      PhoshBackgroundAppItem *item = value_p;
      gboolean should_hide = bg_item_is_disabled (self, item) ||
                              bg_item_has_sni_match (self, item) ||
                              bg_item_is_masked_by_sibling (self, item);
      gboolean is_visible = g_list_store_find (self->flat_model, item, NULL);

      if (should_hide && is_visible)
        g_ptr_array_add (to_hide, g_strdup ((const char *) key_p));
      else if (!should_hide && !is_visible)
        g_ptr_array_add (to_show, item);
    }

    for (guint i = 0; i < to_show->len; i++)
      g_list_store_append (self->flat_model, g_ptr_array_index (to_show, i));

    for (guint i = 0; i < to_hide->len; i++) {
      const char *instance = g_ptr_array_index (to_hide, i);
      PhoshBackgroundAppItem *item = g_hash_table_lookup (self->bg_items, instance);
      HiddenBgEntry *entry;

      if (!item)
        continue;

      entry = g_new0 (HiddenBgEntry, 1);
      entry->instance = g_strdup (instance);
      entry->app_id = g_strdup (phosh_background_app_item_get_app_id (item));
      entry->cached_display_name = g_strdup (phosh_background_app_item_get_display_name (item));

      untrack_bg_item (self, instance, item); /* destroys item, releases the row */
      g_hash_table_insert (self->hidden_bg_items, g_strdup (instance), entry);
    }
  }

  phosh_tray_quick_setting_update_tile (self);
}



static void
on_item_menu_expanded (PhoshSniItem *opened_item, gpointer user_data)
{
  PhoshTrayQuickSetting *self = PHOSH_TRAY_QUICK_SETTING (user_data);
  GHashTableIter iter;
  gpointer value;
  GPtrArray *entries;
  guint position;

  /* Mutual exclusion spans both item kinds. */
  g_hash_table_iter_init (&iter, self->items);
  while (g_hash_table_iter_next (&iter, NULL, &value)) {
    if (value != (gpointer) opened_item)
      phosh_sni_item_collapse_menu (value);
  }
  g_hash_table_iter_init (&iter, self->bg_items);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    phosh_background_app_item_collapse_menu (value);

  entries = phosh_sni_item_get_menu_entries (opened_item);
  if (entries && g_list_store_find (self->flat_model, opened_item, &position))
    insert_entries_one_at_a_time (self->flat_model, position + 1, entries);
}


static void
on_item_menu_ready (PhoshSniItem *item, gpointer user_data)
{
  PhoshTrayQuickSetting *self = PHOSH_TRAY_QUICK_SETTING (user_data);
  GPtrArray *entries;
  guint position;

  if (!phosh_sni_item_get_expanded (item))
    return;

  entries = phosh_sni_item_get_menu_entries (item);
  if (!entries || !g_list_store_find (self->flat_model, item, &position))
    return;

  insert_entries_one_at_a_time (self->flat_model, position + 1, entries);
}


static void
on_item_menu_structure_changed (PhoshSniItem *item, guint old_count, gpointer user_data)
{
  PhoshTrayQuickSetting *self = PHOSH_TRAY_QUICK_SETTING (user_data);
  GPtrArray *entries;
  guint position;

  if (!phosh_sni_item_get_expanded (item))
    return;

  entries = phosh_sni_item_get_menu_entries (item);
  if (!g_list_store_find (self->flat_model, item, &position))
    return;

  g_list_store_splice (self->flat_model, position + 1, old_count, NULL, 0);
  if (entries)
    insert_entries_one_at_a_time (self->flat_model, position + 1, entries);
}


static void
on_item_menu_collapsed (PhoshSniItem *item, gpointer user_data)
{
  PhoshTrayQuickSetting *self = PHOSH_TRAY_QUICK_SETTING (user_data);
  GPtrArray *entries = phosh_sni_item_get_menu_entries (item);
  guint position;

  if (!entries || entries->len == 0 || !g_list_store_find (self->flat_model, item, &position))
    return;

  g_list_store_splice (self->flat_model, position + 1, entries->len, NULL, 0);
}


static void
on_item_title_changed (PhoshSniItem *item, gpointer user_data)
{
  resync_visibility (PHOSH_TRAY_QUICK_SETTING (user_data));
  (void) item;
}


/*
 * Shared by on_item_added() (watcher registration) and resync_visibility()'s
 * revive phase (re-showing a hidden item); both need the same construction and
 * signal wiring.
 */
static void
construct_and_track_sni_item (PhoshTrayQuickSetting *self, GDBusConnection *connection,
                               const char *service, const char *path)
{
  g_autofree char *key = item_key (service, path);
  PhoshSniItem *item;

  if (g_hash_table_contains (self->items, key))
    return;

  item = phosh_sni_item_new (connection, service, path);

  g_signal_connect_object (item, "menu-expanded", G_CALLBACK (on_item_menu_expanded), self, 0);
  g_signal_connect_object (item, "menu-ready", G_CALLBACK (on_item_menu_ready), self, 0);
  g_signal_connect_object (item, "menu-collapsed", G_CALLBACK (on_item_menu_collapsed), self, 0);
  g_signal_connect_object (item, "menu-structure-changed",
                            G_CALLBACK (on_item_menu_structure_changed), self, 0);
  g_signal_connect_object (item, "title-changed", G_CALLBACK (on_item_title_changed), self, 0);

  g_hash_table_insert (self->items, g_strdup (key), item);
}


/*
 * Shared by on_item_removed() (the app quit or unregistered) and
 * resync_visibility()'s hide phase (settings or dedup says it should no longer
 * show): both need to get the item out of the visible model and drop the
 * reference. hidden_sni_items is not touched here; callers handle it, since
 * removal should clear any stale hidden entry and hiding should create one.
 */
static void
untrack_sni_item (PhoshTrayQuickSetting *self, const char *key, PhoshSniItem *item)
{
  GPtrArray *entries = phosh_sni_item_get_menu_entries (item);
  guint position;

  if (g_list_store_find (self->flat_model, item, &position)) {
    guint remove_count = 1;

    if (phosh_sni_item_get_expanded (item) && entries)
      remove_count += entries->len;

    g_list_store_splice (self->flat_model, position, remove_count, NULL, 0);
    /*
     * As in untrack_bg_item(): tells the item its row is already gone before
     * finalize() runs.
     */
    phosh_sni_item_note_row_removed_from_list_box (item);
  }

  g_hash_table_remove (self->items, key); /* drops the ref, finalizing the item */
}


static void
on_item_added (PhoshSniWatcher *watcher, const char *service, const char *path, gpointer user_data)
{
  PhoshTrayQuickSetting *self = PHOSH_TRAY_QUICK_SETTING (user_data);
  g_autofree char *key = item_key (service, path);

  /*
   * A registration takes precedence over a stale hidden entry for the same key
   * (e.g. the app quit and relaunched while hidden).
   */
  g_hash_table_remove (self->hidden_sni_items, key);

  construct_and_track_sni_item (self, phosh_sni_watcher_get_connection (watcher), service, path);

  phosh_tray_quick_setting_update_tile (self);
  resync_visibility (self); /* decides whether this shows, per disabled-apps + dedup */
}


static void
on_item_removed (PhoshSniWatcher *watcher, const char *service, const char *path, gpointer user_data)
{
  PhoshTrayQuickSetting *self = PHOSH_TRAY_QUICK_SETTING (user_data);
  g_autofree char *key = item_key (service, path);
  PhoshSniItem *item = g_hash_table_lookup (self->items, key);

  /*
   * The app is gone: drop any stale hidden entry too (e.g. hidden by a setting,
   * then the app quit while hidden), so nothing tries to revive it.
   */
  g_hash_table_remove (self->hidden_sni_items, key);

  if (item)
    untrack_sni_item (self, key, item);

  phosh_tray_quick_setting_update_tile (self);
  resync_visibility (self); /* a bg item this was masking might now show */
  (void) watcher;
}



static void
on_bg_item_menu_expanded (PhoshBackgroundAppItem *opened_item, gpointer user_data)
{
  PhoshTrayQuickSetting *self = PHOSH_TRAY_QUICK_SETTING (user_data);
  GHashTableIter iter;
  gpointer value;
  GPtrArray *entries;
  guint position;

  g_hash_table_iter_init (&iter, self->items);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    phosh_sni_item_collapse_menu (value);
  g_hash_table_iter_init (&iter, self->bg_items);
  while (g_hash_table_iter_next (&iter, NULL, &value)) {
    if (value != (gpointer) opened_item)
      phosh_background_app_item_collapse_menu (value);
  }

  entries = phosh_background_app_item_get_menu_entries (opened_item);
  if (entries && g_list_store_find (self->flat_model, opened_item, &position))
    insert_entries_one_at_a_time (self->flat_model, position + 1, entries);
}


static void
on_bg_item_menu_collapsed (PhoshBackgroundAppItem *item, gpointer user_data)
{
  PhoshTrayQuickSetting *self = PHOSH_TRAY_QUICK_SETTING (user_data);
  GPtrArray *entries = phosh_background_app_item_get_menu_entries (item);
  guint position;

  if (!entries || entries->len == 0 || !g_list_store_find (self->flat_model, item, &position))
    return;

  g_list_store_splice (self->flat_model, position + 1, entries->len, NULL, 0);
}


/* Shared by on_bg_item_added() and resync_visibility()'s revive phase. */
static void
construct_and_track_bg_item (PhoshTrayQuickSetting *self, GDBusConnection *connection,
                              const char *instance, const char *app_id)
{
  PhoshBackgroundAppItem *item;

  if (g_hash_table_contains (self->bg_items, instance))
    return;

  item = phosh_background_app_item_new (connection, instance, app_id);

  g_signal_connect_object (item, "menu-expanded", G_CALLBACK (on_bg_item_menu_expanded), self, 0);
  g_signal_connect_object (item, "menu-collapsed", G_CALLBACK (on_bg_item_menu_collapsed), self, 0);

  g_hash_table_insert (self->bg_items, g_strdup (instance), item);
}


/* Shared by on_bg_item_removed() and resync_visibility()'s hide phase. */
static void
untrack_bg_item (PhoshTrayQuickSetting *self, const char *instance, PhoshBackgroundAppItem *item)
{
  GPtrArray *entries = phosh_background_app_item_get_menu_entries (item);
  guint position;

  if (g_list_store_find (self->flat_model, item, &position)) {
    guint remove_count = 1;

    if (phosh_background_app_item_get_expanded (item) && entries)
      remove_count += entries->len;

    g_list_store_splice (self->flat_model, position, remove_count, NULL, 0);
    /*
     * gtk_list_box_bind_model() destroys the row widget as a side effect of
     * this splice, regardless of the item's reference; without telling the
     * item, finalize() would try to release the row a second time (intermittent
     * g_object_unref failures).
     */
    phosh_background_app_item_note_row_removed_from_list_box (item);
  }

  g_hash_table_remove (self->bg_items, instance);
}


static gboolean
on_bg_item_debounce_timeout (gpointer user_data)
{
  PendingBgItem *pending = user_data;
  PhoshTrayQuickSetting *self = pending->self;

  g_hash_table_remove (self->pending_bg_items, pending->instance);

  construct_and_track_bg_item (self, phosh_background_app_watcher_get_connection (self->bg_watcher),
                               pending->instance, pending->app_id);

  phosh_tray_quick_setting_update_tile (self);
  resync_visibility (self); /* decides whether this shows, per dedup */

  return G_SOURCE_REMOVE;
}


static void
on_bg_item_added (PhoshBackgroundAppWatcher *watcher, const char *instance, const char *app_id,
                   gpointer user_data)
{
  PhoshTrayQuickSetting *self = PHOSH_TRAY_QUICK_SETTING (user_data);
  PendingBgItem *pending;
  guint source_id;

  g_hash_table_remove (self->hidden_bg_items, instance);

  /*
   * Already displayed or waiting out its debounce delay: not scheduled again.
   */
  if (g_hash_table_contains (self->bg_items, instance) ||
      g_hash_table_contains (self->pending_bg_items, instance))
    return;

  /*
   * Delay before showing a newly appeared background app: xdg-desktop-portal's
   * GetAppState polling can briefly misreport a just-launched app as
   * backgrounded before its window registers with the toplevel tracker, making
   * apps flash in the list on first launch. If the app disappears from the
   * watcher's list before the delay ends (the normal case once the window
   * registers), it is never shown.
   */
  pending = g_new0 (PendingBgItem, 1);
  pending->self = g_object_ref (self);
  pending->instance = g_strdup (instance);
  pending->app_id = g_strdup (app_id);

  source_id = g_timeout_add_seconds_full (G_PRIORITY_DEFAULT, BG_ITEM_DEBOUNCE_SECONDS,
                                          on_bg_item_debounce_timeout, pending,
                                          pending_bg_item_free);
  g_hash_table_insert (self->pending_bg_items, g_strdup (instance), GUINT_TO_POINTER (source_id));
  (void) watcher;
}


static void
on_bg_item_removed (PhoshBackgroundAppWatcher *watcher, const char *instance, gpointer user_data)
{
  PhoshTrayQuickSetting *self = PHOSH_TRAY_QUICK_SETTING (user_data);
  PhoshBackgroundAppItem *item;
  gpointer pending_source_id;

  g_hash_table_remove (self->hidden_bg_items, instance);

  /*
   * Still waiting out its debounce delay and never shown: cancelling the source
   * frees the PendingBgItem and releases its ref on self (via the
   * destroy-notify passed to g_timeout_add_seconds_full() above); nothing else
   * to undo.
   */
  if (g_hash_table_lookup_extended (self->pending_bg_items, instance, NULL, &pending_source_id)) {
    g_source_remove (GPOINTER_TO_UINT (pending_source_id));
    g_hash_table_remove (self->pending_bg_items, instance);
    return;
  }

  item = g_hash_table_lookup (self->bg_items, instance);
  if (item)
    untrack_bg_item (self, instance, item);

  phosh_tray_quick_setting_update_tile (self);
  resync_visibility (self); /* a masked sibling instance might now be the winner */
  (void) watcher;
}


static void
on_bg_item_changed (PhoshBackgroundAppWatcher *watcher, const char *instance, const char *message,
                     gpointer user_data)
{
  PhoshTrayQuickSetting *self = PHOSH_TRAY_QUICK_SETTING (user_data);
  PhoshBackgroundAppItem *item = g_hash_table_lookup (self->bg_items, instance);

  if (item) {
    phosh_background_app_item_set_message (item, message);
    /* A message appearing/disappearing can flip which sibling instance
     * of the same app_id should currently be the visible "winner". */
    resync_visibility (self);
  }
  (void) watcher;
}



static void
on_row_activated (GtkListBox *list_box, GtkListBoxRow *row, gpointer user_data)
{
  gpointer sni_owner = g_object_get_data (G_OBJECT (row), "sni-owner");
  gpointer bg_owner = g_object_get_data (G_OBJECT (row), "bg-owner");

  if (sni_owner)
    phosh_sni_item_row_activated (PHOSH_SNI_ITEM (sni_owner), GTK_WIDGET (row), list_box);
  else if (bg_owner)
    phosh_background_app_item_row_activated (PHOSH_BACKGROUND_APP_ITEM (bg_owner),
                                              GTK_WIDGET (row), list_box);

  (void) user_data;
}


/*
 * PhoshQuickSetting has no "show-status"/"hide-status" signals (quick-setting.h
 * exposes can-show-status and showing-status as properties). The base widget
 * and PhoshQuickSettingsBox/PhoshQuickSettings display the PhoshStatusPage when
 * the arrow is tapped; this widget needs can-show-status set and a status page
 * attached.
 */


static void
phosh_tray_quick_setting_dispose (GObject *object)
{
  PhoshTrayQuickSetting *self = PHOSH_TRAY_QUICK_SETTING (object);
  GHashTableIter pending_iter;
  gpointer pending_source_id;

  /*
   * Temporary self-reference for the whole function, guarding against reentrant
   * dispose(): cancelling a pending debounce timer drops its reference on self
   * (see on_bg_item_added()), and if that was the last reference, dispose()
   * would run again while still iterating self->pending_bg_items, corrupting
   * the iteration. Releasing the reference only after chaining to the parent
   * class keeps the refcount above zero until all of dispose() is done.
   */
  g_object_ref (self);

  /*
   * Each pending debounce timeout holds a ref on self (see on_bg_item_added()),
   * so an uncancelled one would keep self alive for up to
   * BG_ITEM_DEBOUNCE_SECONDS after dispose() and then fire against fields
   * cleared below. All are cancelled first; each cancellation frees its
   * PendingBgItem and releases that ref via the destroy-notify.
   */
  if (self->pending_bg_items) {
    g_hash_table_iter_init (&pending_iter, self->pending_bg_items);
    while (g_hash_table_iter_next (&pending_iter, NULL, &pending_source_id))
      g_source_remove (GPOINTER_TO_UINT (pending_source_id));
    g_hash_table_remove_all (self->pending_bg_items);
  }

  g_clear_pointer (&self->items, g_hash_table_unref);
  g_clear_pointer (&self->bg_items, g_hash_table_unref);
  g_clear_pointer (&self->hidden_sni_items, g_hash_table_unref);
  g_clear_pointer (&self->hidden_bg_items, g_hash_table_unref);
  g_clear_pointer (&self->pending_bg_items, g_hash_table_unref);
  g_clear_object (&self->flat_model);
  g_clear_object (&self->watcher);
  g_clear_object (&self->bg_watcher);
  g_clear_object (&self->background_portal);
  g_clear_object (&self->settings);

  G_OBJECT_CLASS (phosh_tray_quick_setting_parent_class)->dispose (object);

  g_object_unref (self);
}


/*
 * Standalone, because it is also called from on_gtk_theme_changed():
 * class_init() runs once at class registration, and a GTK3 theme switch can
 * drop this provider while loading the new theme's stylesheet. Re-adding it on
 * every theme change prevents that; GTK tolerates the same rules being added
 * more than once.
 *
 * css_provider is tracked in a static so each call removes the previous
 * provider before adding its replacement. Theme changes recur (many devices
 * switch light/dark at sunset and sunrise), so otherwise redundant identical
 * providers would accumulate on the whole screen's style context, costing
 * memory and adding to every style recalculation.
 */
static GtkCssProvider *css_provider = NULL;

static void
phosh_tray_quick_setting_load_css (void)
{
  static const char *css =
    ".phosh-tray-menu-row {"
    "  background-color: alpha(@theme_fg_color, 0.05);"
    "}"
    /*
     * Some background apps' icons render forced-symbolic in this row context
     * regardless of the icon name unless -gtk-icon-style is overridden. Two
     * classes exist instead of one unconditional rule:
     * phosh_background_app_item_update_display() applies the right one from the
     * prefer-symbolic-icons setting, since an always-on "regular" rule
     * overrides the icon-selection preference.
     */
    ".phosh-tray-app-icon-regular {"
    "  -gtk-icon-style: regular;"
    "}"
    ".phosh-tray-app-icon-symbolic {"
    "  -gtk-icon-style: symbolic;"
    "}";

  if (css_provider) {
    gtk_style_context_remove_provider_for_screen (gdk_screen_get_default (),
                                                   GTK_STYLE_PROVIDER (css_provider));
    g_clear_object (&css_provider);
  }

  css_provider = gtk_css_provider_new ();
  gtk_css_provider_load_from_data (css_provider, css, -1, NULL);
  gtk_style_context_add_provider_for_screen (gdk_screen_get_default (),
                                              GTK_STYLE_PROVIDER (css_provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  /*
   * No g_object_unref(): css_provider is a kept reference, separate from the
   * screen's own via add_provider_for_screen(), so a later call can find and
   * remove this provider.
   */
}


static void
phosh_tray_quick_setting_class_init (PhoshTrayQuickSettingClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  /*
   * Subtle darkening on menu-entry rows (SNI DBusMenu entries and the synthetic
   * Show/Quit ones) so they read as inside an app's menu. Loaded here instead
   * of in either item type's class_init so it loads whichever item kinds
   * appear.
   */
  phosh_tray_quick_setting_load_css ();

  object_class->dispose = phosh_tray_quick_setting_dispose;

  gtk_widget_class_set_template_from_resource (widget_class,
                                                "/mobi/phosh/plugins/tray-quick-setting/qs.ui");
  gtk_widget_class_bind_template_child (widget_class, PhoshTrayQuickSetting, status_icon);
}


static void
phosh_tray_quick_setting_init (PhoshTrayQuickSetting *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  /*
   * Constructed before anything (watchers, etc.) that could trigger
   * resync_visibility() -> is_app_disabled() and dereference it.
   */
  self->settings = g_settings_new ("mobi.phosh.plugins.tray-quick-setting");
  /*
   * Plain "changed" with no key filter: the handler re-runs visibility for
   * disabled-apps, enable-sni and enable-background-apps alike.
   */
  g_signal_connect_object (self->settings, "changed",
                           G_CALLBACK (on_relevant_settings_changed), self, 0);
  /*
   * Also sends every tracked background item through the destroy/reconstruct
   * cycle when the icon-mode preference changes: the handler above only adjusts
   * flat_model membership, so visible items would not repaint.
   */
  g_signal_connect_object (self->settings, "changed::prefer-symbolic-icons",
                           G_CALLBACK (on_icon_mode_changed), self, 0);
  /*
   * See phosh_tray_quick_setting_load_css(): a GTK3 theme switch can drop the
   * CSS provider.
   */
  g_signal_connect_object (gtk_settings_get_default (), "notify::gtk-theme-name",
                           G_CALLBACK (on_gtk_theme_changed), self, 0);

  self->items = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  self->bg_items = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  self->hidden_sni_items = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                                   (GDestroyNotify) hidden_sni_entry_free);
  self->hidden_bg_items = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                                  (GDestroyNotify) hidden_bg_entry_free);
  self->pending_bg_items = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  self->flat_model = g_list_store_new (G_TYPE_OBJECT);

  self->list_box = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (self->list_box), GTK_SELECTION_NONE);
  gtk_style_context_add_class (gtk_widget_get_style_context (self->list_box), "content");
  g_signal_connect (self->list_box, "row-activated", G_CALLBACK (on_row_activated), self);
  gtk_list_box_bind_model (GTK_LIST_BOX (self->list_box), G_LIST_MODEL (self->flat_model),
                            create_row_widget, NULL, NULL);

  {
    GtkWidget *empty_label = gtk_label_new (_("No tray icons yet"));

    gtk_style_context_add_class (gtk_widget_get_style_context (empty_label), "dim-label");
    gtk_widget_set_margin_top (empty_label, 24);
    gtk_widget_set_margin_bottom (empty_label, 24);

    /*
     * A GtkStack switched explicitly between the list and its empty-state
     * widget replaces gtk_list_box_set_placeholder(), which keeps the
     * placeholder as a hidden structural child of the list; the stack prevents
     * the last row failing to register as :last-child.
     */
    self->stack = gtk_stack_new ();
    gtk_stack_add_named (GTK_STACK (self->stack), self->list_box, "list");
    gtk_stack_add_named (GTK_STACK (self->stack), empty_label, "empty");
  }

  self->status_page = phosh_status_page_new ();
  phosh_status_page_set_title (self->status_page, _("Background Apps"));
  /* PhoshStatusPage's content is a plain GtkListBox with no
   * GtkScrolledWindow wrapper: it handles scrolling internally. */
  /*
   * Phosh's registered action for opening Mobile Settings at this
   * quick-setting's entry; the name and target must match it. Set via
   * g_object_set() since this status_page is built in code, not a .ui template.
   */
  {
    GtkWidget *footer = gtk_button_new ();
    GtkWidget *footer_label = gtk_label_new (_("System Tray Settings"));
    GVariantBuilder target_names;
    GVariant *target;

    gtk_label_set_ellipsize (GTK_LABEL (footer_label), PANGO_ELLIPSIZE_END);
    gtk_widget_show (footer_label);
    gtk_container_add (GTK_CONTAINER (footer), footer_label);

    gtk_widget_set_hexpand (footer, TRUE);
    gtk_widget_set_focus_on_click (footer, FALSE);
    gtk_actionable_set_action_name (GTK_ACTIONABLE (footer), "panel.launch-mobile-panel");

    g_variant_builder_init (&target_names, G_VARIANT_TYPE ("av"));
    g_variant_builder_add (&target_names, "v", g_variant_new_string (PLUGIN_NAME));
    target = g_variant_new ("(s@av)", "topbar", g_variant_builder_end (&target_names));
    gtk_actionable_set_action_target_value (GTK_ACTIONABLE (footer), target);

    gtk_widget_show (footer);
    g_object_set (self->status_page, "footer", footer, NULL);
  }

  phosh_status_page_set_content (self->status_page, self->stack);
  gtk_widget_show_all (GTK_WIDGET (self->status_page));

  phosh_quick_setting_set_status_page (PHOSH_QUICK_SETTING (self), self->status_page);
  phosh_quick_setting_set_can_show_status (PHOSH_QUICK_SETTING (self), TRUE);

  /*
   * No "clicked" connection: this tile has no on/off state to toggle. Only the
   * arrow, which the base widget wires to showing-status, does anything.
   */

  self->watcher = phosh_sni_watcher_get_default ();
  /*
   * g_signal_connect_object() ties disconnection to self's lifetime.
   * self->watcher is a plain GObject that self owns, not a widget in self's
   * hierarchy (unlike self->list_box above, where g_signal_connect() is fine,
   * since a child cannot outlive its parent); g_signal_connect() would assume
   * self->watcher never outlives self.
   */
  g_signal_connect_object (self->watcher, "item-added", G_CALLBACK (on_item_added), self, 0);
  g_signal_connect_object (self->watcher, "item-removed", G_CALLBACK (on_item_removed), self, 0);
  /*
   * self->watcher is a singleton (see phosh_sni_watcher_get_default()) that may
   * have tracked items before this widget connected the two signals above,
   * whose "item-added" emissions have already happened. This call catches up on
   * them. It is safe here because everything on_item_added() touches
   * (self->items, self->flat_model, self->settings, etc.) is already
   * constructed.
   */
  phosh_sni_watcher_replay_existing_items (self->watcher);

  self->bg_watcher = phosh_background_app_watcher_get_default ();
  g_signal_connect_object (self->bg_watcher, "app-added", G_CALLBACK (on_bg_item_added), self, 0);
  g_signal_connect_object (self->bg_watcher, "app-removed", G_CALLBACK (on_bg_item_removed), self, 0);
  g_signal_connect_object (self->bg_watcher, "app-changed", G_CALLBACK (on_bg_item_changed), self, 0);
  /* Catches up on already-known apps, as for the SNI replay above. */
  phosh_background_app_watcher_replay_existing_apps (self->bg_watcher);

  /*
   * xdg-desktop-portal-phosh does not implement the Background portal; once
   * this portal answers requests, self->bg_watcher sees data via
   * org.freedesktop.background.Monitor.
   */
  self->background_portal = phosh_background_portal_get_default ();

  phosh_tray_quick_setting_update_tile (self);
}
