/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/**
 * PhoshBackgroundPortal:
 *
 * Implements org.freedesktop.impl.portal.Background in-process, so
 * xdg-desktop-portal (routed here by a .portal file) exposes the
 * org.freedesktop.background.Monitor property consumed by
 * background-app-watcher.c. xdg-desktop-portal-phosh does not implement the
 * Background portal.
 *
 * Methods and signal:
 * - GetAppState() -> a{sv}: per-app-id state (0=background, 1=running,
 *   2=active), computed with PhoshWlToplevelTracker.
 * - NotifyBackground(handle, app_id, name) -> (response, results): auto-granted
 *   (response=1) with no prompt.
 * - EnableAutostart(...): deprecated; stubbed for interface completeness.
 * - RunningApplicationsChanged: emitted when tracked state changes, prompting
 *   the frontend to call GetAppState again.
 */

#define PHOSH_TYPE_BACKGROUND_PORTAL (phosh_background_portal_get_type ())
G_DECLARE_FINAL_TYPE (PhoshBackgroundPortal, phosh_background_portal, PHOSH, BACKGROUND_PORTAL, GObject)

/**
 * phosh_background_portal_get_default:
 *
 * Returns the process-wide singleton, constructing it on first call. A
 * constructor tied to PhoshTrayQuickSetting's lifecycle would re-run the D-Bus
 * name ownership and Wayland tracker setup on every tile rearrangement and
 * on/off toggle, which the "requires restart" behavior avoids. A separate
 * permanent reference taken at plugin module load (see g_io_module_load() in
 * phosh-plugin-tray-quick-setting.c) keeps the singleton alive across widget
 * instances.
 *
 * Returns: (transfer full): the singleton instance.
 */
PhoshBackgroundPortal *phosh_background_portal_get_default (void);

G_END_DECLS
