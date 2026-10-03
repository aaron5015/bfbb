#include "zTF2Bridge.h"

#include "iTF2Bridge.h"
#include "zGameState.h"
#include "zGlobals.h"

#include <math.h>

// Axis mapping between the two worlds. BFBB: x right, y up, z forward
// (left-handed). Source: x forward, y left, z up (right-handed). A physical
// match, with nothing mirrored, is
//
//      Source = ( bfbb.z, -bfbb.x, bfbb.y )
//
// tf2bridge_server.cpp uses the same mapping for positions; the TF2 view
// direction is converted with it below.

static void WalkStickFromView(const BridgeIntentPacket* in)
{
    // Direction the TF2 player is asking to walk, in Source's horizontal plane.
    const float th = in->yaw * (3.14159265f / 180.0f);
    const float f = in->forward;
    const float s = in->side;
    const float srcX = f * cosf(th) + s * sinf(th);
    const float srcY = f * sinf(th) - s * cosf(th);

    // Same direction in BFBB's horizontal plane: Source (x, y) = (bz, -bx).
    const float dx = -srcY; // bfbb x
    const float dz = srcX; // bfbb z

    // The game turns the stick into a heading relative to its own camera, so
    // express the direction in the camera's frame instead of the world's.
    const xMat4x3& cam = globals.camera.mat;
    float atx = cam.at.x, atz = cam.at.z;
    float rx = cam.right.x, rz = cam.right.z;
    const float atLen = sqrtf(atx * atx + atz * atz);
    const float rLen = sqrtf(rx * rx + rz * rz);

    if (atLen < 0.05f || rLen < 0.05f)
    {
        // Camera looking straight up or down: no horizontal frame to use.
        iTF2BridgeSetStick(FALSE, 0.0f, 0.0f);
        return;
    }

    atx /= atLen;
    atz /= atLen;
    rx /= rLen;
    rz /= rLen;

    iTF2BridgeSetStick(TRUE, dx * rx + dz * rz, dx * atx + dz * atz);
}

void zTF2Bridge_Frame()
{
    static bool sInited = false;
    if (!sInited)
    {
        sInited = true;
        iTF2BridgeInit();
    }

    if (!iTF2BridgeActive())
    {
        return;
    }

    iTF2BridgePoll();

    const bool playing = zGameModeGet() == eGameMode_Game;

    const BridgeIntentPacket* in = iTF2BridgeGetIntent();
    if (playing && in != NULL)
    {
        WalkStickFromView(in);
    }
    else
    {
        iTF2BridgeSetStick(FALSE, 0.0f, 0.0f);
    }

    BridgeStatePacket st = {};
    st.flags = 0;

    if (playing && globals.player.ent.frame != NULL)
    {
        const xMat4x3& m = globals.player.ent.frame->mat;
        st.x = m.pos.x;
        st.y = m.pos.y;
        st.z = m.pos.z;
        // BFBB-convention facing: atan2(at.x, at.z), degrees, +z is 0.
        st.yaw = atan2f(m.at.x, m.at.z) * (180.0f / 3.14159265f);
        st.health = (int32_t)globals.player.Health;
        st.sceneId = globals.sceneCur != NULL ? globals.sceneCur->sceneID : 0;
        st.flags |= BRIDGE_STATE_GAMEPLAY;
    }

    iTF2BridgeSendState(&st);
}
