#ifndef IFIRSTPERSON_H
#define IFIRSTPERSON_H

#include <types.h>

struct xCamera;

// PC-only: a camera at the player's eyes, toggled by the `view` binding
// (XPAD_BUTTON_VIEW). Gameplay only -- zCameraUpdate asks after cutscenes,
// dialogue, fly-throughs and every stop_track owner have had the camera, so
// those keep their own and first person comes back when they let go.
//
// The mouse and touch turn it through iCameraLook.h, the right stick at a
// rate. Movement needs nothing: the player already runs relative to the
// camera's right vector.

// Places the camera and returns 1, or returns 0 and leaves it to retail. `d`
// and `h` are retail's distance and height goals this frame; leaving first
// person puts the camera there, behind where the player was looking, so the
// third-person camera picks up facing the same way.
S32 iFirstPersonCamera(xCamera* cam, F32 dt, F32 d, F32 h);

// Whether the camera is in the player's head this frame, so the body is not
// drawn. False whenever anything else placed the camera.
S32 iFirstPersonHidesPlayer();

// Hides what Patrick holds over his head for the scene's model passes while
// the camera is in his head, and shows it again after. The eye sits inside it
// otherwise. Render-time only: game logic never sees it hidden.
void iFirstPersonHideCarried();
void iFirstPersonShowCarried();

#endif
