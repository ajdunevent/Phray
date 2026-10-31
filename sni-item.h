/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/**
 * PhoshSniItem:
 *
 * Wraps one registered org.kde.StatusNotifierItem as a HdyActionRow (icon
 * prefix, title, and a chevron suffix once a menu is discovered).
 *
 * The menu, if any, is fetched eagerly, cached, and spliced into the owner's
 * model once via "menu-ready". DBusMenu's ItemsPropertiesUpdated signal updates
 * rows in place (e.g. Play/Pause toggling, enable/disable) without model
 * mutation. LayoutUpdated signals structural changes and fires
 * "menu-structure-changed" (uint arg: number of old rows to remove) for the
 * owner to splice-replace. Expanding/collapsing toggles `expanded` and asks the
 * owner to re-run its GtkListBox filter function to show/hide the entry rows.
 */

#define PHOSH_TYPE_SNI_ITEM phosh_sni_item_get_type ()
G_DECLARE_FINAL_TYPE (PhoshSniItem, phosh_sni_item, PHOSH, SNI_ITEM, GObject)

PhoshSniItem *phosh_sni_item_new       (GDBusConnection *connection,
                                         const char      *service,
                                         const char      *path);

GtkWidget    *phosh_sni_item_get_row   (PhoshSniItem *self);

/*
 * Currently shown row title (Title property, falling back to Id), used for
 * SNI-vs-background dedup matching.
 */
const char   *phosh_sni_item_get_title (PhoshSniItem *self);

/*
 * The raw Id property, a stable per-app identifier for per-app enable/disable
 * filtering (Title can show dynamic status text).
 */
const char   *phosh_sni_item_get_id (PhoshSniItem *self);

/*
 * service and path let the owner's destroy-and-rebuild hide/show mechanism
 * reconstruct an equivalent item via phosh_sni_item_new().
 */
const char   *phosh_sni_item_get_service (PhoshSniItem *self);
const char   *phosh_sni_item_get_path (PhoshSniItem *self);

/*
 * Current expand state, used by the owner's GtkListBox filter function to
 * decide whether this item's menu entry rows are visible.
 */
gboolean      phosh_sni_item_get_expanded (PhoshSniItem *self);

/*
 * Collapses this item's menu if open, and fires "menu-collapsed" if it was.
 * Called by the owner on another item's "menu-expanded" so only one menu is
 * open across the tray.
 */
void          phosh_sni_item_collapse_menu (PhoshSniItem *self);

/*
 * NULL until the menu has been fetched at least once (or the item has no menu).
 * Entries are cached GObjects held by the item; the owner splices them into its
 * model once, on "menu-ready", and never removes them; expand/collapse is a
 * filter-function visibility toggle.
 */
GPtrArray    *phosh_sni_item_get_menu_entries (PhoshSniItem *self);

/*
 * Must be called right after splicing this item's row out of the owner's
 * flat_model, only if it was found there. See the row_destroyed_by_list_box
 * comment in the .c file.
 */
void          phosh_sni_item_note_row_removed_from_list_box (PhoshSniItem *self);

/*
 * Builds a row widget for one menu entry token (from
 * phosh_sni_item_get_menu_entries()). Called by the owner's
 * gtk_list_box_bind_model() create-widget callback.
 */
GtkWidget    *phosh_sni_menu_entry_build_row (GObject *entry);

/*
 * TRUE if `obj` is one of this file's private menu-entry token objects (not a
 * PhoshSniItem or a background-app item), so the owner can dispatch without
 * this type being exposed.
 */
gboolean      phosh_sni_menu_entry_is (GObject *obj);

/*
 * Called by the owner on the shared GtkListBox's "row-activated" (taps do not
 * signal on the row itself). No-op if `row` is not this item's.
 */
void          phosh_sni_item_row_activated (PhoshSniItem *self,
                                             GtkWidget    *row,
                                             GtkListBox   *list_box);

G_END_DECLS
