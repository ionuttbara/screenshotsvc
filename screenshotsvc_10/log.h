/*
 * log.h - an opt-in diagnostic log.
 *
 * Off unless DebugLog=1 is set in the ScreenshotService registry key, in which case lines are appended to
 * ScreenshotService.log beside it. This exists because everything interesting
 * here happens with no window on screen: a hotkey that did not arrive and a
 * capture that failed look identical from the outside.
 */
#ifndef SCREENSHOTSERVICE_LOG_H
#define SCREENSHOTSERVICE_LOG_H

#include "screenshotservice.h"

void Log_Enable(BOOL on);
void Log_Printf(const wchar_t *fmt, ...);
/* Writes even when DebugLog is disabled; for crash/watchdog diagnostics only. */
void Log_Always(const wchar_t *fmt, ...);

#endif /* SCREENSHOTSERVICE_LOG_H */
