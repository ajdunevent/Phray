/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/*
 * Manages one line of Phosh's dedicated
 * ~/.config/xdg-desktop-portal/phosh-portals.conf:
 *
 *   [preferred]
 *   org.freedesktop.impl.portal.Background=...;phray;...
 *
 * The Phosh-specific file (DESKTOP-portals.conf, matched against
 * XDG_CURRENT_DESKTOP) takes precedence over the generic portals.conf, so edits
 * apply only in a Phosh session and do not leak into other desktop environments
 * used by the same account.
 *
 * The same precedence means creating phosh-portals.conf from scratch would
 * shadow the generic file's existing customizations. On first creation, the
 * generic file's content is copied forward as the starting point. Afterwards
 * phosh-portals.conf is authoritative and later edits to the generic file do
 * not propagate.
 *
 * GKeyFile is used for all reads and writes, since this file can hold settings
 * for other portal interfaces and a parsing mistake would affect them. Only the
 * "phray" token within the Background line is added or removed; other listed
 * backends keep their order.
 *
 * xdg-desktop-portal does not reload the file on change, so edits take effect
 * after a Phosh/session restart. xdg-desktop-portal is not restarted
 * automatically because other applications may be mid-operation with it.
 */

/**
 * phosh_portals_conf_enable_background_backend:
 *
 * Ensures "phray" is present in the Background line. An existing line gets
 * "phray" appended, so existing preferences are tried first. A missing line is
 * created as "phosh;phray;", so Phosh's backend takes over if it gains
 * Background support. No-op if "phray" is already present. Creates the config
 * file and directory if missing.
 *
 * Failures (permissions, malformed file) are logged via g_warning() and treated
 * as non-fatal, so they do not block toggling the setting.
 */
void phosh_portals_conf_enable_background_backend (void);

/**
 * phosh_portals_conf_disable_background_backend:
 *
 * Removes "phray" from the Background line, leaving other backends in order. If
 * nothing is left, the key is removed instead of replaced with a comment, since
 * enabling again would not remove the comment and a stale one would mislead.
 * No-op if the file does not exist or "phray" is absent.
 */
void phosh_portals_conf_disable_background_backend (void);

G_END_DECLS
