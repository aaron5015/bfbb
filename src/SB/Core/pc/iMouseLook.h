#ifndef IMOUSELOOK_H
#define IMOUSELOOK_H

#include <types.h>

// PC-only: the mouse pointed at the camera. iPadMouse.h reads the device; this
// decides what the movement means, reads [mouse], and hands the turn to
// iCameraLook.h as its own source.
//
// The pointer is held only while the camera is taking the turn. A camera that
// stops reading it -- the game paused, a menu, a cutscene, a conversation --
// gets the pointer back to the desktop after a moment, and gets it taken again
// the frame it starts reading. Nothing in the game has to say it is paused.

// Called once per iPadHostPoll, after the device has been polled.
void iMouseLookUpdate();

// Whether the mouse is steering the camera at all: [mouse] look, with `auto`
// meaning "while the keyboard and mouse are what the player last touched".
S32 iMouseLookActive();

#endif
