// The mouse pointed at the camera. See iMouseLook.h.

#include "iMouseLook.h"

#include "iCameraLook.h"
#include "iConfig.h"
#include "iHost.h"
#include "iPadHost.h"
#include "iPadMouse.h"

#include <SDL3/SDL.h>

#include <math.h>

namespace
{
    // Radians of turn per count at sensitivity 1, the same on both axes so a
    // diagonal sweep goes where it is pointed. A 400-count sweep -- about 5 cm
    // on an 800 dpi mouse -- turns the camera 80 degrees.
    const F32 kRadPerCount = 0.0035f;

    // How long the camera may go without reading the turn before the pointer
    // is handed back. Long enough to ride out the frames where gameplay keeps
    // its own camera -- a wall jump, a bounce pad -- so the cursor does not
    // flicker into view.
    const F32 kReleaseAfter = 0.5f;

    S32 sActive;
    F32 sIdle = kReleaseAfter;
    F32 sOwedX;
    F32 sOwedY;
    Uint64 sLastNS;

    S32 ResolveActive()
    {
        if (!iPadMousePresent())
        {
            return 0;
        }

        const char* want = iConfigGetString("mouse.look", "auto");

        if (iHostStrCaseCmp(want, "off") == 0)
        {
            return 0;
        }

        if (iHostStrCaseCmp(want, "on") == 0)
        {
            return 1;
        }

        // Follows the device in the player's hands rather than what is plugged
        // in, so a pad on the desk does not stop the mouse working, and picking
        // the pad up stops the mouse fighting it.
        return iPadHostKeyboardIsLive();
    }

    void Drop()
    {
        sOwedX = 0.0f;
        sOwedY = 0.0f;
    }
} // namespace

S32 iMouseLookActive()
{
    return sActive;
}

void iMouseLookUpdate()
{
    Uint64 now = SDL_GetTicksNS();
    F32 dt = (sLastNS != 0) ? (F32)(now - sLastNS) / 1e9f : 0.0f;
    sLastNS = now;
    if (dt > 0.1f)
    {
        dt = 0.1f;
    }

    sActive = ResolveActive();
    iCameraLookSetActive(iCAMERALOOK_MOUSE, sActive);

    F32 dx;
    F32 dy;
    iPadMouseTake(&dx, &dy);

    // Asked every poll so the flag means "since the last poll".
    S32 taken = iCameraLookTaken();

    if (!sActive)
    {
        // Taken and dropped rather than left, so the counts that pile up while
        // a pad is in use do not spin the camera when the mouse comes back.
        Drop();
        sIdle = kReleaseAfter;
        iPadMouseSetCapture(0);
        return;
    }

    sIdle = taken ? 0.0f : sIdle + dt;
    S32 steering = sIdle < kReleaseAfter;

    iPadMouseSetCapture(steering && iConfigGetBool("mouse.capture", TRUE));

    if (!steering)
    {
        // Moving the mouse over a pause menu must not swing the camera the
        // moment play resumes.
        Drop();
        iCameraLookSet(iCAMERALOOK_MOUSE, 0.0f, 0.0f);
        return;
    }

    F32 sens = iConfigGetFloat("mouse.sensitivity", 1.0f);
    if (sens < 0.0f)
    {
        sens = 0.0f;
    }

    if (iConfigGetBool("input.invert_camera_x", FALSE))
    {
        dx = -dx;
    }
    if (iConfigGetBool("input.invert_camera_y", FALSE))
    {
        dy = -dy;
    }

    // Smoothing spreads each count over the next few frames. 0 is none: every
    // count turns the camera in the frame it arrived.
    sOwedX += dx;
    sOwedY += dy;

    F32 tau = (F32)iConfigGetInt("mouse.smoothing", 0) / 1000.0f;
    F32 share = (tau > 0.0f) ? 1.0f - expf(-dt / tau) : 1.0f;
    F32 turnX = sOwedX * share;
    F32 turnY = sOwedY * share;
    sOwedX -= turnX;
    sOwedY -= turnY;

    // Both negated against the raw counts. pgoal grows the way the C-stick
    // pushes it, and the engine's pitch is positive-down while a mouse pushed
    // away should look up.
    F32 k = kRadPerCount * sens;
    iCameraLookSet(iCAMERALOOK_MOUSE, -turnX * k, turnY * k);
}
