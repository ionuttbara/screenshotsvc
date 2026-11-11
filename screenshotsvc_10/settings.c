/*
 * settings.c - registry-backed settings, one-time INI import, save folders,
 * and autostart.
 */

#include "screenshotservice.h"
#include "settings.h"
#include "log.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <strsafe.h>

Settings g_cfg;

#define REG_BASE L"Software\\Gallery Inc\\ScreenshotSvc"
#define REG_CACHE_SUBKEY L"EncoderCache"
#define REG_STORAGE_VERSION L"StorageVersion"
#define REG_STORAGE_VERSION_CURRENT 1u
#define SEC L"ScreenshotService"

static wchar_t g_legacyIniPath[MAX_PATH];
static BOOL g_registryWritable;
static LONG g_registryWriteErrorLogged;

/*
 * The program was called BetterSnippingTool until 0.8. Its data folder, INI
 * section and autostart value carried that name. We still read the old INI
 * once, but no code writes settings back to it.
 */
#define LEGACY_NAME_W L"BetterSnippingTool"
#define OLD_APP_NAME_W L"Nitshot"

static BOOL RegistryPath(wchar_t *out, size_t cch, const wchar_t *subKey)
{
    HRESULT hr;
    if (!out || !cch) return FALSE;
    if (subKey && subKey[0])
        hr = StringCchPrintfW(out, cch, L"%s\\%s", REG_BASE, subKey);
    else
        hr = StringCchCopyW(out, cch, REG_BASE);
    return SUCCEEDED(hr);
}

static BOOL RegistryValueExists(const wchar_t *subKey, const wchar_t *name)
{
    wchar_t path[256];
    HKEY key = NULL;
    DWORD type = 0, cb = 0;
    LSTATUS rc;
    BOOL exists = FALSE;
    if (!RegistryPath(path, ARRAYSIZE(path), subKey)) return FALSE;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        rc = RegQueryValueExW(key, name, NULL, &type, NULL, &cb);
        exists = rc == ERROR_SUCCESS || rc == ERROR_MORE_DATA;
        RegCloseKey(key);
    }
    return exists;
}

BOOL Settings_RegGetString(const wchar_t *subKey, const wchar_t *name,
                           const wchar_t *def, wchar_t *out, size_t cch)
{
    wchar_t path[256];
    HKEY key = NULL;
    DWORD type = 0, cb = 0;
    LSTATUS rc;

    if (!out || !cch) return FALSE;
    out[0] = 0;
    if (def) (void)StringCchCopyW(out, cch, def);
    if (!RegistryPath(path, ARRAYSIZE(path), subKey)) return FALSE;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return FALSE;

    rc = RegQueryValueExW(key, name, NULL, &type, NULL, &cb);
    if (rc == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) {
        DWORD dataBytes = (DWORD)(cch * sizeof(wchar_t));
        rc = RegQueryValueExW(key, name, NULL, &type, (BYTE *)out, &dataBytes);
        if (rc == ERROR_SUCCESS) {
            size_t chars = dataBytes / sizeof(wchar_t);
            if (chars >= cch) out[cch - 1] = L'\0';
            else out[chars] = L'\0';
            RegCloseKey(key);
            return TRUE;
        }
    } else if (rc == ERROR_SUCCESS && type == REG_DWORD) {
        DWORD value = 0;
        cb = sizeof(value);
        if (RegQueryValueExW(key, name, NULL, &type, (BYTE *)&value, &cb) == ERROR_SUCCESS &&
            cb == sizeof(value)) {
            (void)StringCchPrintfW(out, cch, L"%lu", (unsigned long)value);
            RegCloseKey(key);
            return TRUE;
        }
    }
    RegCloseKey(key);
    if (def) (void)StringCchCopyW(out, cch, def);
    else out[0] = 0;
    return FALSE;
}

int Settings_RegGetInt(const wchar_t *subKey, const wchar_t *name, int def)
{
    wchar_t value[64], *end = NULL;
    long parsed;
    if (!Settings_RegGetString(subKey, name, NULL, value, ARRAYSIZE(value)) || !value[0])
        return def;
    parsed = wcstol(value, &end, 10);
    if (end == value) return def;
    if (parsed > INT_MAX) return INT_MAX;
    if (parsed < INT_MIN) return INT_MIN;
    return (int)parsed;
}

BOOL Settings_RegSetString(const wchar_t *subKey, const wchar_t *name,
                           const wchar_t *value)
{
    wchar_t path[256];
    HKEY key = NULL;
    LSTATUS rc;
    DWORD bytes;

    if (!RegistryPath(path, ARRAYSIZE(path), subKey) || !name || !value)
        return FALSE;
    rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, NULL, 0,
                         KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_CREATE_SUB_KEY,
                         NULL, &key, NULL);
    if (rc == ERROR_SUCCESS) {
        bytes = (DWORD)((wcslen(value) + 1) * sizeof(wchar_t));
        rc = RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)value, bytes);
        RegCloseKey(key);
    }
    if (rc != ERROR_SUCCESS) {
        if (rc == ERROR_ACCESS_DENIED) g_registryWritable = FALSE;
        if (InterlockedCompareExchange(&g_registryWriteErrorLogged, 1, 0) == 0)
            Log_Always(L"settings: registry write failed at HKLM\\%s\\%s (error %lu); run setup-registry-settings.bat as administrator",
                       path, name, (unsigned long)rc);
        return FALSE;
    }
    return TRUE;
}

BOOL Settings_RegSetInt(const wchar_t *subKey, const wchar_t *name, int value)
{
    wchar_t text[32];
    if (FAILED(StringCchPrintfW(text, ARRAYSIZE(text), L"%d", value))) return FALSE;
    return Settings_RegSetString(subKey, name, text);
}

BOOL Settings_RegistryWritable(void)
{
    return g_registryWritable;
}

/* The legacy INI path is used only as a read-only import source. */
static const wchar_t *LegacyIniPath(void)
{
    PWSTR appdata = NULL;
    if (g_legacyIniPath[0]) return g_legacyIniPath;

    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_RoamingAppData, 0, NULL, &appdata))) {
        wchar_t dir[MAX_PATH], old[MAX_PATH];
        if (SUCCEEDED(StringCchPrintfW(dir, ARRAYSIZE(dir), L"%s\\%s", appdata, APP_NAME_W))) {
            /* Move old profile folders only when the current one is absent.
               This preserves logs and user-language overrides during upgrade. */
            if (GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES) {
                if (SUCCEEDED(StringCchPrintfW(old, ARRAYSIZE(old), L"%s\\%s", appdata, OLD_APP_NAME_W)) &&
                    PathIsDirectoryW(old))
                    MoveFileExW(old, dir, 0);
                if (GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES &&
                    SUCCEEDED(StringCchPrintfW(old, ARRAYSIZE(old), L"%s\\%s", appdata, LEGACY_NAME_W)) &&
                    PathIsDirectoryW(old))
                    MoveFileExW(old, dir, 0);
            }
            /* The profile directory still holds logs and optional languages;
               creating it does not create or write settings.ini. */
            SHCreateDirectoryExW(NULL, dir, NULL);
            (void)StringCchPrintfW(g_legacyIniPath, ARRAYSIZE(g_legacyIniPath),
                                   L"%s\\settings.ini", dir);
        }
        CoTaskMemFree(appdata);
    }

    if (!g_legacyIniPath[0]) {
        GetModuleFileNameW(NULL, g_legacyIniPath, ARRAYSIZE(g_legacyIniPath));
        PathRemoveFileSpecW(g_legacyIniPath);
        (void)StringCchCatW(g_legacyIniPath, ARRAYSIZE(g_legacyIniPath), L"\\settings.ini");
    }
    return g_legacyIniPath;
}

#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

static void MigrateLegacyAutostart(void)
{
    const wchar_t *oldNames[2] = { OLD_APP_NAME_W, LEGACY_NAME_W };
    HKEY key;
    unsigned i;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0,
                      KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return;
    for (i = 0; i < ARRAYSIZE(oldNames); ++i) {
        if (RegQueryValueExW(key, oldNames[i], NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
            (void)RegDeleteValueW(key, oldNames[i]);
            RegCloseKey(key);
            (void)Settings_SetAutostart(TRUE);
            return;
        }
    }
    RegCloseKey(key);
}

static BOOL IniSectionHasValues(const wchar_t *ini, const wchar_t *section)
{
    wchar_t probe[32767];
    DWORD chars;
    ZeroMemory(probe, sizeof(probe));
    chars = GetPrivateProfileSectionW(section, probe, ARRAYSIZE(probe), ini);
    return chars > 0 && probe[0] != 0;
}

static void ImportIniSection(const wchar_t *ini, const wchar_t *section,
                             const wchar_t *subKey)
{
    static wchar_t entries[32767];
    DWORD chars;
    wchar_t *p, *end;

    ZeroMemory(entries, sizeof(entries));
    chars = GetPrivateProfileSectionW(section, entries, ARRAYSIZE(entries), ini);
    if (!chars) return;
    end = entries + (chars < ARRAYSIZE(entries) ? chars : ARRAYSIZE(entries) - 1);
    for (p = entries; p < end && *p; p += lstrlenW(p) + 1) {
        wchar_t *equals = wcschr(p, L'=');
        if (!equals || equals == p) continue;
        *equals = L'\0';
        /* Existing machine settings take precedence. The INI is imported only
           into missing values, so rerunning migration is safe and idempotent. */
        if (!RegistryValueExists(subKey, p))
            (void)Settings_RegSetString(subKey, p, equals + 1);
        *equals = L'=';
    }
}

static void WriteStorageVersion(DWORD version)
{
    HKEY key = NULL;
    LSTATUS rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, REG_BASE, 0, NULL, 0,
        KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_CREATE_SUB_KEY, NULL, &key, NULL);
    if (rc == ERROR_SUCCESS) {
        rc = RegSetValueExW(key, REG_STORAGE_VERSION, 0, REG_DWORD,
                            (const BYTE *)&version, sizeof(version));
        RegCloseKey(key);
    }
    if (rc != ERROR_SUCCESS)
        Log_Always(L"settings: could not write registry migration marker (error %lu)",
                   (unsigned long)rc);
}

static DWORD ReadStorageVersion(void)
{
    HKEY key = NULL;
    DWORD type = 0, version = 0, cb = sizeof(version);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, REG_BASE, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return 0;
    if (RegQueryValueExW(key, REG_STORAGE_VERSION, NULL, &type,
                         (BYTE *)&version, &cb) != ERROR_SUCCESS ||
        type != REG_DWORD || cb != sizeof(version))
        version = 0;
    RegCloseKey(key);
    return version;
}

static void MigrateLegacyIniIfNeeded(void)
{
    const wchar_t *ini;
    const wchar_t *mainSection = SEC;
    DWORD version;
    HKEY key = NULL;
    LSTATUS rc;

    rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, REG_BASE, 0, NULL, 0,
                         KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_CREATE_SUB_KEY,
                         NULL, &key, NULL);
    if (rc != ERROR_SUCCESS) {
        g_registryWritable = FALSE;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, REG_BASE, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
            Log_Always(L"settings: registry key is readable but not writable; settings changes will not persist until registry permissions are configured (error %lu)",
                       (unsigned long)rc);
            RegCloseKey(key);
        } else {
            Log_Always(L"settings: cannot create/open HKLM\\%s (error %lu); run setup-registry-settings.bat as administrator",
                       REG_BASE, (unsigned long)rc);
        }
        return;
    }
    RegCloseKey(key);
    g_registryWritable = TRUE;

    version = ReadStorageVersion();
    if (version >= REG_STORAGE_VERSION_CURRENT) return;

    ini = LegacyIniPath();
    if (GetFileAttributesW(ini) != INVALID_FILE_ATTRIBUTES) {
        if (!IniSectionHasValues(ini, SEC)) {
            if (IniSectionHasValues(ini, OLD_APP_NAME_W)) mainSection = OLD_APP_NAME_W;
            else if (IniSectionHasValues(ini, LEGACY_NAME_W)) mainSection = LEGACY_NAME_W;
        }
        if (IniSectionHasValues(ini, mainSection)) {
            ImportIniSection(ini, mainSection, NULL);
            Log_Always(L"settings: imported legacy INI section [%s] into HKLM\\%s",
                       mainSection, REG_BASE);
        }
        ImportIniSection(ini, L"EncoderCache", REG_CACHE_SUBKEY);
    }

    WriteStorageVersion(REG_STORAGE_VERSION_CURRENT);
}

static BOOL GetBool(const wchar_t *key, BOOL def)
{
    return Settings_RegGetInt(NULL, key, def ? 1 : 0) != 0;
}

static void PutBool(const wchar_t *key, BOOL value)
{
    (void)Settings_RegSetInt(NULL, key, value ? 1 : 0);
}

static void PutInt(const wchar_t *key, int value)
{
    (void)Settings_RegSetInt(NULL, key, value);
}

static int GetSignedInt(const wchar_t *key, int def)
{
    return Settings_RegGetInt(NULL, key, def);
}

void Settings_Load(void)
{
    int mode;

    MigrateLegacyIniIfNeeded();
    MigrateLegacyAutostart();

    g_cfg.copyToClipboard = GetBool(L"CopyToClipboard", TRUE);
    g_cfg.saveToDisk      = GetBool(L"SaveToDisk", TRUE);
    (void)Settings_RegGetString(NULL, L"SaveFolder", L"", g_cfg.saveFolder, ARRAYSIZE(g_cfg.saveFolder));
    (void)Settings_RegGetString(NULL, L"VideoFolder", L"", g_cfg.videoFolder, ARRAYSIZE(g_cfg.videoFolder));

    g_cfg.hookWinShiftS = GetBool(L"HookWinShiftS", TRUE);
    g_cfg.hookPrintScreen = GetBool(L"HookPrintScreen", TRUE);
    g_cfg.prtScnFullscreen = GetBool(L"PrtScnFullscreen", FALSE);

    mode = Settings_RegGetInt(NULL, L"DefaultMode", SNIP_RECT);
    if (mode < 0 || mode >= SNIP_MODE_COUNT) mode = SNIP_RECT;
    g_cfg.defaultMode = (SnipMode)mode;

    g_cfg.dimPercent = Settings_RegGetInt(NULL, L"DimPercent", 40);
    if (g_cfg.dimPercent < 0) g_cfg.dimPercent = 0;
    if (g_cfg.dimPercent > 90) g_cfg.dimPercent = 90;

    g_cfg.showToast = GetBool(L"ShowToast", TRUE);
    g_cfg.hdrSaveJxr = GetBool(L"HdrSaveJxr", TRUE);
    g_cfg.sdrSaveJxr = GetBool(L"SdrSaveJxr", TRUE);
    g_cfg.hdrSdrSidecar = GetBool(L"HdrSdrSidecar", TRUE);
    g_cfg.hdrSdrRollOff = GetBool(L"HdrSdrRollOff", FALSE);

    g_cfg.hdrJxrQuality = Settings_RegGetInt(NULL, L"HdrJxrQuality", 90);
    if (g_cfg.hdrJxrQuality < 1) g_cfg.hdrJxrQuality = 1;
    if (g_cfg.hdrJxrQuality > 100) g_cfg.hdrJxrQuality = 100;
    g_cfg.forceGdi = GetBool(L"ForceGdi", FALSE);
    g_cfg.recordAudio = Settings_RegGetInt(NULL, L"RecordAudio", 3);
    if (g_cfg.recordAudio < 0 || g_cfg.recordAudio > 3) g_cfg.recordAudio = 3;
    g_cfg.recordFps = Settings_RegGetInt(NULL, L"RecordFps", 30);
    if (g_cfg.recordFps < 5) g_cfg.recordFps = 5;
    if (g_cfg.recordFps > 240) g_cfg.recordFps = 240;

    g_cfg.recordRawAudio = GetBool(L"RecordRawAudio", TRUE);
    (void)Settings_RegGetString(NULL, L"RecordEncoder", L"auto", g_cfg.recordEncoder, ARRAYSIZE(g_cfg.recordEncoder));
    (void)Settings_RegGetString(NULL, L"RecordEncoderVendor", L"auto", g_cfg.recordEncoderVendor, ARRAYSIZE(g_cfg.recordEncoderVendor));
    if (_wcsicmp(g_cfg.recordEncoderVendor, L"amd") != 0 &&
        _wcsicmp(g_cfg.recordEncoderVendor, L"intel") != 0 &&
        _wcsicmp(g_cfg.recordEncoderVendor, L"nvidia") != 0)
        StringCchCopyW(g_cfg.recordEncoderVendor, ARRAYSIZE(g_cfg.recordEncoderVendor), L"auto");
    g_cfg.recordHdr = GetBool(L"RecordHdr", TRUE);
    g_cfg.recordWavSidecar = GetBool(L"RecordWavSidecar", FALSE);
    g_cfg.recordMuxFromWav = GetBool(L"RecordMuxFromWav", FALSE);
    /* Three-track recording is the default. Existing explicit user choices are
       preserved; do not silently disable multitrack on upgrade. */
    g_cfg.recordMultiTrack = GetBool(L"RecordMultiTrack", TRUE);
    g_cfg.recordFullscreen = GetBool(L"RecordFullscreen", FALSE);
    g_cfg.recordVsync = GetBool(L"RecordVsync", TRUE);
    g_cfg.monitorForceLast = GetBool(L"MonitorForceLast", FALSE);
    (void)Settings_RegGetString(NULL, L"MonitorTargetPath", L"", g_cfg.monitorTargetPath, ARRAYSIZE(g_cfg.monitorTargetPath));
    (void)Settings_RegGetString(NULL, L"MonitorDeviceName", L"", g_cfg.monitorDeviceName, ARRAYSIZE(g_cfg.monitorDeviceName));
    g_cfg.recordZeroCopy = GetBool(L"RecordZeroCopy", TRUE);
    g_cfg.recordDirect = GetBool(L"RecordDirectEncoder", GetBool(L"RecordNvenc", TRUE));
    g_cfg.recordHwPreset = Settings_RegGetInt(NULL, L"RecordEncoderPreset",
        Settings_RegGetInt(NULL, L"RecordNvencPreset", 5));
    if (g_cfg.recordHwPreset < 1) g_cfg.recordHwPreset = 1;
    if (g_cfg.recordHwPreset > 7) g_cfg.recordHwPreset = 7;
    g_cfg.recordHwBFrames = GetSignedInt(L"RecordEncoderBFrames",
        GetSignedInt(L"RecordNvencBFrames", -1));
    if (g_cfg.recordHwBFrames < -1) g_cfg.recordHwBFrames = -1;
    if (g_cfg.recordHwBFrames > 5) g_cfg.recordHwBFrames = 5;
    g_cfg.recordHwLookahead = Settings_RegGetInt(NULL, L"RecordEncoderLookahead",
        Settings_RegGetInt(NULL, L"RecordNvencLookahead", 16));
    if (g_cfg.recordHwLookahead < 0) g_cfg.recordHwLookahead = 0;
    if (g_cfg.recordHwLookahead > 32) g_cfg.recordHwLookahead = 32;
    g_cfg.recordHwAq = GetBool(L"RecordEncoderAq", GetBool(L"RecordNvencAq", FALSE));
    g_cfg.recordIntelVideoMemory = GetBool(L"RecordIntelVideoMemory", FALSE);

    (void)Settings_RegGetString(NULL, L"Language", L"auto", g_cfg.language, ARRAYSIZE(g_cfg.language));
    g_cfg.settingsX = GetSignedInt(L"SettingsX", SETTINGS_POS_UNSET);
    g_cfg.settingsY = GetSignedInt(L"SettingsY", SETTINGS_POS_UNSET);

    g_cfg.idleReleaseMinutes = Settings_RegGetInt(NULL, L"IdleReleaseMinutes", 5);
    if (g_cfg.idleReleaseMinutes < 0) g_cfg.idleReleaseMinutes = 0;
    if (g_cfg.idleReleaseMinutes > 1440) g_cfg.idleReleaseMinutes = 1440;
    g_cfg.forceSdr = GetBool(L"ForceSdr", FALSE);
    g_cfg.debugLog = GetBool(L"DebugLog", FALSE);
}

void Settings_Save(void)
{
    PutBool(L"CopyToClipboard", g_cfg.copyToClipboard);
    PutBool(L"SaveToDisk", g_cfg.saveToDisk);
    (void)Settings_RegSetString(NULL, L"SaveFolder", g_cfg.saveFolder);
    (void)Settings_RegSetString(NULL, L"VideoFolder", g_cfg.videoFolder);

    PutBool(L"HookWinShiftS", g_cfg.hookWinShiftS);
    PutBool(L"HookPrintScreen", g_cfg.hookPrintScreen);
    PutBool(L"PrtScnFullscreen", g_cfg.prtScnFullscreen);
    PutInt(L"DefaultMode", (int)g_cfg.defaultMode);
    PutInt(L"DimPercent", g_cfg.dimPercent);
    PutBool(L"ShowToast", g_cfg.showToast);
    PutBool(L"HdrSaveJxr", g_cfg.hdrSaveJxr);
    PutBool(L"SdrSaveJxr", g_cfg.sdrSaveJxr);
    PutBool(L"HdrSdrSidecar", g_cfg.hdrSdrSidecar);
    PutBool(L"HdrSdrRollOff", g_cfg.hdrSdrRollOff);
    PutInt(L"HdrJxrQuality", g_cfg.hdrJxrQuality);
    PutBool(L"ForceGdi", g_cfg.forceGdi);
    PutInt(L"RecordAudio", g_cfg.recordAudio);
    PutInt(L"RecordFps", g_cfg.recordFps);
    PutBool(L"RecordRawAudio", g_cfg.recordRawAudio);
    (void)Settings_RegSetString(NULL, L"RecordEncoder", g_cfg.recordEncoder);
    (void)Settings_RegSetString(NULL, L"RecordEncoderVendor", g_cfg.recordEncoderVendor);
    PutBool(L"RecordHdr", g_cfg.recordHdr);
    PutBool(L"RecordWavSidecar", g_cfg.recordWavSidecar);
    PutBool(L"RecordMuxFromWav", g_cfg.recordMuxFromWav);
    PutBool(L"RecordMultiTrack", g_cfg.recordMultiTrack);
    PutBool(L"RecordFullscreen", g_cfg.recordFullscreen);
    PutBool(L"RecordVsync", g_cfg.recordVsync);
    PutBool(L"MonitorForceLast", g_cfg.monitorForceLast);
    (void)Settings_RegSetString(NULL, L"MonitorTargetPath", g_cfg.monitorTargetPath);
    (void)Settings_RegSetString(NULL, L"MonitorDeviceName", g_cfg.monitorDeviceName);
    PutBool(L"RecordZeroCopy", g_cfg.recordZeroCopy);
    PutBool(L"RecordDirectEncoder", g_cfg.recordDirect);
    PutInt(L"RecordEncoderPreset", g_cfg.recordHwPreset);
    PutInt(L"RecordEncoderBFrames", g_cfg.recordHwBFrames);
    PutInt(L"RecordEncoderLookahead", g_cfg.recordHwLookahead);
    PutBool(L"RecordEncoderAq", g_cfg.recordHwAq);
    PutBool(L"RecordIntelVideoMemory", g_cfg.recordIntelVideoMemory);
    (void)Settings_RegSetString(NULL, L"Language", g_cfg.language);
    PutInt(L"SettingsX", g_cfg.settingsX);
    PutInt(L"SettingsY", g_cfg.settingsY);
    PutInt(L"IdleReleaseMinutes", g_cfg.idleReleaseMinutes);
    PutBool(L"ForceSdr", g_cfg.forceSdr);
    PutBool(L"DebugLog", g_cfg.debugLog);
}

/* 'custom' if set, otherwise C:\Screenshots; created on demand. */
static BOOL ResolveFolder(const wchar_t *custom, REFKNOWNFOLDERID known,
                         wchar_t *out, size_t cch)
{
    if (!out || cch == 0)
        return FALSE;
    out[0] = 0;

    if (custom && custom[0]) {
        if (FAILED(StringCchCopyW(out, cch, custom)))
            return FALSE;
    } else {
        UNREFERENCED_PARAMETER(known);
        if (FAILED(StringCchCopyW(out, cch, L"C:\\Screenshots")))
            return FALSE;
    }

    /* ERROR_ALREADY_EXISTS is the common case and is not a failure. */
    {
        int rc = SHCreateDirectoryExW(NULL, out, NULL);
        if (rc != ERROR_SUCCESS && rc != ERROR_ALREADY_EXISTS && rc != ERROR_FILE_EXISTS)
            return FALSE;
    }
    return TRUE;
}

BOOL Settings_ResolveFolderFrom(const wchar_t *custom, BOOL videos,
                                wchar_t *out, size_t cch)
{
    return ResolveFolder(custom, videos ? &FOLDERID_Videos : &FOLDERID_Pictures,
                         out, cch);
}

BOOL Settings_ResolveSaveFolder(wchar_t *out, size_t cch)
{
    return ResolveFolder(g_cfg.saveFolder, &FOLDERID_Pictures, out, cch);
}

BOOL Settings_DefaultFolder(BOOL videos, wchar_t *out, size_t cch)
{
    return ResolveFolder(NULL, videos ? &FOLDERID_Videos : &FOLDERID_Pictures,
                         out, cch);
}

BOOL Settings_ResolveVideoFolder(wchar_t *out, size_t cch)
{
    /* Videos, not Pictures: a recording is not a screenshot, and every other
       tool on the system puts captures there. */
    return ResolveFolder(g_cfg.videoFolder, &FOLDERID_Videos, out, cch);
}


BOOL Settings_GetAutostart(void)
{
    HKEY  key;
    BOOL  found = FALSE;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return FALSE;
    found = RegQueryValueExW(key, APP_NAME_W, NULL, NULL, NULL, NULL) == ERROR_SUCCESS;
    RegCloseKey(key);
    return found;
}

BOOL Settings_SetAutostart(BOOL enable)
{
    HKEY    key;
    LSTATUS rc;

    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS)
        return FALSE;

    if (enable) {
        wchar_t exe[MAX_PATH], quoted[MAX_PATH + 2];
        DWORD   n = GetModuleFileNameW(NULL, exe, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) {
            RegCloseKey(key);
            return FALSE;
        }
        StringCchPrintfW(quoted, MAX_PATH + 2, L"\"%s\"", exe);
        rc = RegSetValueExW(key, APP_NAME_W, 0, REG_SZ, (const BYTE *)quoted,
                            (DWORD)((wcslen(quoted) + 1) * sizeof(wchar_t)));
    } else {
        rc = RegDeleteValueW(key, APP_NAME_W);
        if (rc == ERROR_FILE_NOT_FOUND)
            rc = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}


/* A marker is written only while a native hardware encoder is active. If the
   process dies, the next start can disable that backend once and report it. */
void Settings_MarkDirectRecording(const wchar_t *encoder)
{
    (void)Settings_RegSetString(NULL, L"DirectRecordingRunning", encoder ? encoder : L"");
}

BOOL Settings_TakeDirectCrash(wchar_t *encoder, size_t cch)
{
    wchar_t name[64];
    if (!Settings_RegGetString(NULL, L"DirectRecordingRunning", L"", name,
                               ARRAYSIZE(name)) || !name[0])
        return FALSE;
    Settings_MarkDirectRecording(NULL);
    if (encoder && cch)
        StringCchCopyW(encoder, cch, name);
    return TRUE;
}
