/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/**
 * PhoshSniWatcher:
 *
 * Implements the org.kde.StatusNotifierWatcher and Host roles needed to host
 * KStatusNotifierItem tray icons.
 *
 * On construction it tries to own org.kde.StatusNotifierWatcher on the session
 * bus. If nothing else owns it, this instance is the Watcher: items register
 * via RegisterStatusNotifierItem and it self-registers as its own Host.
 *
 * If the name is already owned (a KDE session component, another tray host),
 * this instance runs in Host mode: it calls RegisterStatusNotifierHost on the
 * existing Watcher and mirrors its RegisteredStatusNotifierItems list.
 *
 * Consumers connect to "item-added" and "item-removed" in either mode.
 *
 * Process-wide singleton (see phosh_sni_watcher_get_default()). A fresh
 * instance per tile rearrangement or toggle can lose the name-ownership race to
 * its dying predecessor, and SNI apps register with the Watcher once and cannot
 * be asked to re-register with a replacement. A singleton that becomes the
 * Watcher at session start keeps that status and its registry across tile
 * reconstructions.
 */

#define PHOSH_TYPE_SNI_WATCHER phosh_sni_watcher_get_type ()
G_DECLARE_FINAL_TYPE (PhoshSniWatcher, phosh_sni_watcher, PHOSH, SNI_WATCHER, GObject)

/**
 * phosh_sni_watcher_get_default:
 *
 * Returns the process-wide singleton, constructing it on first call. A separate
 * permanent reference taken at plugin module load (see g_io_module_load() in
 * phosh-plugin-tray-quick-setting.c) keeps it alive.
 *
 * A caller connecting to an already-running singleton does not receive
 * "item-added" or "item-removed" for items known before it connected. Call
 * phosh_sni_watcher_replay_existing_items() after connecting.
 *
 * Returns: (transfer full): the singleton instance.
 */
PhoshSniWatcher *phosh_sni_watcher_get_default (void);

/**
 * phosh_sni_watcher_replay_existing_items:
 *
 * Synchronously re-emits "item-added" for every registered item, as if each had
 * just registered. Call once after connecting to a singleton that may already
 * have been tracking items; see phosh_sni_watcher_get_default().
 */
void phosh_sni_watcher_replay_existing_items (PhoshSniWatcher *self);

/*
 * NULL until the session-bus connection is acquired; both modes need it before
 * "item-added" can fire.
 */
GDBusConnection *phosh_sni_watcher_get_connection (PhoshSniWatcher *self);

G_END_DECLS
