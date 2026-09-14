#include "iPadTouch.h"

#include "iCameraLook.h"
#include "iConfig.h"
#include "iPadHost.h"
#include "iTouchOverlay.h"
#include "xPad.h"

#include <SDL3/SDL.h>

#include <math.h>
#include <string.h>

// See iPadTouch.h.

namespace
{
    enum Role
    {
        ROLE_NONE,
        ROLE_STICK,
        ROLE_CAMERA,
        ROLE_BUTTONS,
    };

    struct Finger
    {
        SDL_FingerID id;
        Role role;
        F32 x;
        F32 y;
        bool seen;
    };

    const S32 kMaxFingers = 10;

    Finger sFingers[kMaxFingers];
    S32 sFingerCount;

    // One on-screen button. Placed in units of the window's height, from the
    // corner named by `right` and `bottom`, so the layout keeps its size on
    // any aspect.
    struct Button
    {
        U32 bits;
        char label;
        F32 x;
        F32 y;
        bool right;
        bool bottom;
        F32 halfWidth;
        F32 halfHeight;
        U8 r, g, b;
    };

    // The face four by position, as an Xbox pad has them: A south, B east, X
    // west, Y north. Their bits are the Xbox preset's (iPadBind.cpp); Z and HUD
    // carry L2 and R2 as LB and RB do there.
    const F32 kFaceX = 0.24f;
    const F32 kFaceY = 0.28f;
    const F32 kFaceGap = 0.12f;
    const F32 kFace = 0.07f;

    const Button kButtons[] = {
        { XPAD_BUTTON_X, 'A', kFaceX, kFaceY - kFaceGap, true, true, kFace, kFace, 96, 176, 72 },
        { XPAD_BUTTON_TRIANGLE, 'B', kFaceX - kFaceGap, kFaceY, true, true, kFace, kFace, 208, 72, 64 },
        { XPAD_BUTTON_O, 'X', kFaceX + kFaceGap, kFaceY, true, true, kFace, kFace, 64, 120, 208 },
        { XPAD_BUTTON_SQUARE, 'Y', kFaceX, kFaceY + kFaceGap, true, true, kFace, kFace, 224, 184, 48 },

        { XPAD_BUTTON_L1, 'L', 0.16f, 0.09f, false, false, 0.11f, 0.05f, 160, 160, 160 },
        { XPAD_BUTTON_Z | XPAD_BUTTON_L2, 'Z', 0.13f, 0.21f, false, false, 0.08f, 0.045f, 160, 160, 160 },
        { XPAD_BUTTON_R1, 'R', 0.16f, 0.09f, true, false, 0.11f, 0.05f, 160, 160, 160 },
        { XPAD_BUTTON_HUD | XPAD_BUTTON_R2, 'H', 0.13f, 0.21f, true, false, 0.08f, 0.045f, 160, 160, 160 },

        { XPAD_BUTTON_START, '>', 0.05f, 0.35f, true, false, 0.04f, 0.04f, 160, 160, 160 },
    };

    const S32 kButtonCount = (S32)(sizeof(kButtons) / sizeof(kButtons[0]));

    // The stick, as fractions of the window's height.
    const F32 kStickRadius = 0.13f;
    const F32 kStickKnob = 0.055f;
    const F32 kStickHomeX = 0.24f;
    const F32 kStickHomeY = 0.28f;

    // Of the stick's reach. A resting thumb still wanders a few pixels.
    const F32 kStickDeadzone = 0.12f;

    // Radians the camera turns for a drag the height of the window.
    const F32 kCameraTurnPerHeight = 3.0f;

    // Seconds for the camera to cover most of a drag's movement. Touch samples
    // and frames arrive at different rates, so the distance moved in one frame
    // jitters; the camera takes this share of what is still owed each frame,
    // which evens it out and still turns it exactly as far as the finger went.
    const F32 kCameraSmoothing = 0.04f;

    bool sEnabled;

    // Hidden while a controller is in use, until the screen is touched.
    bool sShowing = true;

    F32 sStickCenterX;
    F32 sStickCenterY;
    F32 sStickX;
    F32 sStickY;
    F32 sCameraDX;
    F32 sCameraDY;

    // Drag movement in pixels not yet handed to the camera.
    F32 sCameraOwedX;
    F32 sCameraOwedY;
    Uint64 sLastPollNS;
    U32 sHeld;

    void ButtonCenter(const Button& b, F32 w, F32 h, F32* cx, F32* cy)
    {
        *cx = b.right ? w - b.x * h : b.x * h;
        *cy = b.bottom ? h - b.y * h : b.y * h;
    }

    // The button under a point, with some slack past its edge: a thumb covers
    // more than it hits.
    S32 ButtonAt(F32 px, F32 py, F32 w, F32 h)
    {
        S32 best = -1;
        F32 bestDist = 0.0f;

        for (S32 i = 0; i < kButtonCount; i++)
        {
            const Button& b = kButtons[i];
            F32 cx;
            F32 cy;
            ButtonCenter(b, w, h, &cx, &cy);

            F32 dx = fabsf(px - cx) / (b.halfWidth * h * 1.3f);
            F32 dy = fabsf(py - cy) / (b.halfHeight * h * 1.3f);
            if (dx <= 1.0f && dy <= 1.0f)
            {
                F32 d = dx * dx + dy * dy;
                if (best < 0 || d < bestDist)
                {
                    best = i;
                    bestDist = d;
                }
            }
        }

        return best;
    }

    Finger* FindFinger(SDL_FingerID id)
    {
        for (S32 i = 0; i < sFingerCount; i++)
        {
            if (sFingers[i].id == id)
            {
                return &sFingers[i];
            }
        }
        return NULL;
    }

    bool RoleTaken(Role role)
    {
        for (S32 i = 0; i < sFingerCount; i++)
        {
            if (sFingers[i].role == role)
            {
                return true;
            }
        }
        return false;
    }

    // What the fingers hold this frame. Returns whether a finger went down.
    bool ReadFingers(F32 w, F32 h)
    {
        for (S32 i = 0; i < sFingerCount; i++)
        {
            sFingers[i].seen = false;
        }

        bool newFinger = false;
        F32 cameraDX = 0.0f;
        F32 cameraDY = 0.0f;

        int deviceCount = 0;
        SDL_TouchID* devices = SDL_GetTouchDevices(&deviceCount);

        for (int d = 0; devices != NULL && d < deviceCount; d++)
        {
            int count = 0;
            SDL_Finger** fingers = SDL_GetTouchFingers(devices[d], &count);

            for (int i = 0; fingers != NULL && i < count; i++)
            {
                F32 px = fingers[i]->x * w;
                F32 py = fingers[i]->y * h;

                Finger* f = FindFinger(fingers[i]->id);
                if (f == NULL)
                {
                    if (sFingerCount == kMaxFingers)
                    {
                        continue;
                    }

                    f = &sFingers[sFingerCount++];
                    f->id = fingers[i]->id;
                    f->role = ROLE_NONE;
                    f->x = px;
                    f->y = py;
                    newFinger = true;

                    // A finger's job is decided where it lands and kept until
                    // it lifts, so a thumb that strays across the middle does
                    // not change what it is doing.
                    if (ButtonAt(px, py, w, h) >= 0)
                    {
                        f->role = ROLE_BUTTONS;
                    }
                    else if (px < w * 0.5f && !RoleTaken(ROLE_STICK))
                    {
                        f->role = ROLE_STICK;

                        // The stick centres where the thumb lands, kept whole
                        // on the screen.
                        F32 r = kStickRadius * h;
                        sStickCenterX = px < r ? r : px;
                        sStickCenterY = py > h - r ? h - r : (py < r ? r : py);
                    }
                    else if (px >= w * 0.5f && !RoleTaken(ROLE_CAMERA))
                    {
                        f->role = ROLE_CAMERA;
                    }
                }

                if (f->role == ROLE_CAMERA)
                {
                    cameraDX = px - f->x;
                    cameraDY = py - f->y;
                }

                f->x = px;
                f->y = py;
                f->seen = true;
            }

            SDL_free(fingers);
        }

        SDL_free(devices);

        // Lifted fingers.
        for (S32 i = 0; i < sFingerCount;)
        {
            if (!sFingers[i].seen)
            {
                sFingers[i] = sFingers[--sFingerCount];
            }
            else
            {
                i++;
            }
        }

        // Buttons: whatever each button finger is over now, so a thumb can
        // roll from one face button to the next.
        sHeld = 0;
        sStickX = 0.0f;
        sStickY = 0.0f;
        bool stickHeld = false;
        bool cameraHeld = false;

        for (S32 i = 0; i < sFingerCount; i++)
        {
            const Finger& f = sFingers[i];

            if (f.role == ROLE_BUTTONS)
            {
                S32 b = ButtonAt(f.x, f.y, w, h);
                if (b >= 0)
                {
                    sHeld |= kButtons[b].bits;
                }
            }
            else if (f.role == ROLE_STICK)
            {
                F32 r = kStickRadius * h;
                F32 dx = (f.x - sStickCenterX) / r;
                F32 dy = (f.y - sStickCenterY) / r;
                F32 len = sqrtf(dx * dx + dy * dy);
                if (len > 1.0f)
                {
                    dx /= len;
                    dy /= len;
                }
                else if (len < kStickDeadzone)
                {
                    dx = 0.0f;
                    dy = 0.0f;
                }

                // Up-positive, as iPadHost.h asks.
                sStickX = dx;
                sStickY = -dy;
                stickHeld = true;
            }
            else if (f.role == ROLE_CAMERA)
            {
                cameraHeld = true;
            }
        }

        if (!stickHeld)
        {
            sStickCenterX = kStickHomeX * h;
            sStickCenterY = h - kStickHomeY * h;
        }

        sCameraDX = cameraHeld ? cameraDX : 0.0f;
        sCameraDY = cameraHeld ? cameraDY : 0.0f;

        return newFinger;
    }

    void Submit(F32 w, F32 h)
    {
        iTouchOverlayShape shapes[ITOUCHOVERLAY_MAX_SHAPES];
        S32 n = 0;

        if (sShowing)
        {
            // The stick: a ring for its reach and a knob where it points.
            {
                iTouchOverlayShape& ring = shapes[n++];
                memset(&ring, 0, sizeof(ring));
                ring.x = sStickCenterX;
                ring.y = sStickCenterY;
                ring.halfWidth = ring.halfHeight = ring.corner = kStickRadius * h;
                ring.stroke = 0.008f * h;
                ring.r = ring.g = ring.b = 255;
                ring.a = 110;

                iTouchOverlayShape& knob = shapes[n++];
                memset(&knob, 0, sizeof(knob));
                knob.x = sStickCenterX + sStickX * kStickRadius * h;
                knob.y = sStickCenterY - sStickY * kStickRadius * h;
                knob.halfWidth = knob.halfHeight = knob.corner = kStickKnob * h;
                knob.r = knob.g = knob.b = 255;
                knob.a = (sStickX != 0.0f || sStickY != 0.0f) ? 150 : 90;
            }

            for (S32 i = 0; i < kButtonCount; i++)
            {
                const Button& b = kButtons[i];
                bool held = (sHeld & b.bits) == b.bits;

                iTouchOverlayShape& s = shapes[n++];
                memset(&s, 0, sizeof(s));
                ButtonCenter(b, w, h, &s.x, &s.y);
                s.halfWidth = b.halfWidth * h;
                s.halfHeight = b.halfHeight * h;
                s.corner = b.halfWidth == b.halfHeight ? s.halfWidth : s.halfHeight * 0.5f;
                s.r = b.r;
                s.g = b.g;
                s.b = b.b;
                s.a = held ? 200 : 90;
                s.label = b.label;
            }
        }

        iTouchOverlaySubmit(shapes, n);
    }
} // namespace

void iPadTouchInit()
{
    const char* mode = iConfigGetString("input.touch_controls", "auto");

    if (strcmp(mode, "on") == 0)
    {
        sEnabled = true;
    }
    else if (strcmp(mode, "off") == 0)
    {
        sEnabled = false;
    }
    else
    {
#ifdef __ANDROID__
        sEnabled = true;
#else
        sEnabled = false;
#endif
    }
}

void iPadTouchPoll(iPadHostState* s, bool padOnPort0)
{
    if (!sEnabled)
    {
        return;
    }

    S32 winW = 0;
    S32 winH = 0;
    if (!iTouchOverlayWindowSize(&winW, &winH))
    {
        // Nothing has been presented yet, so there is no size to lay out in.
        return;
    }

    F32 w = (F32)winW;
    F32 h = (F32)winH;

    bool touched = ReadFingers(w, h);

    if (touched || !padOnPort0)
    {
        sShowing = true;
    }
    else
    {
        // A controller in use takes the screen back.
        if (s->buttons != 0 || s->stick_x != 0.0f || s->stick_y != 0.0f ||
            s->substick_x != 0.0f || s->substick_y != 0.0f)
        {
            sShowing = false;
        }
    }

    Submit(w, h);

    // The camera turns with the finger while the controls are showing, and the
    // retail camera, springs and all, comes back with a controller.
    iCameraLookSetActive(sShowing);

    if (!sShowing)
    {
        sCameraOwedX = 0.0f;
        sCameraOwedY = 0.0f;
        return;
    }

    Uint64 now = SDL_GetTicksNS();
    F32 dt = sLastPollNS != 0 ? (F32)(now - sLastPollNS) / 1e9f : 0.0f;
    sLastPollNS = now;
    if (dt > 0.1f)
    {
        dt = 0.1f;
    }

    sCameraOwedX += sCameraDX;
    sCameraOwedY += sCameraDY;

    F32 share = 1.0f - expf(-dt / kCameraSmoothing);
    F32 turnX = sCameraOwedX * share;
    F32 turnY = sCameraOwedY * share;
    sCameraOwedX -= turnX;
    sCameraOwedY -= turnY;

    // As a mouse moves it: a drag right turns the view right, a drag down looks
    // down. Yaw runs against the way the C-stick pushes pgoal, and the engine's
    // pitch is positive-down.
    F32 perPixel = kCameraTurnPerHeight / h;
    iCameraLookSet(-turnX * perPixel, turnY * perPixel);

    s->connected = true;
    s->buttons |= sHeld;

    if (sStickX * sStickX + sStickY * sStickY > s->stick_x * s->stick_x + s->stick_y * s->stick_y)
    {
        s->stick_x = sStickX;
        s->stick_y = sStickY;
    }
}
