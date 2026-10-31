/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <adwaita.h>

#pragma once

G_BEGIN_DECLS

#define PHOSH_TYPE_TRAY_QUICK_SETTING_PREFS (phosh_tray_quick_setting_prefs_get_type ())
G_DECLARE_FINAL_TYPE (PhoshTrayQuickSettingPrefs,
                      phosh_tray_quick_setting_prefs,
                      PHOSH, TRAY_QUICK_SETTING_PREFS,
                      AdwPreferencesDialog)

G_END_DECLS
