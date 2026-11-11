#ifndef SCREENSHOTSERVICE_LIVEPEN_H
#define SCREENSHOTSERVICE_LIVEPEN_H
#include "screenshotservice.h"
BOOL LivePen_Start(const RECT *region);
void LivePen_Toggle(void);
void LivePen_ClearStrokes(void);
BOOL LivePen_IsRunning(void);
void LivePen_Stop(void);
BOOL LivePen_IsEnabled(void);
BOOL LivePen_ContainsScreenPoint(POINT screenPoint);
void LivePen_MouseDownScreen(POINT screenPoint);
void LivePen_MouseMoveScreen(POINT screenPoint);
void LivePen_MouseUpScreen(POINT screenPoint);
void LivePen_MouseLeaveRegion(void);
#endif
