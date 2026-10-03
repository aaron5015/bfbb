#include "zTF2Bridge.h"

#include "iTF2Bridge.h"
#include "zGameState.h"
#include "zGlobals.h"

#include <math.h>

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

    BridgeStatePacket st = {};
    st.flags = 0;

    if (zGameModeGet() == eGameMode_Game && globals.player.ent.frame != NULL)
    {
        const xMat4x3& m = globals.player.ent.frame->mat;
        st.x = m.pos.x;
        st.y = m.pos.y;
        st.z = m.pos.z;
        // Facing: the matrix's `at` axis, flattened. atan2(x, z) so that +z is 0.
        st.yaw = atan2f(m.at.x, m.at.z) * (180.0f / 3.14159265f);
        st.health = (int32_t)globals.player.Health;
        st.sceneId = globals.sceneCur != NULL ? globals.sceneCur->sceneID : 0;
        st.flags |= BRIDGE_STATE_GAMEPLAY;
    }

    iTF2BridgeSendState(&st);
}
