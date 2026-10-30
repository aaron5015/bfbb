// The mouse, off SDL's events. The interface and the argument for it are in
// iPadMouse.h.

#include "iPadMouse.h"

#include "iHost.h"

#include <SDL3/SDL.h>

#include <stdio.h>

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
    S32 sEverSeen;

    S32 sMoved;
    U32 sDown;

    S32 sWatching;
    S32 sCaptured;
    SDL_Window* sCaptureWindow;

    // The menu pointer. Built from res/cursor at a size that follows the
    // window, since a cursor is drawn by the OS at its own pixel size and a
    // 32-pixel arrow disappears on a 4K screen.
    struct CursorImage
    {
        const char* file;
        S32 hotX;
        S32 hotY; // at the file's own size
        SDL_Surface* source;
        SDL_Cursor* cursor;
    };

    CursorImage sCursors[2] = {
        { "arrow.png", 33, 3, NULL, NULL },
        { "hand.png", 52, 4, NULL, NULL },
    };
    S32 sCursorSize;
    S32 sCursorsLoaded;
    S32 sPointer;
    S32 sWantHand;
    S32 sShownHand = -1; // -1: nothing of ours set

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

    void LoadCursorSources()
    {
        if (sCursorsLoaded)
        {
            return;
        }
        sCursorsLoaded = 1;

        char dir[512];
        if (!iHostExeDir(dir, sizeof(dir)))
        {
            return;
        }

        for (S32 i = 0; i < 2; i++)
        {
            char path[640];
            snprintf(path, sizeof(path), "%s/cursor/%s", dir, sCursors[i].file);

            SDL_Surface* loaded = SDL_LoadPNG(path);
            if (loaded == NULL)
            {
                printf("bfbb: no menu pointer at %s (%s); the system's is used\n", path,
                       SDL_GetError());
                fflush(stdout);
                continue;
            }

            // Scaling wants true colour, and the files are paletted.
            sCursors[i].source = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
            SDL_DestroySurface(loaded);
        }
    }

    // About a twentieth of the window's height: 54 pixels at 1080, the size
    // the arrow is drawn at, and never under the OS's own 32.
    S32 CursorSizeFor(SDL_Window* window)
    {
        S32 w = 0;
        S32 h = 0;
        if (window == NULL || !SDL_GetWindowSizeInPixels(window, &w, &h) || h <= 0)
        {
            return 48;
        }
        S32 size = h / 20;
        return size < 32 ? 32 : (size > 128 ? 128 : size);
    }

    void BuildCursors(S32 size)
    {
        for (S32 i = 0; i < 2; i++)
        {
            CursorImage& c = sCursors[i];
            if (c.cursor != NULL)
            {
                SDL_DestroyCursor(c.cursor);
                c.cursor = NULL;
            }
            if (c.source == NULL)
            {
                continue;
            }

            SDL_Surface* scaled = SDL_ScaleSurface(c.source, size, size, SDL_SCALEMODE_LINEAR);
            if (scaled == NULL)
            {
                continue;
            }
            S32 hx = c.hotX * size / c.source->w;
            S32 hy = c.hotY * size / c.source->h;
            c.cursor = SDL_CreateColorCursor(scaled, hx, hy);
            SDL_DestroySurface(scaled);
        }
        sCursorSize = size;
    }

    void ApplyPointer(SDL_Window* focus)
    {
        if (!sPointer)
        {
            // Over the game itself the OS pointer is in the way.
            if (SDL_CursorVisible())
            {
                SDL_HideCursor();
            }
            return;
        }

        LoadCursorSources();
        S32 size = CursorSizeFor(focus);
        if (size != sCursorSize)
        {
            BuildCursors(size);
            sShownHand = -1;
        }

        S32 want = sWantHand ? 1 : 0;
        if (want != sShownHand && sCursors[want].cursor != NULL)
        {
            SDL_SetCursor(sCursors[want].cursor);
            sShownHand = want;
        }
        if (!SDL_CursorVisible())
        {
            SDL_ShowCursor();
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

    // SDL_HasMouse alone is not enough: under D3D9 on Windows it has answered
    // false with a mouse plugged in. A mouse that has moved or clicked is
    // there.
    if (activity)
    {
        sEverSeen = 1;
    }
    sPresent = (sEverSeen || SDL_HasMouse()) ? 1 : 0;

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
        sMoved = 0;
        sDown = 0;
        return;
    }

    sDX += dx;
    sDY += dy;
    sWheel = wheel;
    sHeld = held | down; // a click inside one poll still reads as held for it
    sSaw = activity;
    sMoved = (dx != 0.0f || dy != 0.0f) ? 1 : 0;
    sDown = down;

    ApplyPointer(focus);

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
    sCaptured = (on && !sPointer) ? 1 : 0;
    ApplyCapture(sCaptured ? SDL_GetKeyboardFocus() : NULL);
}

void iPadMouseSetPointer(S32 on, S32 hand)
{
    on = on ? 1 : 0;
    sWantHand = hand ? 1 : 0;

    if (on != sPointer)
    {
        sPointer = on;
        if (on)
        {
            // Let go first: relative mode hides the cursor and pins it.
            iPadMouseSetCapture(0);
        }
    }

    SDL_Window* focus = SDL_GetKeyboardFocus();
    if (focus != NULL)
    {
        ApplyPointer(focus);
    }
}

S32 iPadMousePointerOn()
{
    return sPointer;
}

S32 iPadMousePointerAt(F32* x, F32* y)
{
    SDL_Window* focus = SDL_GetKeyboardFocus();
    if (focus == NULL)
    {
        return FALSE;
    }

    // SDL reports the pointer in window coordinates, which are points and not
    // pixels on a scaled display.
    F32 mx = 0.0f;
    F32 my = 0.0f;
    SDL_GetMouseState(&mx, &my);

    S32 pw = 0;
    S32 ph = 0;
    S32 lw = 0;
    S32 lh = 0;
    if (SDL_GetWindowSizeInPixels(focus, &pw, &ph) && SDL_GetWindowSize(focus, &lw, &lh) &&
        lw > 0 && lh > 0)
    {
        mx *= (F32)pw / (F32)lw;
        my *= (F32)ph / (F32)lh;
    }

    *x = mx;
    *y = my;
    return TRUE;
}

S32 iPadMouseMoved()
{
    return sMoved;
}

S32 iPadMouseButtonPressed(S32 n)
{
    return (n >= 1 && n <= 5 && (sDown & (1u << (n - 1)))) ? 1 : 0;
}

S32 iPadMouseSawInput()
{
    return sSaw;
}

S32 iPadMousePresent()
{
    return sPresent;
}
