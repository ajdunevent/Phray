/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/**
 * PhoshBackgroundAppWatcher:
 *
 * Watches org.freedesktop.background.Monitor for apps running in the
 * background.
 *
 * Monitor is a standalone D-Bus service at /org/freedesktop/background/monitor,
 * not exposed under org.freedesktop.portal.Desktop. Connecting at the portal
 * path reaches nothing, regardless of backend, .portal file, or portals.conf
 * setup.
 *
 * Its single property, BackgroundApps ("aa{sv}"), lists entries with app_id,
 * instance (Flatpak instance ID, the per-running-instance key, since app_id
 * does not distinguish multiple instances of an app) and an optional message.
 * Updates arrive as property-changed notifications; the watcher diffs the
 * contents against its known set to emit "app-added", "app-removed" and
 * "app-changed".
 *
 * Process-wide singleton (see phosh_background_app_watcher_get_default()), so
 * tile rearrangement or toggling does not repeat D-Bus connection setup and the
 * full re-query of running apps.
 */

#define PHOSH_TYPE_BACKGROUND_APP_WATCHER phosh_background_app_watcher_get_type ()
G_DECLARE_FINAL_TYPE (PhoshBackgroundAppWatcher, phosh_background_app_watcher,
                       PHOSH, BACKGROUND_APP_WATCHER, GObject)

/**
 * phosh_background_app_watcher_get_default:
 *
 * Returns the process-wide singleton, constructing it on first call. A separate
 * permanent reference taken at plugin module load (see g_io_module_load() in
 * phosh-plugin-tray-quick-setting.c) keeps it alive.
 *
 * A caller connecting to an already-running singleton does not receive
 * "app-added", "app-removed" or "app-changed" for apps known before it
 * connected. Call phosh_background_app_watcher_replay_existing_apps() after
 * connecting.
 *
 * Returns: (transfer full): the singleton instance.
 */
PhoshBackgroundAppWatcher *phosh_background_app_watcher_get_default (void);

/**
 * phosh_background_app_watcher_replay_existing_apps:
 *
 * Synchronously re-emits "app-added" (and "app-changed", for apps with a
 * message) for every known app, as if each had just appeared. Call once after
 * connecting to a singleton that may already have been tracking apps; see
 * phosh_background_app_watcher_get_default().
 */
void phosh_background_app_watcher_replay_existing_apps (PhoshBackgroundAppWatcher *self);

GDBusConnection *phosh_background_app_watcher_get_connection (PhoshBackgroundAppWatcher *self);

G_END_DECLS
