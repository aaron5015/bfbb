#ifndef IPADKEYBOARD_H
#define IPADKEYBOARD_H

#include <types.h>

struct iPadHostState;
struct iPadBind;

// PC-only: the keyboard and the mouse buttons on port 0. A keyboard is not a
// second player. It is read every frame and laid over whatever controller is on
// port 0, so either can be picked up at any moment.
//
// Behind its own seam rather than inside an input backend. Which controller API
// a backend talks to says nothing about how it should read a keyboard.

void iPadKeyboardInit();

// Fills `s` from the keys held right now, or with nothing at all when the
// game does not have focus.
void iPadKeyboardPoll(iPadHostState* s);

// iPadHostCaptureKey's half; see iPadHost.h.
S32 iPadKeyboardCapture(S32 prime, char* out, S32 size);

// The [keyboard] bindings as last loaded, one per row of kPadBindButtons.
const iPadBind* iPadKeyboardBindings();

// The key or mouse button bound to one XPAD_BUTTON_*, by the name config.ini
// writes it as -- "e", "space", "mouse1". NULL when nothing is bound to it, and
// NULL for a chord, which has no single name. The keyboard's half of
// iPadHostBoundInput.
const char* iPadKeyboardBoundInput(U32 xpadButton);

#endif
