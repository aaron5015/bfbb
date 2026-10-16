#ifndef IMOUSELOOK_H
#define IMOUSELOOK_H

#include <types.h>

// PC-only: the mouse pointed at the camera. iPadMouse.h reads the device; this
// decides what the movement means, reads [mouse], and hands the turn to
// iCameraLook.h as its own source.
//
// The pointer is held through play, cutscenes and dialogue, and let go while a
// menu is up, which iMenuMouse.cpp turns into a pointer of the game's own.

// Called once per iPadHostPoll, after the device has been polled.
void iMouseLookUpdate();

// Whether the mouse is steering the camera at all: [mouse] look, with `auto`
// meaning "while the keyboard and mouse are what the player last touched".
S32 iMouseLookActive();

#endif
