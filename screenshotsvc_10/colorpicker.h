#ifndef SCREENSHOTSERVICE_COLORPICKER_H
#define SCREENSHOTSERVICE_COLORPICKER_H
#include "screenshotservice.h"
BOOL ColorPicker_Run(HWND owner, COLORREF *outColor, wchar_t *description, size_t cchDescription);
#endif
