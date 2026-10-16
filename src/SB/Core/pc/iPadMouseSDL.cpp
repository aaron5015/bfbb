// The mouse, off SDL's events. The interface and the argument for it are in
// iPadMouse.h.

#include "iPadMouse.h"

#include <SDL3/SDL.h>

namespace
{
    // Written by the watch, which runs on whichever thread posted the event --
    // on Windows that can be SDL's raw input thread -- and read by the poll on
    // the game's. Everything the two share is under this lock.
    SDL_SpinLock sLock;
    F32 sPendingDX;
    F32 sPendingDY;
    S32 sPendingWheel;
    U32 sPendingDown; // presses since the last poll, a bit per button
    U32 sHeldMask; // as of the last event, a bit per button
    S32 sPendingActivity;

    // The poll's copies, which every reader in one frame sees the same answer
    // from.
    F32 sDX;
    F32 sDY;
    S32 sWheel;
    U32 sHeld;
    S32 sPressed;
    S32 sSaw;
    S32 sPresent;

    S32 sWatching;
    S32 sCaptured;
    SDL_Window* sCaptureWindow;

    // Mouse events SDL makes up from a finger or a pen. A touchscreen is
    // iPadTouch.cpp's to read, and a drag across its on-screen stick must not
    // also turn the camera or claim the device for the keyboard.
    bool IsRealMouse(SDL_MouseID which)
    {
        return which != SDL_TOUCH_MOUSEID && which != SDL_PEN_MOUSEID;
    }

    // SDL numbers the middle button 2 and the right 3. A binding's `mouse2` is
    // the right button and `mouse3` the middle, as everywhere else on PC.
    U32 ButtonBit(Uint8 button)
    {
        switch (button)
        {
        case SDL_BUTTON_LEFT:
            return 1u << 0;
        case SDL_BUTTON_RIGHT:
            return 1u << 1;
        case SDL_BUTTON_MIDDLE:
            return 1u << 2;
        case SDL_BUTTON_X1:
            return 1u << 3;
        case SDL_BUTTON_X2:
            return 1u << 4;
        default:
            return 0;
        }
    }

    bool SDLCALL Watch(void*, SDL_Event* e)
    {
        switch (e->type)
        {
        case SDL_EVENT_MOUSE_MOTION:
            if (IsRealMouse(e->motion.which))
            {
                SDL_LockSpinlock(&sLock);
                sPendingDX += e->motion.xrel;
                sPendingDY += e->motion.yrel;
                if (e->motion.xrel != 0.0f || e->motion.yrel != 0.0f)
                {
                    sPendingActivity = 1;
                }
                SDL_UnlockSpinlock(&sLock);
            }
            break;

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (IsRealMouse(e->button.which))
            {
                U32 bit = ButtonBit(e->button.button);
                SDL_LockSpinlock(&sLock);
                if (e->button.down)
                {
                    sHeldMask |= bit;
                    sPendingDown |= bit;
                }
                else
                {
                    sHeldMask &= ~bit;
                }
                sPendingActivity = 1;
                SDL_UnlockSpinlock(&sLock);
            }
            break;

        case SDL_EVENT_MOUSE_WHEEL:
            if (IsRealMouse(e->wheel.which))
            {
                // integer_y is whole notches, with a high-resolution wheel's
                // fractions carried by SDL. FLIPPED is the platform's natural
                // scrolling, undone so that up is up on the wheel itself.
                S32 notches = e->wheel.integer_y;
                if (e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
                {
                    notches = -notches;
                }
                SDL_LockSpinlock(&sLock);
                sPendingWheel += notches;
                if (notches != 0)
                {
                    sPendingActivity = 1;
                }
                SDL_UnlockSpinlock(&sLock);
            }
            break;

        default:
            break;
        }

        return true;
    }

    void ApplyCapture(SDL_Window* want)
    {
        if (want == sCaptureWindow)
        {
            return;
        }

        if (sCaptureWindow != NULL)
        {
            SDL_SetWindowRelativeMouseMode(sCaptureWindow, false);
        }

        sCaptureWindow = want;

        if (sCaptureWindow != NULL)
        {
            SDL_SetWindowRelativeMouseMode(sCaptureWindow, true);
        }
    }
} // namespace

void iPadMouseInit()
{
    if (!sWatching)
    {
        SDL_AddEventWatch(Watch, NULL);
        sWatching = 1;
    }
}

void iPadMouseExit()
{
    iPadMouseSetCapture(0);

    if (sWatching)
    {
        SDL_RemoveEventWatch(Watch, NULL);
        sWatching = 0;
    }
}

void iPadMousePoll()
{
    SDL_LockSpinlock(&sLock);
    F32 dx = sPendingDX;
    F32 dy = sPendingDY;
    S32 wheel = sPendingWheel;
    U32 down = sPendingDown;
    U32 held = sHeldMask;
    S32 activity = sPendingActivity;
    sPendingDX = 0.0f;
    sPendingDY = 0.0f;
    sPendingWheel = 0;
    sPendingDown = 0;
    sPendingActivity = 0;
    SDL_UnlockSpinlock(&sLock);

    sPresent = SDL_HasMouse() ? 1 : 0;

    SDL_Window* focus = SDL_GetKeyboardFocus();
    if (focus == NULL)
    {
        // Thrown away rather than kept: a player who alt-tabs, moves the mouse
        // across the desktop and comes back should find the camera where they
        // left it. A button let go in another window never reaches this one, so
        // the held state is dropped as well.
        SDL_LockSpinlock(&sLock);
        sHeldMask = 0;
        SDL_UnlockSpinlock(&sLock);

        sDX = 0.0f;
        sDY = 0.0f;
        sWheel = 0;
        sHeld = 0;
        sPressed = 0;
        sSaw = 0;
        return;
    }

    sDX += dx;
    sDY += dy;
    sWheel = wheel;
    sHeld = held | down; // a click inside one poll still reads as held for it
    sSaw = activity;

    sPressed = 0;
    for (S32 n = 1; n <= 5; n++)
    {
        if (down & (1u << (n - 1)))
        {
            sPressed = n;
            break;
        }
    }

    // Focus moved to another window of ours, or capture was asked for before
    // there was a window to apply it to.
    if (sCaptured)
    {
        ApplyCapture(focus);
    }
}

void iPadMouseTake(F32* dx, F32* dy)
{
    *dx = sDX;
    *dy = sDY;
    sDX = 0.0f;
    sDY = 0.0f;
}

S32 iPadMouseButton(S32 n)
{
    return (n >= 1 && n <= 5 && (sHeld & (1u << (n - 1)))) ? 1 : 0;
}

S32 iPadMouseWheel()
{
    return sWheel;
}

S32 iPadMousePressed()
{
    return sPressed;
}

void iPadMouseSetCapture(S32 on)
{
    sCaptured = on ? 1 : 0;
    ApplyCapture(sCaptured ? SDL_GetKeyboardFocus() : NULL);
}

S32 iPadMouseSawInput()
{
    return sSaw;
}

S32 iPadMousePresent()
{
    return sPresent;
}
