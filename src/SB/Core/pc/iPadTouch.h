#ifndef IPADTOUCH_H
#define IPADTOUCH_H

#include <types.h>

struct iPadHostState;

// PC-only: on-screen controls for a touchscreen, standing in for a controller
// on port 0 the way the keyboard does. input.touch_controls turns them on; auto
// means Android.
//
// A floating stick on the left half, a drag on the right half turns the camera
// (through iCameraLook.h, not the C-stick), and buttons for A B X Y, L R, Z and
// HUD, and Start. The buttons produce the bits the Xbox preset gives the same
// physical buttons. Drawn by rw/touch_overlay.cpp; see iTouchOverlay.h.

void iPadTouchInit();

// Reads the fingers on the screen and, while the controls are showing, adds
// what they hold to `s`. `padOnPort0` hides the controls until the screen is
// touched again.
void iPadTouchPoll(iPadHostState* s, bool padOnPort0);

#endif
