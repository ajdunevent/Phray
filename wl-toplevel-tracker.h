/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib-object.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

/**
 * PhoshWlToplevelTracker:
 *
 * Client for the wlr-foreign-toplevel-management-unstable-v1 protocol (bindings
 * generated at build time from protocol/, see meson.build). Phosh's
 * PhoshToplevel and PhoshToplevelManager are not in the installed public SDK.
 * Per toplevel, only the app_id and focus state needed by the Background
 * portal's GetAppState() are tracked.
 *
 * The tracker shares GDK's Wayland display connection
 * (gdk_wayland_display_get_wl_display()) instead of opening a second one. GDK
 * already dispatches all events on that connection, so no separate event queue
 * is needed.
 */

#define PHOSH_TYPE_WL_TOPLEVEL_TRACKER (phosh_wl_toplevel_tracker_get_type ())
G_DECLARE_FINAL_TYPE (PhoshWlToplevelTracker, phosh_wl_toplevel_tracker,
                       PHOSH, WL_TOPLEVEL_TRACKER, GObject)

PhoshWlToplevelTracker *phosh_wl_toplevel_tracker_new (void);

/*
 * Returns TRUE if a toplevel with this app_id exists (the app has a window) and
 * sets *out_activated to whether any of them is focused. Returns FALSE, leaving
 * out_activated untouched, if the app has no window.
 */
gboolean phosh_wl_toplevel_tracker_get_app_window_state (PhoshWlToplevelTracker *self,
                                                           const char             *app_id,
                                                           gboolean               *out_activated);

/*
 * Returns a newly allocated GPtrArray of unique app_ids (an app with several
 * windows appears once; g_free element free func, free with
 * g_ptr_array_unref()). GetAppState uses it to report every windowed app, not
 * only those that called NotifyBackground; see build_app_state() in
 * background-portal.c.
 */
GPtrArray *phosh_wl_toplevel_tracker_get_all_app_ids (PhoshWlToplevelTracker *self);

/*
 * Fires when the tracked toplevel set changes (added, removed or state
 * changed). No arguments: the consumer re-runs its GetAppState comparison.
 */

G_END_DECLS
