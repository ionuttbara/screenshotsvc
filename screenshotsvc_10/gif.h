#ifndef SCREENSHOTSERVICE_GIF_H
#define SCREENSHOTSERVICE_GIF_H

#include "screenshotservice.h"

typedef void (*GifProgressFn)(int percent, void *context);

/* Decode a finalized MP4 and write an animated GIF using Media Foundation and
   Windows Imaging Component. Frames are sampled at 10 fps and scaled down to
   at most 960 pixels wide; duration is capped by maxDurationMs. */
BOOL Gif_ConvertMp4ToGif(const wchar_t *inputPath, const wchar_t *outputPath,
                         DWORD maxDurationMs, GifProgressFn progress, void *context);

#endif
