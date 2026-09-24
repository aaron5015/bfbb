// The shapes between the on-screen controls and their renderer. See
// iTouchOverlay.h.

#include "iTouchOverlay.h"

#include <string.h>

namespace
{
    iTouchOverlayShape sShapes[ITOUCHOVERLAY_MAX_SHAPES];
    S32 sShapeCount;

    S32 sWindowWidth;
    S32 sWindowHeight;
} // namespace

void iTouchOverlaySubmit(const iTouchOverlayShape* shapes, S32 count)
{
    if (count > ITOUCHOVERLAY_MAX_SHAPES)
    {
        count = ITOUCHOVERLAY_MAX_SHAPES;
    }
    if (count < 0 || shapes == NULL)
    {
        count = 0;
    }

    if (count > 0)
    {
        memcpy(sShapes, shapes, count * sizeof(iTouchOverlayShape));
    }
    sShapeCount = count;
}

bool iTouchOverlayWindowSize(S32* width, S32* height)
{
    if (sWindowWidth <= 0 || sWindowHeight <= 0)
    {
        return false;
    }

    *width = sWindowWidth;
    *height = sWindowHeight;
    return true;
}

S32 iTouchOverlayShapes(const iTouchOverlayShape** shapes)
{
    *shapes = sShapes;
    return sShapeCount;
}

void iTouchOverlaySetWindowSize(S32 width, S32 height)
{
    sWindowWidth = width;
    sWindowHeight = height;
}
