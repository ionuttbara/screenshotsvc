/* Controls shown while recording. */
#ifndef SCREENSHOTSERVICE_RECORDBAR_H
#define SCREENSHOTSERVICE_RECORDBAR_H
#include "screenshotservice.h"
BOOL RecordBar_Show(HWND owner, const RECT *region, UINT stopMsg, BOOL microphoneAvailable);
void RecordBar_SetPaused(BOOL paused);
void RecordBar_SetMicMuted(BOOL muted);
void RecordBar_SetSaving(int percent);
void RecordBar_SetPenEnabled(BOOL enabled);
void RecordBar_Hide(void);
BOOL RecordBar_ContainsPoint(POINT screenPoint);
#endif /* SCREENSHOTSERVICE_RECORDBAR_H */
