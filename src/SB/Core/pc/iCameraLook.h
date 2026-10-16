#ifndef ICAMERALOOK_H
#define ICAMERALOOK_H

#include <types.h>

// PC-only: the camera turned by a displacement -- a finger dragged across the
// screen, a mouse moved -- rather than by a stick setting a rate. zCamera.cpp's
// orbit reads it and owns the angles; this only says how far the hand moved
// this frame.
//
// Not fed through the C-stick. The retail camera's pitch is a blend between two
// fixed poses that eases back when the stick is let go, and it swings behind
// the player on its own, so a drag written as a stick deflection springs back.
// The stick path also has a deadzone at 32 and a clamp at 110, and its constant
// converts a RATE, so a mouse fed through it would turn less per hand movement
// as the frame rate rises.

// Who is turning the camera. Each source is set on its own and the camera sees
// the sum, so a touchscreen laptop with a mouse plugged in can use either.
enum iCameraLookSource
{
    iCAMERALOOK_TOUCH,
    iCAMERALOOK_MOUSE,
    iCAMERALOOK_SOURCE_COUNT
};

// This frame's turn from one source, in radians, replacing that source's last.
// Yaw is added to pgoal, which grows the way the C-stick pushes it. Pitch:
// positive raises the camera, which looks DOWN -- the engine's pitch is
// positive-down.
void iCameraLookSet(iCameraLookSource source, F32 yaw, F32 pitch);

// Whether a source is steering. The orbit owns the camera while any source is,
// and the retail camera does not swing behind the player by itself.
void iCameraLookSetActive(iCameraLookSource source, S32 on);

// Declared again in xPad.h for zCamera.cpp, which is decomp and already
// includes that header. Yaw and pitch are consumed by the read.
S32 iCameraLookActive();
F32 iCameraLookYaw();
F32 iCameraLookPitch();

#endif
