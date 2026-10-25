/*
 * crash.h - what to leave behind when the process dies.
 *
 * An unhandled exception writes one line into the log - whether the log is
 * switched on or not - naming the exception, the module and offset it
 * happened in, and the last step the code said it was taking, and saves a
 * small minidump beside it in %APPDATA%\Nitshot. For code that cannot be
 * tested here (other vendors' encoder runtimes), that is the difference
 * between a report and a guess.
 */
#ifndef NITSHOT_CRASH_H
#define NITSHOT_CRASH_H

#include "nitshot.h"

/* Once, early in wWinMain. */
void Crash_Install(void);

/* Names the step about to be taken, for the crash line. 'what' must be a
   string literal or otherwise outlive the step; nothing is copied, so this
   costs one store and can sit in a per-frame path. */
void Crash_Step(const wchar_t *what);

/* Puts the unhandled-exception filter back if something - a vendor runtime
   loaded since - replaced it, and logs that it had to. */
void Crash_Reassert(const wchar_t *after);

/* A watch over a stretch that must keep moving (a recording through the
   card's own encoder): Crash_Beat from its loop; quiet for longer than
   'stallMs' and the log gets a line and a hang-*.dmp of every thread.
   While watched, serious first-chance exceptions are logged too, and so is
   the process leaving through ExitProcess. */
void Crash_WatchBegin(DWORD stallMs);
void Crash_Beat(void);
void Crash_WatchEnd(void);

/* Copies Windows' own record of the newest crash or hang of 'exeName'
   (NULL: this program) within 'maxAgeMs' from the Application event log
   into the log - what Windows saw that no handler inside the process could.
   TRUE if there was one. */
BOOL Crash_LogWindowsRecord(const wchar_t *exeName, DWORD maxAgeMs);

#endif /* NITSHOT_CRASH_H */
