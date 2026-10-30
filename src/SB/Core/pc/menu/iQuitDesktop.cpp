// The pause menu's Quit to Desktop. What it is is in iQuitDesktop.h.

#include "iQuitDesktop.h"

#include "xString.h"
#include "zScene.h"
#include "zUI.h"

S32 iQuitDesktopRequested()
{
    _zUI* yes = (_zUI*)zSceneFindObject(xStrHash(IQUIT_DESKTOP_YES));
    return yes != NULL && (yes->uiFlags & 2) != 0;
}
