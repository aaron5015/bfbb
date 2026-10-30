#ifndef IPADTOKENS_H
#define IPADTOKENS_H

#include "iPadBind.h"

// PC-only: the device inputs a binding's right-hand side names, for both
// devices. The game's SDL backends read them, and so does bfbb_config, which
// is why they are here rather than inside either backend: the configurator
// parses and writes bindings without linking the game.

// The controller's inputs. The token names are POSITIONS, as SDL3 names its
// face buttons -- `a` is south on every pad, whatever is printed on it.
enum iPadInput
{
    PADIN_A,
    PADIN_B,
    PADIN_X,
    PADIN_Y,
    PADIN_LB,
    PADIN_RB,
    PADIN_LT,
    PADIN_RT,
    PADIN_LS,
    PADIN_RS,
    PADIN_BACK,
    PADIN_START,
    PADIN_DPUP,
    PADIN_DPDOWN,
    PADIN_DPLEFT,
    PADIN_DPRIGHT,
    PADIN_COUNT
};

// SDL has a scancode for each side of a modifier and none for "either", which
// is what the bare names have always meant. These ids stand for the pair; they
// are above SDL_SCANCODE_COUNT so they can never collide with a real one.
#define KEY_EITHER_SHIFT 1000
#define KEY_EITHER_CTRL 1001
#define KEY_EITHER_ALT 1002

// The mouse buttons and the wheel, which SDL gives no scancode. They are rows of
// the key table all the same, so a binding can name one wherever it names a
// key. Numbered rather than named left and right: SDL swaps the two for a
// left-handed mouse, so `mouse1` is the primary button whichever side it is on.
#define KEY_MOUSE_1 1010
#define KEY_MOUSE_2 1011
#define KEY_MOUSE_3 1012
#define KEY_MOUSE_4 1013
#define KEY_MOUSE_5 1014
#define KEY_WHEEL_UP 1020
#define KEY_WHEEL_DOWN 1021

// Where a trigger clicks, in SDL's 0..32767. See iPadHostSDL.cpp for how the
// number was arrived at.
#define IPAD_SDL_TRIGGER_THRESHOLD 9830

const iPadBindToken* iPadPadTokens(S32* count);

// Ids are SDL scancodes, KEY_EITHER_*, KEY_MOUSE_* or KEY_WHEEL_*. Letters and
// digits are included.
const iPadBindToken* iPadKeyTokens(S32* count);

// The PADIN_* an SDL_GamepadButton reports as, or -1 for a button no binding
// can name. The triggers are axes, not buttons, and are not here.
S32 iPadInputFromSDLButton(S32 button);

#endif
