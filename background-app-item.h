/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/**
 * PhoshBackgroundAppItem:
 *
 * Row for one app reported by org.freedesktop.background.Monitor, built from
 * the app's .desktop file via GDesktopAppInfo. The portal is read-only
 * (app_id/instance/message), so the menu is a fixed Show / Quit / separator
 * set.
 * - Show: g_app_info_launch() against the .desktop file.
 * - Quit: best-effort D-Bus ActivateAction("quit") on
 *   org.freedesktop.Application, followed by a delayed `flatpak kill
 *   <instance>` fallback, since not every app exposes the action.
 */

#define PHOSH_TYPE_BACKGROUND_APP_ITEM phosh_background_app_item_get_type ()
G_DECLARE_FINAL_TYPE (PhoshBackgroundAppItem, phosh_background_app_item,
                       PHOSH, BACKGROUND_APP_ITEM, GObject)

PhoshBackgroundAppItem *phosh_background_app_item_new (GDBusConnection *connection,
                                                         const char      *instance,
                                                         const char      *app_id);

GtkWidget  *phosh_background_app_item_get_row   (PhoshBackgroundAppItem *self);
const char *phosh_background_app_item_get_app_id (PhoshBackgroundAppItem *self);
const char *phosh_background_app_item_get_instance (PhoshBackgroundAppItem *self);

/*
 * Resolved title (display name if a .desktop file was found, else the raw
 * app_id), used for SNI-vs-background dedup matching.
 */
const char *phosh_background_app_item_get_display_name (PhoshBackgroundAppItem *self);

/*
 * Optional per-app status message (e.g. "Syncing files..."), shown as the row's
 * subtitle. NULL or "" clears it.
 */
void        phosh_background_app_item_set_message (PhoshBackgroundAppItem *self,
                                                     const char             *message);

gboolean    phosh_background_app_item_get_expanded (PhoshBackgroundAppItem *self);
void        phosh_background_app_item_collapse_menu (PhoshBackgroundAppItem *self);
GPtrArray  *phosh_background_app_item_get_menu_entries (PhoshBackgroundAppItem *self);

/*
 * Must be called right after splicing this item's row out of the owner's
 * flat_model, only if it was found there. See the row_destroyed_by_list_box
 * comment in the .c file.
 */
void        phosh_background_app_item_note_row_removed_from_list_box (PhoshBackgroundAppItem *self);
GtkWidget  *phosh_background_app_menu_entry_build_row (GObject *entry);

/*
 * TRUE if `obj` is one of this file's private Show/Quit token objects (not a
 * PhoshBackgroundAppItem or an SNI menu-entry item), so the owner can dispatch
 * without this type being exposed.
 */
gboolean    phosh_background_app_menu_entry_is (GObject *obj);

/*
 * Counterpart of phosh_sni_item_row_activated(): called by the owner on the
 * shared GtkListBox's "row-activated".
 */
void        phosh_background_app_item_row_activated (PhoshBackgroundAppItem *self,
                                                       GtkWidget              *row,
                                                       GtkListBox             *list_box);

/*
 * Fires "menu-expanded" (no args) on expand, so the owner can collapse the
 * other tray items (SNI and background).
 */

G_END_DECLS
