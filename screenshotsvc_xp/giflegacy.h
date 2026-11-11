#ifndef SCREENSHOTSERVICE_GIFLEGACY_H
#define SCREENSHOTSERVICE_GIFLEGACY_H
/* Minimal GIF89a writer; no modern Windows APIs or external codecs required. */
int GIF_Start(const char *path, int width, int height, int fps);
int GIF_WriteFrame(const unsigned char *bgra, int width, int height);
int GIF_WriteFrameTimed(const unsigned char *bgra, int width, int height, unsigned int delayCentiseconds);
void GIF_Stop(void);
#endif
