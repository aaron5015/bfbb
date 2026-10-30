// See iFirstPerson.h.

#include "iFirstPerson.h"

#include "iCamera.h"
#include "iCameraLook.h"
#include "iPadBind.h"

#include "xCamera.h"
#include "xMath.h"
#include "xMath3.h"
#include "zEntPlayer.h"
#include "zGlobals.h"

namespace
{
    S32 sOn;

    // The view, in the engine's convention: yaw as xatan2(at.x, at.z), pitch
    // positive looking down.
    F32 sYaw;
    F32 sPitch;

    // Where this placed the camera last, to tell whether anything has moved it
    // since.
    xVec3 sPlaced;
    S32 sPlacedValid;

    // What iFirstPersonHideCarried hid, for iFirstPersonShowCarried.
    xEnt* sHidCarried;

    // The walk bob: its phase, in radians, and how much of it shows, 0..1.
    F32 sBobPhase;
    F32 sBobWeight;

    // His position last frame and his smoothed speed, for OwnSpeed.
    xVec3 sBobLast;
    S32 sBobLastValid;
    F32 sBobSpeed;

    // Above the feet, per character, in eCurrentPlayer order. Each skeleton's
    // highest joint standing still is SpongeBob 0.86, Patrick 1.44 at the
    // point of his head, Sandy 1.20 inside the helmet.
    const F32 kEyeHeight[eCurrentPlayerCount] = { 0.9f, 1.25f, 1.2f };

    // One rise and fall per step: the ground covered per step, and the height
    // of the rise at a full run. Untuned.
    const F32 kBobStride = 1.2f;
    const F32 kBobHeight = 0.06f;

    // How fast the bob fades in when he starts moving and out when he stops,
    // per second.
    const F32 kBobFade = 6.0f;

    // How fast the smoothed speed follows the measured one, per second.
    const F32 kBobSmooth = 12.0f;

    // The right stick, at full deflection, in radians per second. Untuned.
    const F32 kStickYawRate = 3.0f;
    const F32 kStickPitchRate = 2.0f;

    // Short of straight up and down, where yaw stops meaning anything.
    const F32 kPitchLimit = 1.3962634f; // 80 degrees

    // The stick past retail's deadzone and clamp, 32 and 110, as -1..1.
    F32 Stick(S8 v)
    {
        F32 a = (F32)(v < 0 ? -v : v);
        if (a <= 32.0f)
        {
            return 0.0f;
        }
        F32 n = (a > 110.0f ? 110.0f : a) - 32.0f;
        n /= 110.0f - 32.0f;
        return v < 0 ? -n : n;
    }

    void Eye(const xCamera* cam, xVec3* out)
    {
        S32 who = gCurrentPlayer;
        if (who < 0 || who >= eCurrentPlayerCount)
        {
            who = eCurrentPlayerSpongeBob;
        }

        *out = cam->tgt_mat->pos;
        out->y += kEyeHeight[who];
    }

    // His ground speed on his own feet, per second: his change in position
    // since last frame less what the platform under him carried him, smoothed.
    // A jump past four times his run, a respawn or a warp, counts as standing.
    F32 OwnSpeed(F32 dt)
    {
        const xEntFrame* frame = globals.player.ent.frame;
        if (frame == NULL || dt <= 0.0f)
        {
            sBobLastValid = 0;
            return 0.0f;
        }

        F32 raw = 0.0f;
        if (sBobLastValid)
        {
            xVec3 moved = frame->mat.pos - sBobLast;

            // xEntDriveUpdate leaves dloc as it was once nothing drives him.
            const xEntDrive& drv = globals.player.drv;
            if (drv.s || drv.os)
            {
                moved -= drv.dloc;
            }

            raw = xsqrt(moved.x * moved.x + moved.z * moved.z) / dt;
            if (raw > 4.0f * globals.player.s->MoveSpeed[2])
            {
                raw = 0.0f;
            }
        }
        sBobLast = frame->mat.pos;
        sBobLastValid = 1;

        F32 k = kBobSmooth * dt;
        sBobSpeed += (raw - sBobSpeed) * (k > 1.0f ? 1.0f : k);
        return sBobSpeed;
    }

    // The bob's offset above the eye this frame.
    F32 Bob(F32 dt)
    {
        F32 run = 0.0f;
        F32 speed = OwnSpeed(dt);
        S32 grounded = globals.player.JumpState == 0 || globals.player.JumpState == 1;
        if (grounded)
        {
            F32 full = globals.player.s->MoveSpeed[2];
            run = full > 0.0f ? speed / full : 0.0f;
            run = run > 1.0f ? 1.0f : run;
            sBobPhase += speed * dt * (PI / kBobStride);
            if (sBobPhase > 2.0f * PI)
            {
                sBobPhase -= 2.0f * PI;
            }
        }

        F32 step = kBobFade * dt;
        if (sBobWeight < run)
        {
            sBobWeight = (sBobWeight + step > run) ? run : sBobWeight + step;
        }
        else
        {
            sBobWeight = (sBobWeight - step < run) ? run : sBobWeight - step;
        }

        // The phase makes a half turn per stride, and |sin| rises and falls
        // once per half turn: once per step.
        F32 s = isin(sBobPhase);
        return kBobHeight * sBobWeight * (s < 0.0f ? -s : s);
    }

    // Leaves the camera behind and above where he was looking, at retail's
    // framing, with retail's cylinder goals agreeing. xCameraUpdate derives
    // its angle from the position, but this frame's pgoal was copied from pcur
    // before that, so both have to be set or the camera swings back to the
    // angle it had before first person.
    void Leave(xCamera* cam, F32 d, F32 h)
    {
        const xVec3& tgt = cam->tgt_mat->pos;
        F32 behind = xDangleClamp(sYaw + PI);

        cam->mat.pos.x = tgt.x + d * isin(behind);
        cam->mat.pos.y = tgt.y + h;
        cam->mat.pos.z = tgt.z + d * icos(behind);
        cam->omat.pos = cam->mat.pos;

        cam->pcur = behind;
        cam->pgoal = behind;
        cam->dcur = d;
        cam->dgoal = d;
        cam->hcur = h;
        cam->hgoal = h;

        cam->yaw_cur = sYaw;
        cam->yaw_goal = sYaw;
    }
} // namespace

S32 iFirstPersonCamera(xCamera* cam, F32 dt, F32 d, F32 h)
{
    sPlacedValid = 0;

    if (cam->tgt_mat == NULL)
    {
        return 0;
    }

    if (globals.pad0->pressed & XPAD_BUTTON_VIEW)
    {
        sOn = !sOn;

        if (sOn)
        {
            sYaw = xatan2(cam->mat.at.x, cam->mat.at.z);
            sPitch = 0.0f;
            sBobPhase = 0.0f;
            sBobWeight = 0.0f;
            sBobLastValid = 0;
            sBobSpeed = 0.0f;
        }
        else
        {
            Leave(cam, d, h);
            return 0;
        }
    }

    if (!sOn)
    {
        return 0;
    }

    F32 yaw = 0.0f;
    F32 pitch = 0.0f;
    if (iCameraLookActive())
    {
        yaw = iCameraLookYaw();
        pitch = iCameraLookPitch();
    }

    // Signs as zCameraUpdate's stick arms: right turns the way pgoal grows,
    // and up on the stick looks up.
    yaw += kStickYawRate * dt * Stick(globals.pad0->analog2.x);
    pitch -= kStickPitchRate * dt * Stick(globals.pad0->analog2.y);

    sYaw = xDangleClamp(sYaw + yaw);
    sPitch = CLAMP(sPitch + pitch, -kPitchLimit, kPitchLimit);

    xVec3 eye;
    Eye(cam, &eye);
    eye.y += Bob(dt);

    xMat3x3Euler((xMat3x3*)&cam->mat, sYaw, sPitch, 0.0f);
    cam->mat.pos = eye;
    cam->omat = cam->mat;

    cam->yaw_cur = sYaw;
    cam->yaw_goal = sYaw;
    cam->pitch_cur = sPitch;
    cam->pitch_goal = sPitch;
    cam->roll_cur = 0.0f;
    cam->roll_goal = 0.0f;

    iCameraUpdatePos(cam->lo_cam, &cam->mat);

    sPlaced = cam->mat.pos;
    sPlacedValid = 1;
    return 1;
}

S32 iFirstPersonHidesPlayer()
{
    if (!sOn || !sPlacedValid)
    {
        return 0;
    }

    const xVec3& pos = globals.camera.mat.pos;
    return pos.x == sPlaced.x && pos.y == sPlaced.y && pos.z == sPlaced.z;
}

void iFirstPersonHideCarried()
{
    sHidCarried = NULL;

    xEnt* held = globals.player.carry.grabbed;
    if (held == NULL || !(held->flags & 0x1) || !iFirstPersonHidesPlayer())
    {
        return;
    }

    xEntHide(held);
    sHidCarried = held;
}

void iFirstPersonShowCarried()
{
    if (sHidCarried != NULL)
    {
        xEntShow(sHidCarried);
        sHidCarried = NULL;
    }
}
