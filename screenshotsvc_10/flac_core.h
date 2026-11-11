/* Minimal, standards-compliant FLAC frame writer used by ScreenshotService's native
   MP4 multi-audio mux path. Verbatim subframes trade file size for simplicity
   and deterministic lossless output; no external codec is required. */
#ifndef SCREENSHOTSERVICE_FLAC_CORE_H
#define SCREENSHOTSERVICE_FLAC_CORE_H

#include <stddef.h>
#include <stdint.h>

int FlacCore_EncodeVerbatimFrame(const int32_t *interleaved, uint32_t frames,
                                 uint32_t channels, uint32_t bitsPerSample,
                                 uint64_t frameNumber, unsigned char **outBytes,
                                 size_t *outSize);
void FlacCore_MakeStreamInfo(uint32_t sampleRate, uint32_t channels,
                             uint32_t bitsPerSample, uint32_t minBlockSize,
                             uint32_t maxBlockSize, uint64_t totalSamples,
                             unsigned char streamInfo[34]);

#endif /* SCREENSHOTSERVICE_FLAC_CORE_H */
