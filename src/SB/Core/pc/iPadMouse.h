#ifndef IPADMOUSE_H
#define IPADMOUSE_H

#include <types.h>

// PC-only: the mouse as a device -- its movement, its buttons and its wheel.
// iMouseLook.h decides what the movement means; the buttons and the wheel are
// rows of the keyboard's token table (KEY_MOUSE_*, KEY_WHEEL_* in
// iPadTokens.h), so a [keyboard] binding names them like keys.
//
// **Raw counts, not the cursor.** While captured, SDL's relative mode reads the
// device through raw input, so the movement has no desktop pointer
// acceleration in it and does not stop at the edge of the screen. Uncaptured --
// `mouse.capture = off`, or a game that is paused -- it is the cursor's
// movement, which is the only thing there is to read while the cursor is free.
//
// Read off an SDL event watch rather than the event queue. iWindowSDL.cpp and
// iPadHostSDL.cpp each drain a range of the queue and drop what they do not
// read, and a watch sees every event as it is posted. It also lets this skip
// the mouse events SDL makes up from a finger or a pen, which iPadTouch.cpp
// already reads as touches.

void iPadMouseInit();
void iPadMouseExit();

// Latches what arrived since the last poll: the wheel notches, the buttons and
// whether anything happened at all. Called once per iPadHostPoll, before
// anything asks. Nothing accumulates while the game does not have focus.
void iPadMousePoll();

// The counts moved since the last take, and zero after. Right and down are
// positive, which is the device's own sign and the screen's.
void iPadMouseTake(F32* dx, F32* dy);

// Whether mouse button `n` (1 to 5) is held, as of the last poll. 1 is the
// left button, 2 the right, 3 the middle, 4 and 5 the side buttons.
S32 iPadMouseButton(S32 n);

// Wheel notches in the last poll: positive up, negative down.
S32 iPadMouseWheel();

// The first button to go down in the last poll, 1 to 5, or 0. For binding
// capture, which waits for a press rather than a hold.
S32 iPadMousePressed();

// Hides the pointer and holds it inside the focused window, so moving the
// mouse turns the camera instead of walking the cursor onto another monitor.
// Idempotent; re-applied when focus moves to another window of ours.
void iPadMouseSetCapture(S32 on);

// Whether the last poll saw the device do anything: movement, a notch, a
// button. A backend asks this to learn which device the player last touched,
// without eating the movement the camera is about to read.
S32 iPadMouseSawInput();

// Whether a mouse is attached at all.
S32 iPadMousePresent();

#endif
