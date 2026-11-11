/* monitorpicker.h - choose a physical display for a full-monitor recording. */
#ifndef SCREENSHOTSERVICE_MONITORPICKER_H
#define SCREENSHOTSERVICE_MONITORPICKER_H

#include "screenshotservice.h"

/*
 * In Ask-every-time mode, lists every attached display (including a single
 * display) in a themed picker; clicking a row or pressing its number selects
 * it, while Escape cancels. Optional force-last mode reuses the last physical
 * target while present and opens the picker again if it disappears.
 * 'screenRect' is returned in absolute virtual-desktop coordinates.
 */
BOOL MonitorPicker_Select(HWND owner, RECT *screenRect);

#endif /* SCREENSHOTSERVICE_MONITORPICKER_H */
