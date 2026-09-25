#ifndef ICAMERALOOK_H
#define ICAMERALOOK_H

#include <types.h>

// PC-only: the camera turned by a displacement -- a finger dragged across the
// screen -- rather than by a stick setting a rate. zCamera.cpp's orbit reads
// it and owns the angles; this only says how far the hand moved this frame.
//
// Not fed through the C-stick. The retail camera's pitch is a blend between two
// fixed poses that eases back when the stick is let go, and it swings behind
// the player on its own, so a drag written as a stick deflection springs back.

// This frame's turn, in radians, replacing the last. Yaw is added to pgoal,
// which grows the way the C-stick pushes it. Pitch: positive raises the camera,
// which looks DOWN -- the engine's pitch is positive-down.
void iCameraLookSet(F32 yaw, F32 pitch);

// Whether the orbit owns the camera. While it does, the retail camera does not
// swing behind the player by itself.
void iCameraLookSetActive(S32 on);

// Declared again in xPad.h for zCamera.cpp, which is decomp and already
// includes that header. Yaw and pitch are consumed by the read.
S32 iCameraLookActive();
F32 iCameraLookYaw();
F32 iCameraLookPitch();

#endif
