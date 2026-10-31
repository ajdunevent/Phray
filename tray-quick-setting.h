/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>
#include "quick-setting.h"

G_BEGIN_DECLS

#define PHOSH_TYPE_TRAY_QUICK_SETTING phosh_tray_quick_setting_get_type ()

G_DECLARE_FINAL_TYPE (PhoshTrayQuickSetting,
                      phosh_tray_quick_setting,
                      PHOSH, TRAY_QUICK_SETTING, PhoshQuickSetting)

G_END_DECLS
