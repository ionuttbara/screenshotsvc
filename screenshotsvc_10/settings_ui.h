/*
 * settings_ui.h - the settings dialog.
 */
#ifndef SCREENSHOTSERVICE_SETTINGS_UI_H
#define SCREENSHOTSERVICE_SETTINGS_UI_H

#include "screenshotservice.h"

/* Modal. Returns TRUE if the user accepted, in which case g_cfg has been
   updated and written. Only one dialog can be open at a time. */
BOOL SettingsUI_Show(HWND owner);

#endif /* SCREENSHOTSERVICE_SETTINGS_UI_H */
