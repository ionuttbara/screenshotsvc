/*
 * crash.c - a line in the log and a minidump when the process dies.
 */

#include "nitshot.h"
#include "crash.h"
#include "log.h"

#include <dbghelp.h>
#include <shlobj.h>
#include <strsafe.h>
#include <winevt.h>

typedef BOOL (WINAPI *PFN_MiniDumpWriteDump)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                             PMINIDUMP_EXCEPTION_INFORMATION,
                                             PMINIDUMP_USER_STREAM_INFORMATION,
                                             PMINIDUMP_CALLBACK_INFORMATION);

typedef EVT_HANDLE (WINAPI *PFN_EvtQuery)(EVT_HANDLE, LPCWSTR, LPCWSTR, DWORD);
typedef BOOL (WINAPI *PFN_EvtNext)(EVT_HANDLE, DWORD, EVT_HANDLE *, DWORD, DWORD, PDWORD);
typedef BOOL (WINAPI *PFN_EvtRender)(EVT_HANDLE, EVT_HANDLE, DWORD, DWORD, PVOID, PDWORD, PDWORD);
typedef BOOL (WINAPI *PFN_EvtClose)(EVT_HANDLE);

static const wchar_t *volatile g_step = L"(none)";
static wchar_t                 g_dir[MAX_PATH];   /* worked out before any crash */
static volatile LONG           g_inside;
static volatile LONG           g_firstChance;     /* lines logged so far */

/* The watch over a recording: a heartbeat, the thread that checks it, and
   whether the process is meant to be inside one. */
static volatile LONG64 g_beat;
static volatile LONG   g_watching;
static HANDLE          g_watchThread, g_watchStop;
static DWORD           g_stallMs;

/* Where the dumps go, worked out before anything has gone wrong. */
static void FindDir(void)
{
    PWSTR appdata = NULL;

    if (g_dir[0])
        return;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_RoamingAppData, 0, NULL, &appdata))) {
        if (FAILED(StringCchPrintfW(g_dir, MAX_PATH, L"%s\\%s", appdata, APP_NAME_W)))
            g_dir[0] = 0;
        CoTaskMemFree(appdata);
    }
}

void Crash_Step(const wchar_t *what)
{
    g_step = what ? what : L"(none)";
}

/* The module an address belongs to, as a file name and an offset. */
static void WhereIs(const void *addr, wchar_t *name, size_t cch, ULONG_PTR *offset)
{
    HMODULE mod = NULL;
    wchar_t path[MAX_PATH];

    StringCchCopyW(name, cch, L"?");
    *offset = (ULONG_PTR)addr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)addr, &mod) && mod &&
        GetModuleFileNameW(mod, path, MAX_PATH)) {
        const wchar_t *base = wcsrchr(path, L'\\');
        StringCchCopyW(name, cch, base ? base + 1 : path);
        *offset = (ULONG_PTR)addr - (ULONG_PTR)mod;
    }
}

/* A minidump beside the log: stacks, threads, the modules and their
   versions - enough to place the crash, small enough to mail. 'ep' is NULL
   for a hang, which dumps every thread as it stands. */
static void WriteDump(EXCEPTION_POINTERS *ep, const wchar_t *kind)
{
    wchar_t    dump[MAX_PATH];
    SYSTEMTIME st;

    GetLocalTime(&st);
    if (g_dir[0] &&
        SUCCEEDED(StringCchPrintfW(dump, MAX_PATH, L"%s\\%s-%04u%02u%02u-%02u%02u%02u.dmp",
                                   g_dir, kind, st.wYear, st.wMonth, st.wDay,
                                   st.wHour, st.wMinute, st.wSecond))) {
        HMODULE dbg = LoadLibraryExW(L"dbghelp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        PFN_MiniDumpWriteDump write = dbg ? (PFN_MiniDumpWriteDump)(void *)
                                      GetProcAddress(dbg, "MiniDumpWriteDump") : NULL;
        HANDLE f = write ? CreateFileW(dump, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
        if (f != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei;
            BOOL ok;
            mei.ThreadId = GetCurrentThreadId();
            mei.ExceptionPointers = ep;
            mei.ClientPointers = FALSE;
            ok = write(GetCurrentProcess(), GetCurrentProcessId(), f,
                       (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                                       MiniDumpWithIndirectlyReferencedMemory),
                       ep ? &mei : NULL, NULL, NULL);
            CloseHandle(f);
            Log_Always(L"crash: %s %s", ok ? L"minidump written to" : L"no minidump at", dump);
        }
    }
}

static void LogException(const wchar_t *what, const EXCEPTION_RECORD *er)
{
    wchar_t   module[MAX_PATH];
    ULONG_PTR offset = 0;

    WhereIs(er ? er->ExceptionAddress : NULL, module, ARRAYSIZE(module), &offset);
    Log_Always(L"crash: %s 0x%08lX at %s+0x%Ix on thread %lu, last step: %s", what,
               er ? (unsigned long)er->ExceptionCode : 0UL, module, offset,
               GetCurrentThreadId(), g_step);
    if (er && er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        Log_Always(L"crash: %s of address 0x%Ix",
                   er->ExceptionInformation[0] == 0 ? L"read" :
                   er->ExceptionInformation[0] == 1 ? L"write" : L"execution",
                   (ULONG_PTR)er->ExceptionInformation[1]);
}

static LONG WINAPI OnCrash(EXCEPTION_POINTERS *ep)
{
    /* A crash inside this handler must not loop. */
    if (InterlockedExchange(&g_inside, 1))
        return EXCEPTION_CONTINUE_SEARCH;
    LogException(L"exception", ep ? ep->ExceptionRecord : NULL);
    WriteDump(ep, L"crash");
    return EXCEPTION_CONTINUE_SEARCH;
}

/*
 * Every serious exception, the moment it is raised - before any handler,
 * and before anything that replaced the unhandled-exception filter can
 * swallow it. Some are caught and handled where they happen (drivers probe
 * memory that way), so the line says "first-chance" and only a few are
 * logged; the last one before a silent death is the one that matters.
 */
static LONG WINAPI OnFirstChance(EXCEPTION_POINTERS *ep)
{
    const EXCEPTION_RECORD *er = ep ? ep->ExceptionRecord : NULL;
    DWORD code = er ? er->ExceptionCode : 0;

    if ((code & 0xF0000000) != 0xC0000000 || g_inside || !g_watching)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&g_firstChance) > 8)
        return EXCEPTION_CONTINUE_SEARCH;
    LogException(L"first-chance exception", er);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* The watch thread: a recording that stops beating has hung. */
static DWORD WINAPI WatchThread(void *unused)
{
    BOOL fired = FALSE;
    (void)unused;
    while (WaitForSingleObject(g_watchStop, 1000) == WAIT_TIMEOUT) {
        ULONGLONG quiet = GetTickCount64() - (ULONGLONG)InterlockedCompareExchange64(&g_beat, 0, 0);
        if (!fired && quiet > g_stallMs) {
            fired = TRUE;
                    Log_Always(L"crash: the recording has not moved for %llu s, last step: %s",
                       quiet / 1000, g_step);
            WriteDump(NULL, L"hang");
        } else if (fired && quiet < g_stallMs) {
            fired = FALSE;
            Log_Always(L"crash: the recording moves again, last step: %s", g_step);
        }
    }
    return 0;
}

void Crash_Beat(void)
{
    InterlockedExchange64(&g_beat, (LONG64)GetTickCount64());
}

void Crash_WatchBegin(DWORD stallMs)
{
    Crash_WatchEnd();
    FindDir();
    g_stallMs = stallMs;
    Crash_Beat();
    g_watchStop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (g_watchStop)
        g_watchThread = CreateThread(NULL, 0, WatchThread, NULL, 0, NULL);
    InterlockedExchange(&g_watching, 1);
}

void Crash_WatchEnd(void)
{
    InterlockedExchange(&g_watching, 0);
    if (g_watchThread) {
        SetEvent(g_watchStop);
        WaitForSingleObject(g_watchThread, INFINITE);
        CloseHandle(g_watchThread);
        g_watchThread = NULL;
    }
    if (g_watchStop) {
        CloseHandle(g_watchStop);
        g_watchStop = NULL;
    }
}

void Crash_Reassert(const wchar_t *after)
{
    LPTOP_LEVEL_EXCEPTION_FILTER was = SetUnhandledExceptionFilter(OnCrash);
    if (was != OnCrash)
        Log_Always(L"crash: the crash filter had been replaced %s (by %p); put back",
                   after, (void *)was);
}

/*
 * A process ending through ExitProcess - its own code or a runtime's -
 * passes through here on the way out, an unhandled exception does not
 * (that has its line already) and TerminateProcess skips it. Inside a
 * watched recording nothing should be leaving, so it says so.
 */
static void NTAPI OnTls(PVOID mod, DWORD reason, PVOID reserved)
{
    (void)mod; (void)reserved;
    if (reason == DLL_PROCESS_DETACH && g_watching && !g_inside) {
            Log_Always(L"crash: the process is exiting in the middle of a recording, "
                   L"thread %lu, last step: %s", GetCurrentThreadId(), g_step);
    }
}

#if defined(_MSC_VER)
#ifdef _WIN64
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:g_crashTls")
#else
#pragma comment(linker, "/INCLUDE:__tls_used")
#pragma comment(linker, "/INCLUDE:_g_crashTls")
#endif
#pragma const_seg(".CRT$XLB")
extern const PIMAGE_TLS_CALLBACK g_crashTls;
const PIMAGE_TLS_CALLBACK g_crashTls = OnTls;
#pragma const_seg()
#elif defined(__GNUC__)
const PIMAGE_TLS_CALLBACK g_crashTls __attribute__((section(".CRT$XLB"), used)) = OnTls;
#endif

/* The text between <Data...> and </Data> elements, joined by " | ". */
static void JoinData(const wchar_t *xml, wchar_t *out, size_t cch, wchar_t *first, size_t cchFirst)
{
    const wchar_t *p = xml;
    int n = 0;

    out[0] = 0;
    first[0] = 0;
    while ((p = wcsstr(p, L"<Data")) != NULL) {
        const wchar_t *gt = wcschr(p, L'>'), *end;
        if (!gt)
            break;
        if (gt[-1] == L'/') {            /* <Data/>: empty */
            p = gt + 1;
            ++n;
            continue;
        }
        end = wcsstr(gt + 1, L"</Data>");
        if (!end)
            break;
        if (out[0])
            StringCchCatW(out, cch, L" | ");
        StringCchCatNW(out, cch, gt + 1, (size_t)(end - gt - 1));
        if (n == 0)
            StringCchCopyNW(first, cchFirst, gt + 1, (size_t)(end - gt - 1));
        ++n;
        p = end + 7;
    }
}

BOOL Crash_LogWindowsRecord(const wchar_t *exeName, DWORD maxAgeMs)
{
    HMODULE        wevt = LoadLibraryExW(L"wevtapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    PFN_EvtQuery   query;
    PFN_EvtNext    next;
    PFN_EvtRender  render;
    PFN_EvtClose   closeH;
    EVT_HANDLE     q, ev[16];
    DWORD          got = 0, i;
    wchar_t        xpath[400], self[MAX_PATH];
    wchar_t       *xml = NULL;
    BOOL           found = FALSE;

    if (!wevt)
        return FALSE;
    query  = (PFN_EvtQuery)(void *)GetProcAddress(wevt, "EvtQuery");
    next   = (PFN_EvtNext)(void *)GetProcAddress(wevt, "EvtNext");
    render = (PFN_EvtRender)(void *)GetProcAddress(wevt, "EvtRender");
    closeH = (PFN_EvtClose)(void *)GetProcAddress(wevt, "EvtClose");
    if (!query || !next || !render || !closeH) {
        FreeLibrary(wevt);
        return FALSE;
    }
    if (!exeName) {
        const wchar_t *base;
        GetModuleFileNameW(NULL, self, MAX_PATH);
        base = wcsrchr(self, L'\\');
        exeName = base ? base + 1 : self;
    }
    /* Windows' own record of a crash (Application Error, 1000) or of a
       program it gave up on as hung (Application Hang, 1002). It sees what
       no handler inside the process can: a fail-fast, a heap the runtime
       found corrupt, a filter that was replaced. */
    StringCchPrintfW(xpath, ARRAYSIZE(xpath),
                     L"*[System[(Provider[@Name='Application Error'] or "
                     L"Provider[@Name='Application Hang']) and "
                     L"(EventID=1000 or EventID=1002) and "
                     L"TimeCreated[timediff(@SystemTime) <= %lu]]]", maxAgeMs);
    q = query(NULL, L"Application", xpath, EvtQueryChannelPath | EvtQueryReverseDirection);
    if (q && next(q, ARRAYSIZE(ev), ev, 2000, 0, &got)) {
        DWORD cap = 32768;
        xml = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, cap);
        for (i = 0; i < got; ++i) {
            DWORD used = 0, props = 0;
            if (!found && xml && render(NULL, ev[i], EvtRenderEventXml, cap, xml, &used, &props)) {
                wchar_t data[900], app[MAX_PATH];
                const wchar_t *id = wcsstr(xml, L"<EventID");
                JoinData(xml, data, ARRAYSIZE(data), app, ARRAYSIZE(app));
                if (lstrcmpiW(app, exeName) == 0) {
                    const wchar_t *when = wcsstr(xml, L"SystemTime='");
                    if (!when) when = wcsstr(xml, L"SystemTime=\"");
                    found = TRUE;
                    Log_Always(L"crash: Windows recorded %s at %.19s UTC: %s",
                               id && wcsstr(id, L">1002<") ? L"a hang" : L"a crash",
                               when ? when + 12 : L"?", data);
                }
            }
            closeH(ev[i]);
        }
    }
    if (q)
        closeH(q);
    if (xml)
        HeapFree(GetProcessHeap(), 0, xml);
    FreeLibrary(wevt);
    return found;
}

void Crash_Install(void)
{
    FindDir();
    SetUnhandledExceptionFilter(OnCrash);
    AddVectoredExceptionHandler(1, OnFirstChance);
}
