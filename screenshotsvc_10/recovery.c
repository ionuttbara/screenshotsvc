/*
 * recovery.c - startup recovery for interrupted recordings.
 *
 * Interrupted MP4 files are not opened, parsed, or decoded during startup.
 * They are left untouched and deferred to an isolated video-recovery path,
 * because malformed sample tables can crash native decoder components.
 * A partial PCM WAV has its RIFF sizes repaired. FLAC/GIF files are promoted
 * only when their headers and first payload are present. RAW microphone/system
 * streams are converted to WAV when exact WAVEFORMATEX metadata is available.
 * Recovered RAW bundles are aligned from their persisted QPC origins and
 * durations. A low-priority worker validates the matching partial MP4,
 * reconstructs the combined AAC source WAV, creates both FLAC tracks and
 * remuxes a recovered MP4. The synchronous startup scan never opens video.
 */
#define COBJMACROS
#include "screenshotservice.h"
#include "recovery.h"
#include "flacenc.h"
#include "mp4multi.h"
#include "rawmix.h"
#include "remux.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include "log.h"

#include <mmreg.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <string.h>
#include <stdint.h>
#include <process.h>

#define RAW_META_MAGIC "SSRRAW1\0"
#define RAW_META_FORMAT_MAX 256u
#define WAV_RIFF_MAX_DATA 0xFFFFFF00u
#define RECOVERY_COPY_BYTES (1024u * 1024u)
#define RECOVERY_RAW_MIX_RATE 48000u

typedef struct {
    char magic[8];
    DWORD structureBytes;
    DWORD formatBytes;
    DWORD reserved;
    UINT64 origin100ns;
    BYTE format[RAW_META_FORMAT_MAX];
} RawRecoveryMeta;

static void PutU32(BYTE *p, UINT32 value)
{
    p[0] = (BYTE)value;
    p[1] = (BYTE)(value >> 8);
    p[2] = (BYTE)(value >> 16);
    p[3] = (BYTE)(value >> 24);
}

static UINT32 GetU32(const BYTE *p)
{
    return (UINT32)p[0] | ((UINT32)p[1] << 8) |
           ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24);
}

static BOOL EndsWithI(const wchar_t *text, const wchar_t *suffix)
{
    size_t textLen, suffixLen;
    if (!text || !suffix) return FALSE;
    textLen = wcslen(text);
    suffixLen = wcslen(suffix);
    return textLen >= suffixLen && _wcsicmp(text + textLen - suffixLen, suffix) == 0;
}

static BOOL MakeMetaPath(const wchar_t *rawPath, wchar_t *out, size_t cch)
{
    if (FAILED(StringCchCopyW(out, cch, rawPath))) return FALSE;
    return PathRenameExtensionW(out, L".meta");
}

void Recovery_DeleteRawMetadata(const wchar_t *rawPath)
{
    wchar_t meta[MAX_PATH], temp[MAX_PATH];
    if (!rawPath || !rawPath[0]) return;
    if (MakeMetaPath(rawPath, meta, ARRAYSIZE(meta))) {
        DeleteFileW(meta);
        if (FAILED(StringCchCopyW(temp, ARRAYSIZE(temp), meta))) return;
        if (SUCCEEDED(StringCchCatW(temp, ARRAYSIZE(temp), L".tmp")))
            DeleteFileW(temp);
    }
}

BOOL Recovery_WriteRawMetadata(const wchar_t *rawPath,
                               const BYTE *formatBytes,
                               UINT32 formatByteCount,
                               UINT64 origin100ns)
{
    RawRecoveryMeta meta;
    wchar_t metaPath[MAX_PATH], tempPath[MAX_PATH];
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD wrote = 0;
    BOOL ok = FALSE;

    if (!rawPath || !rawPath[0] || !formatBytes ||
        formatByteCount < sizeof(WAVEFORMATEX) ||
        formatByteCount > RAW_META_FORMAT_MAX)
        return FALSE;

    ZeroMemory(&meta, sizeof(meta));
    CopyMemory(meta.magic, RAW_META_MAGIC, sizeof(meta.magic));
    meta.structureBytes = (DWORD)sizeof(meta);
    meta.formatBytes = formatByteCount;
    meta.origin100ns = origin100ns;
    CopyMemory(meta.format, formatBytes, formatByteCount);

    if (!MakeMetaPath(rawPath, metaPath, ARRAYSIZE(metaPath)) ||
        FAILED(StringCchCopyW(tempPath, ARRAYSIZE(tempPath), metaPath)) ||
        FAILED(StringCchCatW(tempPath, ARRAYSIZE(tempPath), L".tmp")))
        return FALSE;

    file = CreateFileW(tempPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return FALSE;
    ok = WriteFile(file, &meta, (DWORD)sizeof(meta), &wrote, NULL) &&
         wrote == (DWORD)sizeof(meta) && FlushFileBuffers(file);
    CloseHandle(file);
    if (ok)
        ok = MoveFileExW(tempPath, metaPath, MOVEFILE_REPLACE_EXISTING |
                                             MOVEFILE_WRITE_THROUGH);
    if (!ok) DeleteFileW(tempPath);
    return ok;
}

/* Replace the filename's .partial marker with .recovered. The remainder is
   kept, so e.g. *.partial.system.raw becomes *.recovered.system.raw. */
static BOOL MakeRecoveredPath(const wchar_t *source, const wchar_t *newExtension,
                              wchar_t *out, size_t cch)
{
    const wchar_t *base, *marker;
    size_t prefixChars;

    if (!source || !out || !cch) return FALSE;
    base = wcsrchr(source, L'\\');
    base = base ? base + 1 : source;
    marker = StrStrIW(base, L".partial");
    if (!marker) return FALSE;
    prefixChars = (size_t)(marker - source);
    if (prefixChars >= cch) return FALSE;

    if (FAILED(StringCchCopyNW(out, cch, source, prefixChars)) ||
        FAILED(StringCchCatW(out, cch, L".recovered")) ||
        FAILED(StringCchCatW(out, cch, marker + 8)))
        return FALSE;
    if (newExtension && newExtension[0] &&
        !PathRenameExtensionW(out, newExtension))
        return FALSE;
    return TRUE;
}

static BOOL MakeTempPath(const wchar_t *destination, wchar_t *out, size_t cch)
{
    if (FAILED(StringCchCopyW(out, cch, destination))) return FALSE;
    return SUCCEEDED(StringCchCatW(out, cch, L".tmp"));
}

/* The recovered WAV is the immediately usable copy; keep the exact original
   PCM and its format metadata under a non-partial name for forensic recovery. */
static void PreserveRecoveredRawSource(const wchar_t *rawPath)
{
    wchar_t rawDestination[MAX_PATH], metaSource[MAX_PATH], metaDestination[MAX_PATH];
    if (!MakeRecoveredPath(rawPath, L".raw", rawDestination, ARRAYSIZE(rawDestination)))
        return;
    if (!PathFileExistsW(rawDestination)) {
        if (!MoveFileExW(rawPath, rawDestination, MOVEFILE_WRITE_THROUGH))
            Log_Printf(L"recovery: WAV was created but original RAW could not be preserved as %s (%lu)",
                       rawDestination, GetLastError());
    } else {
        /* The RAW source may already have been moved by a previous interrupted
           recovery attempt; do not create another copy. */
        if (GetFileAttributesW(rawPath) != INVALID_FILE_ATTRIBUTES)
            Log_Printf(L"recovery: recovered WAV already exists; original RAW remains at %s", rawPath);
    }
    if (MakeMetaPath(rawPath, metaSource, ARRAYSIZE(metaSource)) &&
        MakeRecoveredPath(metaSource, L".meta", metaDestination, ARRAYSIZE(metaDestination)) &&
        !PathFileExistsW(metaDestination) && PathFileExistsW(metaSource))
        (void)MoveFileExW(metaSource, metaDestination, MOVEFILE_WRITE_THROUGH);
}

static BOOL MakeRecoveredWavPath(const wchar_t *source, wchar_t *out, size_t cch)
{
    if (EndsWithI(source, L".partial.wav"))
        return MakeRecoveredPath(source, L".wav", out, cch);
    if (FAILED(StringCchCopyW(out, cch, source)))
        return FALSE;
    /* PathRemoveExtensionW is a void API; it edits the buffer in place. */
    PathRemoveExtensionW(out);
    if (FAILED(StringCchCatW(out, cch, L".recovered.wav")))
        return FALSE;
    return TRUE;
}

static BOOL WavNeedsRepair(const wchar_t *source)
{
    BYTE header[44];
    HANDLE file;
    DWORD got = 0;
    LARGE_INTEGER fileSize;
    UINT32 blockAlign;
    BOOL needsRepair = FALSE;

    file = CreateFileW(source, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
                       FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    if (GetFileSizeEx(file, &fileSize) && fileSize.QuadPart > (LONGLONG)sizeof(header) &&
        ReadFile(file, header, sizeof(header), &got, NULL) && got == sizeof(header) &&
        memcmp(header, "RIFF", 4) == 0 && memcmp(header + 8, "WAVEfmt ", 8) == 0 &&
        GetU32(header + 16) == 16 && header[20] == 1 && header[21] == 0 &&
        memcmp(header + 36, "data", 4) == 0) {
        blockAlign = (UINT32)header[32] | ((UINT32)header[33] << 8);
        if (blockAlign && header[34] == 16 && header[35] == 0 &&
            GetU32(header + 24) != 0 && header[22] != 0 &&
            blockAlign == (UINT32)header[22] * 2u) {
            needsRepair = GetU32(header + 4) != (UINT32)(fileSize.QuadPart - 8) ||
                          GetU32(header + 40) != (UINT32)(fileSize.QuadPart - 44);
        }
    }
    CloseHandle(file);
    return needsRepair;
}

static BOOL RecoverPartialWav(const wchar_t *source, RecoverySummary *summary)
{
    BYTE header[44], sizes[4];
    HANDLE file = INVALID_HANDLE_VALUE;
    LARGE_INTEGER fileSize, pos;
    DWORD got = 0, wrote = 0;
    UINT32 dataBytes, blockAlign;
    wchar_t destination[MAX_PATH];

    if (!MakeRecoveredWavPath(source, destination, ARRAYSIZE(destination)))
        return FALSE;
    if (PathFileExistsW(destination)) return TRUE;

    file = CreateFileW(source, GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &fileSize) ||
        fileSize.QuadPart <= (LONGLONG)sizeof(header) ||
        !ReadFile(file, header, sizeof(header), &got, NULL) || got != sizeof(header))
        goto done;

    /* WavSink writes a canonical 44-byte PCM16 RIFF header and leaves both
       size fields at zero until close. Only repair that exact layout. */
    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVEfmt ", 8) != 0 ||
        GetU32(header + 16) != 16 || header[20] != 1 || header[21] != 0 ||
        memcmp(header + 36, "data", 4) != 0)
        goto done;
    blockAlign = (UINT32)header[32] | ((UINT32)header[33] << 8);
    if (!blockAlign || header[34] != 16 || header[35] != 0 ||
        GetU32(header + 24) == 0 || header[22] == 0 ||
        blockAlign != (UINT32)header[22] * 2u)
        goto done;
    if ((ULONGLONG)(fileSize.QuadPart - (LONGLONG)sizeof(header)) > WAV_RIFF_MAX_DATA)
        goto done;
    dataBytes = (UINT32)(fileSize.QuadPart - (LONGLONG)sizeof(header));
    dataBytes -= dataBytes % blockAlign;
    if (!dataBytes) goto done;

    /* The original remains the same file; only the two RIFF length fields
       are patched, which is much faster and safer than copying a large WAV. */
    PutU32(sizes, 36u + dataBytes);
    pos.QuadPart = 4;
    if (!SetFilePointerEx(file, pos, NULL, FILE_BEGIN) ||
        !WriteFile(file, sizes, sizeof(sizes), &wrote, NULL) || wrote != sizeof(sizes))
        goto done;
    PutU32(sizes, dataBytes);
    pos.QuadPart = 40;
    if (!SetFilePointerEx(file, pos, NULL, FILE_BEGIN) ||
        !WriteFile(file, sizes, sizeof(sizes), &wrote, NULL) || wrote != sizeof(sizes))
        goto done;
    pos.QuadPart = (LONGLONG)sizeof(header) + dataBytes;
    if (!SetFilePointerEx(file, pos, NULL, FILE_BEGIN) || !SetEndOfFile(file) ||
        !FlushFileBuffers(file))
        goto done;
    CloseHandle(file);
    file = INVALID_HANDLE_VALUE;
    if (!MoveFileExW(source, destination, MOVEFILE_WRITE_THROUGH)) {
        Log_Printf(L"recovery: repaired WAV but could not rename %s (%lu)",
                   source, GetLastError());
        summary->needsAttention++;
        return FALSE;
    }
    Log_Printf(L"recovery: repaired WAV sidecar: %s", destination);
    summary->recoveredWav++;
    return TRUE;

done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    Log_Printf(L"recovery: WAV sidecar could not be repaired: %s", source);
    summary->needsAttention++;
    return FALSE;
}

static BOOL FlacHasPayload(const wchar_t *path)
{
    HANDLE file;
    BYTE head[48];
    DWORD got = 0;
    LARGE_INTEGER size;
    BOOL ok = FALSE;

    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
                       FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    if (GetFileSizeEx(file, &size) && size.QuadPart >= 44 &&
        ReadFile(file, head, sizeof(head), &got, NULL) && got == sizeof(head) &&
        memcmp(head, "fLaC", 4) == 0 && (head[4] & 0x7Fu) == 0 &&
        head[5] == 0 && head[6] == 0 && head[7] == 34 &&
        head[42] == 0xFF && (head[43] & 0xFCu) == 0xF8u)
        ok = TRUE;
    CloseHandle(file);
    return ok;
}

static BOOL GifHasTrailer(const wchar_t *path)
{
    HANDLE file;
    BYTE head[6], tail = 0;
    DWORD got = 0;
    LARGE_INTEGER size, pos;
    BOOL ok = FALSE;

    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
                       FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    if (GetFileSizeEx(file, &size) && size.QuadPart >= 14 &&
        ReadFile(file, head, sizeof(head), &got, NULL) && got == sizeof(head) &&
        (memcmp(head, "GIF87a", 6) == 0 || memcmp(head, "GIF89a", 6) == 0)) {
        pos.QuadPart = size.QuadPart - 1;
        if (SetFilePointerEx(file, pos, NULL, FILE_BEGIN) &&
            ReadFile(file, &tail, 1, &got, NULL) && got == 1 && tail == 0x3B)
            ok = TRUE;
    }
    CloseHandle(file);
    return ok;
}

static BOOL PromoteSimpleFile(const wchar_t *source, const wchar_t *extension,
                              RecoverySummary *summary, unsigned *counter,
                              const wchar_t *kind, BOOL (*validator)(const wchar_t *))
{
    wchar_t destination[MAX_PATH];
    if (!MakeRecoveredPath(source, extension, destination, ARRAYSIZE(destination)))
        return FALSE;
    if (PathFileExistsW(destination)) return TRUE;
    if (!validator(source)) {
        Log_Printf(L"recovery: incomplete %s kept unchanged: %s", kind, source);
        summary->needsAttention++;
        return FALSE;
    }
    if (!MoveFileExW(source, destination, MOVEFILE_WRITE_THROUGH)) {
        Log_Printf(L"recovery: could not promote %s %s (%lu)",
                   kind, source, GetLastError());
        summary->needsAttention++;
        return FALSE;
    }
    Log_Printf(L"recovery: recovered %s: %s", kind, destination);
    (*counter)++;
    return TRUE;
}

static BOOL CopyRawToRecoveredWav(const wchar_t *rawPath,
                                  const wchar_t *destination,
                                  const RawRecoveryMeta *meta,
                                  RecoverySummary *summary)
{
    const WAVEFORMATEX *format = (const WAVEFORMATEX *)meta->format;
    DWORD formatBytes, blockAlign, dataBytes, riffBytes, copied = 0;
    ULONGLONG riffBytes64;
    LARGE_INTEGER rawSize;
    HANDLE raw = INVALID_HANDLE_VALUE, wav = INVALID_HANDLE_VALUE;
    wchar_t tempPath[MAX_PATH];
    BYTE header[RAW_META_FORMAT_MAX + 32u];
    BYTE *buffer = NULL;
    DWORD headerBytes, got, wrote;
    ULONGLONG remaining;
    BOOL ok = FALSE;

    tempPath[0] = 0;
    if (meta->formatBytes < sizeof(WAVEFORMATEX) ||
        (UINT32)sizeof(WAVEFORMATEX) + format->cbSize > meta->formatBytes ||
        !format->nSamplesPerSec || !format->nChannels || !format->nBlockAlign ||
        (format->wFormatTag != WAVE_FORMAT_PCM &&
         format->wFormatTag != WAVE_FORMAT_IEEE_FLOAT &&
         format->wFormatTag != WAVE_FORMAT_EXTENSIBLE))
        return FALSE;

    formatBytes = (format->wFormatTag == WAVE_FORMAT_PCM && format->cbSize == 0)
        ? 16u : (DWORD)sizeof(WAVEFORMATEX) + format->cbSize;
    if (formatBytes > sizeof(meta->format)) return FALSE;
    blockAlign = format->nBlockAlign;

    /* A 1 MiB automatic array here can exhaust the default 1 MiB Windows
       thread stack when Recovery_ScanFolder -> RecoverRawAudio calls us.
       Allocate the streaming buffer on the process heap instead. */
    buffer = (BYTE *)HeapAlloc(GetProcessHeap(), 0, RECOVERY_COPY_BYTES);
    if (!buffer) goto done;

    raw = CreateFileW(rawPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
                      FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (raw == INVALID_HANDLE_VALUE || !GetFileSizeEx(raw, &rawSize) || rawSize.QuadPart <= 0)
        goto done;
    remaining = (ULONGLONG)rawSize.QuadPart;
    remaining -= remaining % blockAlign;
    if (!remaining || remaining > WAV_RIFF_MAX_DATA) goto done;
    dataBytes = (DWORD)remaining;

    riffBytes64 = 4ull + 8ull + formatBytes + (formatBytes & 1u) + 8ull + dataBytes;
    if (riffBytes64 > UINT32_MAX) goto done;
    riffBytes = (DWORD)riffBytes64;

    ZeroMemory(header, sizeof(header));
    CopyMemory(header, "RIFF", 4);
    PutU32(header + 4, riffBytes);
    CopyMemory(header + 8, "WAVEfmt ", 8);
    PutU32(header + 16, formatBytes);
    CopyMemory(header + 20, meta->format, formatBytes);
    headerBytes = 20u + formatBytes;
    if (formatBytes & 1u) header[headerBytes++] = 0;
    CopyMemory(header + headerBytes, "data", 4);
    PutU32(header + headerBytes + 4, dataBytes);
    headerBytes += 8;

    if (!MakeTempPath(destination, tempPath, ARRAYSIZE(tempPath))) goto done;
    wav = CreateFileW(tempPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (wav == INVALID_HANDLE_VALUE) goto done;
    if (!WriteFile(wav, header, headerBytes, &wrote, NULL) || wrote != headerBytes)
        goto done;

    while (remaining) {
        DWORD request = remaining > RECOVERY_COPY_BYTES
            ? RECOVERY_COPY_BYTES : (DWORD)remaining;
        if (!ReadFile(raw, buffer, request, &got, NULL) || got != request)
            goto done;
        if (!WriteFile(wav, buffer, got, &wrote, NULL) || wrote != got)
            goto done;
        remaining -= got;
        copied += got;
    }
    if (copied != dataBytes || !FlushFileBuffers(wav)) goto done;
    CloseHandle(wav);
    wav = INVALID_HANDLE_VALUE;
    if (!MoveFileExW(tempPath, destination, MOVEFILE_WRITE_THROUGH))
        goto done;
    Log_Printf(L"recovery: recovered RAW audio as WAV: %s (%lu Hz, %u ch)",
               destination, (unsigned long)format->nSamplesPerSec, format->nChannels);
    summary->recoveredRawAudio++;
    ok = TRUE;

done:
    if (wav != INVALID_HANDLE_VALUE) CloseHandle(wav);
    if (raw != INVALID_HANDLE_VALUE) CloseHandle(raw);
    if (buffer) HeapFree(GetProcessHeap(), 0, buffer);
    if (!ok && tempPath[0]) DeleteFileW(tempPath);
    return ok;
}

static BOOL RecoverRawAudio(const wchar_t *source, RecoverySummary *summary)
{
    RawRecoveryMeta meta;
    wchar_t metaPath[MAX_PATH], destination[MAX_PATH];
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD got = 0;
    BOOL ok;

    if (!MakeMetaPath(source, metaPath, ARRAYSIZE(metaPath)) ||
        !MakeRecoveredPath(source, L".wav", destination, ARRAYSIZE(destination)))
        return FALSE;
    if (PathFileExistsW(destination)) {
        PreserveRecoveredRawSource(source);
        return TRUE;
    }

    file = CreateFileW(metaPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        Log_Printf(L"recovery: RAW stream has no format metadata and cannot be reconstructed: %s", source);
        summary->needsAttention++;
        return FALSE;
    }
    ZeroMemory(&meta, sizeof(meta));
    ok = ReadFile(file, &meta, (DWORD)sizeof(meta), &got, NULL) && got == (DWORD)sizeof(meta) &&
         memcmp(meta.magic, RAW_META_MAGIC, sizeof(meta.magic)) == 0 &&
         meta.structureBytes == (DWORD)sizeof(meta) &&
         meta.formatBytes >= sizeof(WAVEFORMATEX) &&
         meta.formatBytes <= RAW_META_FORMAT_MAX;
    CloseHandle(file);
    if (!ok || !CopyRawToRecoveredWav(source, destination, &meta, summary)) {
        Log_Printf(L"recovery: RAW stream could not be converted to WAV: %s", source);
        summary->needsAttention++;
        return FALSE;
    }
    PreserveRecoveredRawSource(source);
    return TRUE;
}

static BOOL ReadRawMetaFile(const wchar_t *rawPath, RawRecoveryMeta *meta)
{
    wchar_t metaPath[MAX_PATH];
    HANDLE file;
    DWORD got = 0;
    BOOL ok;
    if (!rawPath || !meta || !MakeMetaPath(rawPath, metaPath, ARRAYSIZE(metaPath)))
        return FALSE;
    file = CreateFileW(metaPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ZeroMemory(meta, sizeof(*meta));
    ok = ReadFile(file, meta, (DWORD)sizeof(*meta), &got, NULL) &&
         got == (DWORD)sizeof(*meta) &&
         memcmp(meta->magic, RAW_META_MAGIC, sizeof(meta->magic)) == 0 &&
         meta->structureBytes == (DWORD)sizeof(*meta) &&
         meta->formatBytes >= sizeof(WAVEFORMATEX) &&
         meta->formatBytes <= RAW_META_FORMAT_MAX;
    CloseHandle(file);
    if (!ok) return FALSE;
    {
        const WAVEFORMATEX *fmt = (const WAVEFORMATEX *)meta->format;
        if ((UINT32)sizeof(WAVEFORMATEX) + fmt->cbSize > meta->formatBytes ||
            !fmt->nSamplesPerSec || !fmt->nChannels || fmt->nChannels > 8 ||
            !fmt->nBlockAlign)
            return FALSE;
    }
    return TRUE;
}

static BOOL ReadRawFrameCount(const wchar_t *path, const RawRecoveryMeta *meta,
                              UINT64 *frames)
{
    const WAVEFORMATEX *fmt;
    HANDLE file;
    LARGE_INTEGER size;
    if (!path || !meta || !frames) return FALSE;
    fmt = (const WAVEFORMATEX *)meta->format;
    if (!fmt->nBlockAlign) return FALSE;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
                       FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) {
        CloseHandle(file);
        return FALSE;
    }
    CloseHandle(file);
    *frames = (UINT64)size.QuadPart / fmt->nBlockAlign;
    return *frames != 0;
}

/* The normal multitrack stop path writes this canonical 48 kHz, stereo,
   PCM16 WAV before embedding the combined AAC track. Its sample count tells
   recovery the exact recording duration even when the old start/stop QPC
   values were not persisted. */
static BOOL Recovery_FramesForDuration(UINT64 duration100ns, UINT32 rate, UINT64 *frames)
{
    UINT64 whole, remainder, tail;
    if (!rate || !frames) return FALSE;
    whole = duration100ns / 10000000u;
    remainder = duration100ns % 10000000u;
    if (whole > UINT64_MAX / rate) return FALSE;
    tail = (remainder * (UINT64)rate + 5000000u) / 10000000u;
    if (whole * rate > UINT64_MAX - tail) return FALSE;
    *frames = whole * rate + tail;
    return TRUE;
}

static BOOL Recovery_DurationForFrames(UINT64 frames, UINT32 rate, UINT64 *duration100ns)
{
    UINT64 whole, remainder, tail;
    if (!rate || !duration100ns) return FALSE;
    whole = frames / rate;
    remainder = frames % rate;
    tail = (remainder * 10000000u) / rate;
    if (whole > (UINT64_MAX - tail) / 10000000u) return FALSE;
    *duration100ns = whole * 10000000u + tail;
    return TRUE;
}

static BOOL Mp4HasCompleteTopLevel(const wchar_t *path)
{
    HANDLE file;
    LARGE_INTEGER size, pos;
    BOOL ftyp = FALSE, moov = FALSE, mdat = FALSE, parsed = TRUE;
    unsigned boxes = 0;

    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
                       FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 24) {
        CloseHandle(file);
        return FALSE;
    }

    pos.QuadPart = 0;
    while (pos.QuadPart < size.QuadPart) {
        BYTE head[16];
        DWORD got = 0, headerBytes = 8;
        ULONGLONG boxBytes;
        LONGLONG remaining = size.QuadPart - pos.QuadPart;
        if (remaining < 8 || boxes++ >= 10000 ||
            !SetFilePointerEx(file, pos, NULL, FILE_BEGIN) ||
            !ReadFile(file, head, 8, &got, NULL) || got != 8) {
            parsed = FALSE;
            break;
        }
        boxBytes = ((ULONGLONG)head[0] << 24) | ((ULONGLONG)head[1] << 16) |
                   ((ULONGLONG)head[2] << 8) | (ULONGLONG)head[3];
        if (boxBytes == 1) {
            unsigned i;
            if (remaining < 16 || !ReadFile(file, head + 8, 8, &got, NULL) || got != 8) {
                parsed = FALSE;
                break;
            }
            boxBytes = 0;
            for (i = 8; i < 16; ++i) boxBytes = (boxBytes << 8) | head[i];
            headerBytes = 16;
        } else if (boxBytes == 0) {
            boxBytes = (ULONGLONG)remaining;
        }
        if (boxBytes < headerBytes || boxBytes > (ULONGLONG)remaining) {
            parsed = FALSE;
            break;
        }
        if (memcmp(head + 4, "ftyp", 4) == 0 && boxBytes >= 16) ftyp = TRUE;
        if (memcmp(head + 4, "moov", 4) == 0 && boxBytes > headerBytes) moov = TRUE;
        if (memcmp(head + 4, "mdat", 4) == 0 && boxBytes > headerBytes) mdat = TRUE;
        pos.QuadPart += (LONGLONG)boxBytes;
    }
    if (pos.QuadPart != size.QuadPart) parsed = FALSE;
    CloseHandle(file);
    return parsed && ftyp && moov && mdat;
}

static BOOL Mp4HasReadableStream(const wchar_t *path, DWORD stream, const GUID *expectedMajor)
{
    IMFSourceReader *reader = NULL;
    IMFMediaType *type = NULL;
    IMFSample *sample = NULL;
    GUID major = GUID_NULL;
    HRESULT hr;
    BOOL ok = FALSE;
    DWORD streamIndex = 0, flags = 0;
    LONGLONG timestamp = 0;
    unsigned attempts;

    hr = MFCreateSourceReaderFromURL(path, NULL, &reader);
    if (SUCCEEDED(hr) && reader) {
        hr = IMFSourceReader_GetNativeMediaType(reader, stream, 0, &type);
        if (SUCCEEDED(hr) && type &&
            SUCCEEDED(IMFMediaType_GetGUID(type, &MF_MT_MAJOR_TYPE, &major)) &&
            IsEqualGUID(&major, expectedMajor)) {
            for (attempts = 0; attempts < 8; ++attempts) {
                hr = IMFSourceReader_ReadSample(reader, stream, 0,
                    &streamIndex, &flags, &timestamp, &sample);
                if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) break;
                if (sample) { ok = TRUE; break; }
                if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
            }
        }
    }
    if (sample) IMFSample_Release(sample);
    if (type) IMFMediaType_Release(type);
    if (reader) IMFSourceReader_Release(reader);
    return ok;
}

static BOOL Mp4HasReadableVideoTrack(const wchar_t *path)
{
    return Mp4HasReadableStream(path, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                &MFMediaType_Video);
}

static BOOL Mp4HasReadableAudioTrack(const wchar_t *path)
{
    return Mp4HasReadableStream(path, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                &MFMediaType_Audio);
}

static BOOL RepairOneRecoveredBundle(const wchar_t *micRaw)
{
    wchar_t stem[MAX_PATH], systemRaw[MAX_PATH];
    wchar_t partialVideo[MAX_PATH], recoveredVideo[MAX_PATH], completedVideo[MAX_PATH];
    wchar_t mixWav[MAX_PATH], micFlacPath[MAX_PATH], systemFlacPath[MAX_PATH];
    RawRecoveryMeta micMeta, systemMeta;
    const WAVEFORMATEX *micFmt, *systemFmt;
    FlacEncodedTrack micTrack, systemTrack;
    UINT64 micFrames, systemFrames, micEndDuration, systemEndDuration;
    UINT64 micEnd, systemEnd, commonStart, commonEnd, duration100ns;
    UINT64 mixFrames, micStart, systemStart, micCount, systemCount;
    size_t stemLen, suffixLen;
    BOOL videoReady = FALSE, micOk = FALSE, systemOk = FALSE;
    BOOL mixOk = FALSE, muxOk = FALSE;
    ULONGLONG started = GetTickCount64();

    ZeroMemory(&micTrack, sizeof(micTrack));
    ZeroMemory(&systemTrack, sizeof(systemTrack));
    suffixLen = wcslen(L".recovered.mic.raw");
    if (!micRaw || (stemLen = wcslen(micRaw)) <= suffixLen ||
        _wcsicmp(micRaw + stemLen - suffixLen, L".recovered.mic.raw") != 0 ||
        stemLen - suffixLen >= ARRAYSIZE(stem)) {
        Log_Printf(L"recovery: invalid recovered microphone RAW bundle name: %s", micRaw ? micRaw : L"(null)");
        return FALSE;
    }
    if (FAILED(StringCchCopyNW(stem, ARRAYSIZE(stem), micRaw, stemLen - suffixLen)) ||
        FAILED(StringCchPrintfW(systemRaw, ARRAYSIZE(systemRaw), L"%s.recovered.system.raw", stem)) ||
        FAILED(StringCchPrintfW(partialVideo, ARRAYSIZE(partialVideo), L"%s.partial.mp4", stem)) ||
        FAILED(StringCchPrintfW(recoveredVideo, ARRAYSIZE(recoveredVideo), L"%s.recovered.mp4", stem)) ||
        FAILED(StringCchPrintfW(completedVideo, ARRAYSIZE(completedVideo), L"%s.mp4", stem)) ||
        FAILED(StringCchPrintfW(mixWav, ARRAYSIZE(mixWav), L"%s.recovery.mix.wav", stem)) ||
        FAILED(StringCchPrintfW(micFlacPath, ARRAYSIZE(micFlacPath), L"%s.recovery.mic.flac", stem)) ||
        FAILED(StringCchPrintfW(systemFlacPath, ARRAYSIZE(systemFlacPath), L"%s.recovery.system.flac", stem))) {
        Log_Printf(L"recovery: recovered bundle path exceeds MAX_PATH: %s", micRaw);
        return FALSE;
    }

    if (!PathFileExistsW(systemRaw)) {
        Log_Printf(L"recovery: microphone RAW has no matching system RAW: %s", micRaw);
        goto done;
    }
    if (!ReadRawMetaFile(micRaw, &micMeta) || !ReadRawMetaFile(systemRaw, &systemMeta)) {
        Log_Printf(L"recovery: bundle metadata missing/invalid (mic=%s, system=%s)", micRaw, systemRaw);
        goto done;
    }
    if (!ReadRawFrameCount(micRaw, &micMeta, &micFrames) ||
        !ReadRawFrameCount(systemRaw, &systemMeta, &systemFrames)) {
        Log_Printf(L"recovery: cannot read RAW frame counts (mic=%s, system=%s)", micRaw, systemRaw);
        goto done;
    }

    micFmt = (const WAVEFORMATEX *)micMeta.format;
    systemFmt = (const WAVEFORMATEX *)systemMeta.format;
    commonStart = micMeta.origin100ns > systemMeta.origin100ns
        ? micMeta.origin100ns : systemMeta.origin100ns;
    if ((micMeta.origin100ns > systemMeta.origin100ns
            ? micMeta.origin100ns - systemMeta.origin100ns
            : systemMeta.origin100ns - micMeta.origin100ns) > 10000000u) {
        Log_Printf(L"recovery: RAW clocks differ by more than one second; bundle kept separate: %s", stem);
        goto done;
    }
    if (!Recovery_FramesForDuration(commonStart - micMeta.origin100ns,
                                    micFmt->nSamplesPerSec, &micStart) ||
        !Recovery_FramesForDuration(commonStart - systemMeta.origin100ns,
                                    systemFmt->nSamplesPerSec, &systemStart) ||
        !Recovery_DurationForFrames(micFrames, micFmt->nSamplesPerSec, &micEndDuration) ||
        !Recovery_DurationForFrames(systemFrames, systemFmt->nSamplesPerSec, &systemEndDuration) ||
        micMeta.origin100ns > UINT64_MAX - micEndDuration ||
        systemMeta.origin100ns > UINT64_MAX - systemEndDuration) {
        Log_Printf(L"recovery: invalid RAW duration metadata: %s", stem);
        goto done;
    }
    micEnd = micMeta.origin100ns + micEndDuration;
    systemEnd = systemMeta.origin100ns + systemEndDuration;
    commonEnd = micEnd > systemEnd ? micEnd : systemEnd;
    if (commonEnd <= commonStart) {
        Log_Printf(L"recovery: zero/negative audio duration: %s", stem);
        goto done;
    }
    duration100ns = commonEnd - commonStart;
    if (!Recovery_FramesForDuration(duration100ns, RECOVERY_RAW_MIX_RATE, &mixFrames) ||
        !Recovery_FramesForDuration(duration100ns, micFmt->nSamplesPerSec, &micCount) ||
        !Recovery_FramesForDuration(duration100ns, systemFmt->nSamplesPerSec, &systemCount) ||
        !mixFrames || !micCount || !systemCount ||
        mixFrames > (UINT64)WAV_RIFF_MAX_DATA / 4u) {
        Log_Printf(L"recovery: audio duration exceeds supported recovery limits: %s", stem);
        goto done;
    }

    /* The partial filename must be matched explicitly. Do not require the
       already-renamed final MP4 or the old combined WAV sidecar: both may be
       absent when the process crashed before Stop/finalize. */
    if (PathFileExistsW(recoveredVideo)) {
        videoReady = Mp4HasCompleteTopLevel(recoveredVideo) &&
                     Mp4HasReadableVideoTrack(recoveredVideo);
        if (!videoReady)
            Log_Printf(L"recovery: existing recovered MP4 is not readable; preserving: %s", recoveredVideo);
    } else if (PathFileExistsW(completedVideo)) {
        videoReady = Mp4HasCompleteTopLevel(completedVideo) &&
                     Mp4HasReadableVideoTrack(completedVideo);
        if (videoReady) {
            if (!MoveFileExW(completedVideo, recoveredVideo, MOVEFILE_WRITE_THROUGH)) {
                Log_Printf(L"recovery: could not stage completed MP4 (%lu): %s", GetLastError(), completedVideo);
                videoReady = FALSE;
            }
        }
    } else if (PathFileExistsW(partialVideo)) {
        if (!Mp4HasCompleteTopLevel(partialVideo)) {
            Log_Printf(L"recovery: partial MP4 has no complete ftyp/moov/mdat layout; cannot safely recover video yet: %s", partialVideo);
        } else if (!Mp4HasReadableVideoTrack(partialVideo)) {
            Log_Printf(L"recovery: partial MP4 passed box scan but Media Foundation could not read a video sample: %s", partialVideo);
        } else if (!MoveFileExW(partialVideo, recoveredVideo, MOVEFILE_WRITE_THROUGH)) {
            Log_Printf(L"recovery: could not promote readable partial MP4 (%lu): %s", GetLastError(), partialVideo);
        } else {
            videoReady = TRUE;
            Log_Printf(L"recovery: promoted readable partial video to: %s", recoveredVideo);
        }
    } else {
        Log_Printf(L"recovery: matching video file is missing for audio bundle: %s", stem);
    }
    if (!videoReady) goto done;

    if (Mp4Multi_HasFlacTracks(recoveredVideo)) {
        Log_Printf(L"recovery: MP4 already contains both FLAC tracks; bundle left unchanged: %s", recoveredVideo);
        muxOk = TRUE;
        goto done;
    }

    DeleteFileW(mixWav);
    if (!RawMix_BuildStereoWav(micRaw, micMeta.format, micMeta.formatBytes, micStart,
                               systemRaw, systemMeta.format, systemMeta.formatBytes, systemStart,
                               RECOVERY_RAW_MIX_RATE, mixFrames, mixWav, NULL, NULL)) {
        Log_Printf(L"recovery: could not rebuild combined WAV from recovered RAW streams: %s", stem);
        goto done;
    }
    mixOk = TRUE;

    if (!Mp4HasReadableAudioTrack(recoveredVideo)) {
        if (!Remux_AddWavAudio(recoveredVideo, mixWav, NULL, NULL, duration100ns)) {
            Log_Printf(L"recovery: could not mux combined AAC into recovered video: %s", recoveredVideo);
            goto done;
        }
    } else {
        Log_Printf(L"recovery: existing AAC/audio track detected; avoiding duplicate combined audio: %s", recoveredVideo);
    }

    DeleteFileW(micFlacPath);
    DeleteFileW(systemFlacPath);
    micOk = Flac_EncodeRaw(micRaw, micMeta.format, micMeta.formatBytes,
                           micStart, micCount, 24u, micFlacPath, &micTrack, NULL, NULL);
    systemOk = Flac_EncodeRaw(systemRaw, systemMeta.format, systemMeta.formatBytes,
                              systemStart, systemCount, 24u, systemFlacPath, &systemTrack, NULL, NULL);
    if (!micOk || !systemOk) {
        Log_Printf(L"recovery: FLAC encoding failed (mic=%d system=%d): %s",
                   micOk, systemOk, stem);
        goto done;
    }
    if (!Mp4Multi_AddFlacTracks(recoveredVideo, &micTrack, &systemTrack)) {
        Log_Printf(L"recovery: failed to add microphone/system FLAC tracks; source MP4 retained: %s", recoveredVideo);
        goto done;
    }
    muxOk = Mp4Multi_HasFlacTracks(recoveredVideo);
    if (!muxOk) {
        Log_Printf(L"recovery: final MP4 did not validate both FLAC track labels: %s", recoveredVideo);
        goto done;
    }
    Log_Printf(L"recovery: assembled video + combined AAC + microphone FLAC + system FLAC: %s (%.3f s, %llu ms)",
               recoveredVideo, (double)duration100ns / 10000000.0,
               (unsigned long long)(GetTickCount64() - started));

done:
    Flac_FreeTrack(&micTrack);
    Flac_FreeTrack(&systemTrack);
    if (mixOk) DeleteFileW(mixWav);
    if (micOk) DeleteFileW(micFlacPath);
    if (systemOk) DeleteFileW(systemFlacPath);
    return muxOk;
}

typedef struct {
    wchar_t folder[MAX_PATH];
} RecoveredBundleJob;

static unsigned __stdcall RecoveredBundleWorker(void *context)
{
    RecoveredBundleJob *job = (RecoveredBundleJob *)context;
    wchar_t pattern[MAX_PATH], micRaw[MAX_PATH];
    WIN32_FIND_DATAW data;
    HANDLE find;
    unsigned attempted = 0, repaired = 0;
    BOOL mfStarted = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_FULL));
    if (!mfStarted)
        Log_Printf(L"recovery: Media Foundation unavailable; bundle videos will remain unchanged");
    if (mfStarted && job && SUCCEEDED(StringCchPrintfW(pattern, ARRAYSIZE(pattern),
                                          L"%s\\*.recovered.mic.raw", job->folder))) {
        find = FindFirstFileW(pattern, &data);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                if (FAILED(StringCchPrintfW(micRaw, ARRAYSIZE(micRaw),
                                            L"%s\\%s", job->folder, data.cFileName)))
                    continue;
                attempted++;
                Log_Printf(L"recovery: attempting recovered audio/video bundle: %s", micRaw);
                if (RepairOneRecoveredBundle(micRaw)) repaired++;
            } while (FindNextFileW(find, &data));
            FindClose(find);
        }
    }
    if (attempted)
        Log_Printf(L"recovery: recovered multitrack MP4 bundles checked=%u assembled=%u",
                   attempted, repaired);
    if (mfStarted) MFShutdown();
    if (job) HeapFree(GetProcessHeap(), 0, job);
    return 0;
}

static void ScheduleRecoveredBundleRepair(const wchar_t *folder)
{
    RecoveredBundleJob *job;
    uintptr_t thread;
    if (!folder || !folder[0]) return;
    job = (RecoveredBundleJob *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*job));
    if (!job) return;
    if (FAILED(StringCchCopyW(job->folder, ARRAYSIZE(job->folder), folder))) {
        HeapFree(GetProcessHeap(), 0, job);
        return;
    }
    /* Bundle recovery opens Media Foundation readers and can encode large FLAC
       tracks. Keep it below normal priority with a larger dedicated stack so
       neither UI startup nor the recorder thread performs the heavy work. */
    thread = _beginthreadex(NULL, 4u * 1024u * 1024u,
                            RecoveredBundleWorker, job, 0, NULL);
    if (!thread) {
        Log_Printf(L"recovery: could not schedule recovered MP4 bundle repair");
        HeapFree(GetProcessHeap(), 0, job);
        return;
    }
    SetThreadPriority((HANDLE)thread, THREAD_PRIORITY_BELOW_NORMAL);
    CloseHandle((HANDLE)thread);
}

BOOL Recovery_ScanFolder(const wchar_t *folder, RecoverySummary *summary)
{
    wchar_t pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW data;
    HANDLE find;

    if (!folder || !folder[0] || !summary) return FALSE;
    ZeroMemory(summary, sizeof(*summary));
    if (FAILED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\*.partial.*", folder)))
        return FALSE;

    find = FindFirstFileW(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
            return FALSE;
        /* There may be no .partial files left because a previous startup
           already promoted them to .recovered.*. Continue: the bundle worker
           below still needs to join recovered RAW streams to their MP4. */
    } else {
        do {
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            if (EndsWithI(data.cFileName, L".meta") || EndsWithI(data.cFileName, L".tmp"))
                continue;
            if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", folder, data.cFileName)))
                continue;

            summary->detected++;
            if (EndsWithI(data.cFileName, L".partial.mp4")) {
                /* Do not open, parse, or decode interrupted video files during
                   startup. The previous recovery path passed arbitrary partial
                   MP4 sample tables to Media Foundation; malformed containers
                   could crash a decoder/MFT/driver and block application startup.
                   Preserve the file byte-for-byte for a later isolated recovery. */
                Log_Printf(L"recovery: partial MP4 queued for validated multitrack recovery; synchronous scan left it untouched: %s", path);
            } else if (EndsWithI(data.cFileName, L".partial.wav")) {
                (void)RecoverPartialWav(path, summary);
            } else if (EndsWithI(data.cFileName, L".partial.mic.raw") ||
                       EndsWithI(data.cFileName, L".partial.system.raw")) {
                (void)RecoverRawAudio(path, summary);
            } else if (EndsWithI(data.cFileName, L".partial.mic.flac") ||
                       EndsWithI(data.cFileName, L".partial.system.flac")) {
                (void)PromoteSimpleFile(path, L".flac", summary,
                                        &summary->recoveredFlac, L"FLAC", FlacHasPayload);
            } else if (EndsWithI(data.cFileName, L".partial.gif")) {
                (void)PromoteSimpleFile(path, L".gif", summary,
                                        &summary->recoveredGif, L"GIF", GifHasTrailer);
            } else {
                Log_Printf(L"recovery: unknown partial artifact kept unchanged: %s", path);
                summary->needsAttention++;
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    /* Older builds wrote their in-progress WAV sidecar as *.wav instead of
       *.partial.wav. Only canonical WavSink PCM files with stale RIFF lengths
       are considered, so completed user WAV files are left untouched. */
    if (SUCCEEDED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\*.wav", folder))) {
        find = FindFirstFileW(pattern, &data);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                    EndsWithI(data.cFileName, L".partial.wav"))
                    continue;
                if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", folder, data.cFileName)))
                    continue;
                if (WavNeedsRepair(path)) {
                    summary->detected++;
                    (void)RecoverPartialWav(path, summary);
                }
            } while (FindNextFileW(find, &data));
            FindClose(find);
        }
    }
    /* Join recovered RAW pairs to the matching partial MP4. The worker checks
       the MP4 structure before invoking Media Foundation and reconstructs the
       combined WAV directly from RAW if the old Stop-time sidecar is absent. */
    ScheduleRecoveredBundleRepair(folder);
    Log_Printf(L"recovery: scan complete; found=%u mp4=%u wav=%u flac=%u gif=%u raw-audio=%u attention=%u",
               summary->detected, summary->recoveredMp4, summary->recoveredWav,
               summary->recoveredFlac, summary->recoveredGif,
               summary->recoveredRawAudio, summary->needsAttention);
    return TRUE;
}
