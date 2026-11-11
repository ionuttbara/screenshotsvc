#ifndef SCREENSHOTSERVICE_ANNOTATE_H
#define SCREENSHOTSERVICE_ANNOTATE_H
#include "capture.h"
/* Opens the screenshot editor (intended for PrintScreen captures). Cancel keeps
   the original image; Save composites the edits back into img. */
BOOL Annotate_Edit(HWND owner, SnipImage *img);
#endif
