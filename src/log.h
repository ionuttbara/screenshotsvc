/*
 * log.h - an opt-in diagnostic log.
 *
 * Off unless DebugLog=1 is in settings.ini, in which case lines are appended to
 * Nitshot.log beside it. This exists because everything interesting
 * here happens with no window on screen: a hotkey that did not arrive and a
 * capture that failed look identical from the outside.
 */
#ifndef NITSHOT_LOG_H
#define NITSHOT_LOG_H

#include "nitshot.h"

void Log_Enable(BOOL on);
void Log_Printf(const wchar_t *fmt, ...);
/* The same, whether the log is switched on or not: for what a crash or a
   hang leaves behind, which is wanted precisely when nobody asked for it. */
void Log_Always(const wchar_t *fmt, ...);

#endif /* NITSHOT_LOG_H */
