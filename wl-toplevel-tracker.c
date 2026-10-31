/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "wl-toplevel-tracker.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

#include <gdk/gdkwayland.h>

enum {
  CHANGED,
  N_SIGNALS,
};
static guint signals[N_SIGNALS];

typedef struct {
  struct zwlr_foreign_toplevel_handle_v1 *handle;
  char                                   *app_id;
  gboolean                                activated;
  gboolean                                configured; /* seen at least one "done" event */
  PhoshWlToplevelTracker                 *tracker;    /* unowned back-pointer */
} TrackedToplevel;

struct _PhoshWlToplevelTracker {
  GObject parent;

  struct wl_display                       *display; /* GDK's; never destroyed here */
  struct zwlr_foreign_toplevel_manager_v1 *manager;
  GPtrArray                               *toplevels; /* TrackedToplevel*, owned */
};

G_DEFINE_TYPE (PhoshWlToplevelTracker, phosh_wl_toplevel_tracker, G_TYPE_OBJECT)


static void
tracked_toplevel_free (TrackedToplevel *t)
{
  /*
   * The handle's listener holds a raw pointer to this TrackedToplevel.
   * Destroying the proxy here prevents an orphaned handle from dispatching
   * events (e.g. its window closing after the tracker is freed) into freed
   * memory.
   */
  zwlr_foreign_toplevel_handle_v1_destroy (t->handle);
  g_free (t->app_id);
  g_free (t);
}



static void
on_handle_title (void *data, struct zwlr_foreign_toplevel_handle_v1 *handle, const char *title)
{
  /* Not needed: GetAppState only cares about app_id + focus state. */
  (void) data;
  (void) handle;
  (void) title;
}


static void
on_handle_app_id (void *data, struct zwlr_foreign_toplevel_handle_v1 *handle, const char *app_id)
{
  TrackedToplevel *t = data;

  g_free (t->app_id);
  t->app_id = g_strdup (app_id);
  (void) handle;
}


static void
on_handle_output_enter (void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
                         struct wl_output *output)
{
  (void) data;
  (void) handle;
  (void) output;
}


static void
on_handle_output_leave (void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
                         struct wl_output *output)
{
  (void) data;
  (void) handle;
  (void) output;
}


static void
on_handle_state (void *data, struct zwlr_foreign_toplevel_handle_v1 *handle, struct wl_array *state)
{
  TrackedToplevel *t = data;
  uint32_t *value;
  gboolean activated = FALSE;

  wl_array_for_each (value, state) {
    if (*value == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED)
      activated = TRUE;
  }
  t->activated = activated;
  (void) handle;
}


static void
on_handle_done (void *data, struct zwlr_foreign_toplevel_handle_v1 *handle)
{
  TrackedToplevel *t = data;

  t->configured = TRUE;
  g_signal_emit (t->tracker, signals[CHANGED], 0);
  (void) handle;
}


static void
on_handle_closed (void *data, struct zwlr_foreign_toplevel_handle_v1 *handle)
{
  TrackedToplevel *t = data;
  PhoshWlToplevelTracker *self = t->tracker;
  guint index;

  if (g_ptr_array_find (self->toplevels, t, &index))
    g_ptr_array_remove_index (self->toplevels, index); /* frees t via tracked_toplevel_free, which also destroys the handle */

  g_signal_emit (self, signals[CHANGED], 0);
  (void) handle;
}


static void
on_handle_parent (void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
                   struct zwlr_foreign_toplevel_handle_v1 *parent)
{
  /* Not needed: parent/child window relationships aren't tracked. */
  (void) data;
  (void) handle;
  (void) parent;
}


static const struct zwlr_foreign_toplevel_handle_v1_listener handle_listener = {
  on_handle_title,
  on_handle_app_id,
  on_handle_output_enter,
  on_handle_output_leave,
  on_handle_state,
  on_handle_done,
  on_handle_closed,
  on_handle_parent,
};



static void
on_manager_toplevel (void *data, struct zwlr_foreign_toplevel_manager_v1 *manager,
                      struct zwlr_foreign_toplevel_handle_v1 *handle)
{
  PhoshWlToplevelTracker *self = data;
  TrackedToplevel *t = g_new0 (TrackedToplevel, 1);

  t->handle = handle;
  t->tracker = self;
  g_ptr_array_add (self->toplevels, t);

  zwlr_foreign_toplevel_handle_v1_add_listener (handle, &handle_listener, t);
  (void) manager;
}


static void
on_manager_finished (void *data, struct zwlr_foreign_toplevel_manager_v1 *manager)
{
  /*
   * The manager proxy is becoming invalid; existing TrackedToplevel entries are
   * cleaned up through their own "closed" events.
   */
  (void) data;
  (void) manager;
}


static const struct zwlr_foreign_toplevel_manager_v1_listener manager_listener = {
  on_manager_toplevel,
  on_manager_finished,
};



static void
on_registry_global (void *data, struct wl_registry *registry, uint32_t name,
                     const char *interface, uint32_t version)
{
  PhoshWlToplevelTracker *self = data;

  if (g_strcmp0 (interface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
    self->manager = wl_registry_bind (registry, name, &zwlr_foreign_toplevel_manager_v1_interface,
                                       MIN (version, 3));
    zwlr_foreign_toplevel_manager_v1_add_listener (self->manager, &manager_listener, self);
  }
}


static void
on_registry_global_remove (void *data, struct wl_registry *registry, uint32_t name)
{
  /*
   * Registry object names are not tracked; a compositor restart is not handled
   * beyond the handles' "closed" events.
   */
  (void) data;
  (void) registry;
  (void) name;
}


static const struct wl_registry_listener registry_listener = {
  on_registry_global,
  on_registry_global_remove,
};



gboolean
phosh_wl_toplevel_tracker_get_app_window_state (PhoshWlToplevelTracker *self, const char *app_id,
                                                  gboolean *out_activated)
{
  gboolean found = FALSE;
  gboolean activated = FALSE;

  for (guint i = 0; i < self->toplevels->len; i++) {
    TrackedToplevel *t = g_ptr_array_index (self->toplevels, i);

    if (!t->configured || g_strcmp0 (t->app_id, app_id) != 0)
      continue;

    found = TRUE;
    if (t->activated)
      activated = TRUE;
  }

  if (out_activated)
    *out_activated = activated;
  return found;
}


GPtrArray *
phosh_wl_toplevel_tracker_get_all_app_ids (PhoshWlToplevelTracker *self)
{
  GPtrArray *app_ids = g_ptr_array_new_with_free_func (g_free);

  for (guint i = 0; i < self->toplevels->len; i++) {
    TrackedToplevel *t = g_ptr_array_index (self->toplevels, i);
    gboolean already_have = FALSE;

    if (!t->configured || !t->app_id)
      continue;

    for (guint j = 0; j < app_ids->len; j++) {
      if (g_strcmp0 (g_ptr_array_index (app_ids, j), t->app_id) == 0) {
        already_have = TRUE;
        break;
      }
    }
    if (!already_have)
      g_ptr_array_add (app_ids, g_strdup (t->app_id));
  }

  return app_ids;
}



static void
phosh_wl_toplevel_tracker_finalize (GObject *object)
{
  PhoshWlToplevelTracker *self = PHOSH_WL_TOPLEVEL_TRACKER (object);

  /*
   * self->manager is this tracker's own binding (from wl_registry_bind() in
   * on_registry_global()); self->display is shared with GDK and is not
   * destroyed here. A manager left alive keeps a listener holding a raw pointer
   * to self, so any later window event would dispatch into freed memory.
   *
   * The manager interface has no destroy request, only "stop", and its
   * "finished" event (a destructor event) arrives asynchronously, possibly
   * after self is freed. "stop" is sent as a courtesy, then wl_proxy_destroy()
   * destroys the proxy synchronously, which guarantees the listener is never
   * invoked again whether or not the compositor responds.
   */
  if (self->manager) {
    zwlr_foreign_toplevel_manager_v1_stop (self->manager);
    wl_proxy_destroy ((struct wl_proxy *) self->manager);
  }

  /*
   * self->display belongs to GDK's shared connection and is not destroyed here.
   */

  g_clear_pointer (&self->toplevels, g_ptr_array_unref); /* each entry's handle proxy is destroyed via tracked_toplevel_free */

  G_OBJECT_CLASS (phosh_wl_toplevel_tracker_parent_class)->finalize (object);
}


static void
phosh_wl_toplevel_tracker_class_init (PhoshWlToplevelTrackerClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = phosh_wl_toplevel_tracker_finalize;

  signals[CHANGED] = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass),
                                    G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                    G_TYPE_NONE, 0);
}


static void
phosh_wl_toplevel_tracker_init (PhoshWlToplevelTracker *self)
{
  GdkDisplay *gdk_display;
  struct wl_registry *registry;

  self->toplevels = g_ptr_array_new_with_free_func ((GDestroyNotify) tracked_toplevel_free);

  gdk_display = gdk_display_get_default ();
  if (!GDK_IS_WAYLAND_DISPLAY (gdk_display)) {
    g_warning ("Not running on Wayland -- toplevel tracking unavailable");
    return;
  }

  self->display = gdk_wayland_display_get_wl_display (gdk_display);

  registry = wl_display_get_registry (self->display);
  wl_registry_add_listener (registry, &registry_listener, self);

  /* Synchronous roundtrip to receive the initial registry globals. */
  wl_display_roundtrip (self->display);

  /*
   * The registry listener holds a raw pointer to self. The registry is only
   * needed to discover and bind the manager global, which stays valid after the
   * registry is destroyed, so destroying it here removes that listener.
   */
  wl_registry_destroy (registry);

  if (!self->manager)
    g_warning ("Compositor doesn't support wlr-foreign-toplevel-management");
}


PhoshWlToplevelTracker *
phosh_wl_toplevel_tracker_new (void)
{
  return g_object_new (PHOSH_TYPE_WL_TOPLEVEL_TRACKER, NULL);
}
