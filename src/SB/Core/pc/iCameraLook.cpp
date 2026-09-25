// See iCameraLook.h.

#include "iCameraLook.h"

namespace
{
    S32 sActive;
    F32 sYaw;
    F32 sPitch;
} // namespace

void iCameraLookSet(F32 yaw, F32 pitch)
{
    sYaw = sActive ? yaw : 0.0f;
    sPitch = sActive ? pitch : 0.0f;
}

void iCameraLookSetActive(S32 on)
{
    sActive = on ? 1 : 0;
    if (!sActive)
    {
        sYaw = 0.0f;
        sPitch = 0.0f;
    }
}

S32 iCameraLookActive()
{
    return sActive;
}

F32 iCameraLookYaw()
{
    F32 y = sYaw;
    sYaw = 0.0f;
    return y;
}

F32 iCameraLookPitch()
{
    F32 p = sPitch;
    sPitch = 0.0f;
    return p;
}
