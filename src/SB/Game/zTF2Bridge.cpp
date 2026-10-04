#include "zTF2Bridge.h"

#include "iTF2Bridge.h"
#include "iCamera.h"
#include "iEnv.h"
#include "xClumpColl.h"
#include "xBound.h"
#include "xEnt.h"
#include "xEnv.h"
#include "xJSP.h"
#include "zCamera.h"
#include "zGameState.h"
#include "zGlobals.h"
#include "zScene.h"
#include "zNPCMgr.h"
#include "zNPCTypeCommon.h"
#include "xBound.h"
#include "xScene.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

// Axis mapping between the two worlds. librw flips X when it builds the view
// matrix, so BFBB is right-handed with +X toward screen-LEFT, +Y up, +Z forward.
// Source is x forward, y left, z up, and the two line up with no mirroring:
//
//      Source = ( bfbb.z, bfbb.x, bfbb.y ) * scale
//      bfbb   = ( source.y, source.z, source.x ) / scale
//
// Yaw is the same number in both. tf2bridge_server.cpp and bfbb_collision.cpp
// use the same mapping. (The first version of the bridge mirrored X; that was
// the mirrored level seen in TF2.)

static const float kDegToRad = 3.14159265f / 180.0f;

// Forward declaration: the bridge frame runs before the implementation below.
static void TF2Bridge_FireAtNPCs(const BridgeIntentPacket* in);

static xVec3 FromSource(float sx, float sy, float sz, float scale)
{
    xVec3 v;
    v.x = sy / scale;
    v.y = sz / scale;
    v.z = sx / scale;
    return v;
}

// Whether BFBB itself has the player right now: a cutscene, a flythrough, the
// grab-and-respawn after falling out of bounds, a warp. TF2 follows BFBB then.
static bool BfbbOwnsPlayerNow()
{
    return globals.player.ControlOff != 0 || zcam_fly != 0 || zcam_cutscene != 0;
}

// The newest TF2 intent, but only if BFBB should be doing what it says right
// now: in a level, TF2 running the movement, and BFBB not mid-cutscene.
static const BridgeIntentPacket* PuppetIntent()
{
    if (!iTF2BridgeActive() || zGameModeGet() != eGameMode_Game || BfbbOwnsPlayerNow())
    {
        return NULL;
    }

    const BridgeIntentPacket* in = iTF2BridgeGetIntent();
    if (in == NULL || !(in->flags & BRIDGE_INTENT_OWNS_MOVE) || in->scale <= 0.0f)
    {
        return NULL;
    }
    return in;
}

// Fallback (TF2 not owning movement): turn the TF2 view direction and movement
// keys into the stick deflection that walks SpongeBob that way.
static void WalkStickFromView(const BridgeIntentPacket* in)
{
    const float th = in->yaw * kDegToRad;
    const float f = in->forward;
    const float s = in->side;

    // Wished direction in Source's horizontal plane (+side is the player's right).
    const float srcX = f * cosf(th) + s * sinf(th);
    const float srcY = f * sinf(th) - s * cosf(th);

    // The same direction in BFBB's horizontal plane: bfbb (x, z) = Source (y, x).
    const float dx = srcY;
    const float dz = srcX;

    // The game turns the stick into a heading relative to its own camera, so
    // express the direction in the camera's frame instead of the world's.
    const xMat4x3& cam = globals.camera.mat;
    float atx = cam.at.x, atz = cam.at.z;
    float rx = cam.right.x, rz = cam.right.z;
    const float atLen = sqrtf(atx * atx + atz * atz);
    const float rLen = sqrtf(rx * rx + rz * rz);

    if (atLen < 0.05f || rLen < 0.05f)
    {
        iTF2BridgeSetStick(FALSE, 0.0f, 0.0f);
        return;
    }

    atx /= atLen;
    atz /= atLen;
    rx /= rLen;
    rz /= rLen;

    // The camera's `right` vector points to screen-LEFT (see above), so a stick
    // push to the right is the negative of the dot with it.
    iTF2BridgeSetStick(TRUE, -(dx * rx + dz * rz), dx * atx + dz * atz);
}

// ---------------------------------------------------------------------------
// Level collision export.
//
// Writes the current scene's static collision -- the triangles the game's own
// player collision tests against -- to a file the TF2 side can load, so TF2
// can move the merc through the real level. Once per scene, the first frame
// the scene is in play.
//
//   file:   <BFBB_TF2BRIDGE_DIR or .>/bfbb_collision_<sceneId as 8 hex>.bfcl
//   layout (little-endian):
//     char     magic[4]   "BFCL"
//     uint32   version    1
//     uint32   sceneId
//     uint32   triCount
//     float    bounds[6]  min xyz, max xyz   (BFBB units, BFBB axes)
//     triCount x { float v[9]; uint8 flags; uint8 platData; uint16 matIndex; }
//
// Triangles are written with a consistent winding (the game's 0x2 flip applied).
// Only triangles the player collides with (flags & 0x04, the game's own default
// filter) are written. Coordinates are raw BFBB units, Y up; the consumer maps
// them (see zTF2Bridge_Frame / tf2bridge_server.cpp). Static world only:
// platforms, destructibles and enemies are separate and come later.

#pragma pack(push, 1)
struct BridgeCollTri
{
    float v[9];
    uint8_t flags;
    uint8_t platData;
    uint16_t matIndex;
};
#pragma pack(pop)

static void DumpSceneCollision(uint32_t sceneId)
{
    if (globals.sceneCur == NULL || globals.sceneCur->env == NULL ||
        globals.sceneCur->env->geom == NULL || globals.sceneCur->env->geom->jsp == NULL ||
        globals.sceneCur->env->geom->jsp->colltree == NULL)
    {
        return;
    }

    const xClumpCollBSPTree* tree = globals.sceneCur->env->geom->jsp->colltree;

    char path[512];
    const char* dir = getenv("BFBB_TF2BRIDGE_DIR");
    snprintf(path, sizeof(path), "%s/bfbb_collision_%08x.bfcl", dir != NULL ? dir : ".",
             (unsigned)sceneId);

    FILE* f = fopen(path, "wb");
    if (f == NULL)
    {
        printf("bfbb: tf2bridge -- cannot write %s\n", path);
        return;
    }

    // Pass 1: count, and find the bounds.
    uint32_t count = 0;
    float mn[3] = { 1e30f, 1e30f, 1e30f };
    float mx[3] = { -1e30f, -1e30f, -1e30f };
    for (uint32_t i = 0; i < tree->numTriangles; i++)
    {
        const xClumpCollBSPTriangle& t = tree->triangles[i];
        if (!(t.flags & 0x04) || t.v.p == NULL)
        {
            continue;
        }
        count++;
        for (int k = 0; k < 3; k++)
        {
            const float c[3] = { t.v.p[k].x, t.v.p[k].y, t.v.p[k].z };
            for (int a = 0; a < 3; a++)
            {
                if (c[a] < mn[a]) mn[a] = c[a];
                if (c[a] > mx[a]) mx[a] = c[a];
            }
        }
    }

    const uint32_t version = 1;
    fwrite("BFCL", 1, 4, f);
    fwrite(&version, 4, 1, f);
    fwrite(&sceneId, 4, 1, f);
    fwrite(&count, 4, 1, f);
    fwrite(mn, 4, 3, f);
    fwrite(mx, 4, 3, f);

    // Pass 2: the triangles.
    for (uint32_t i = 0; i < tree->numTriangles; i++)
    {
        const xClumpCollBSPTriangle& t = tree->triangles[i];
        if (!(t.flags & 0x04) || t.v.p == NULL)
        {
            continue;
        }

        const RwV3d* a = &t.v.p[0];
        const RwV3d* b = &t.v.p[1];
        const RwV3d* c = &t.v.p[2];
        if (t.flags & 0x2)
        {
            const RwV3d* tmp = b;
            b = c;
            c = tmp;
        }

        BridgeCollTri out;
        out.v[0] = a->x; out.v[1] = a->y; out.v[2] = a->z;
        out.v[3] = b->x; out.v[4] = b->y; out.v[5] = b->z;
        out.v[6] = c->x; out.v[7] = c->y; out.v[8] = c->z;
        out.flags = t.flags;
        out.platData = t.platData;
        out.matIndex = t.matIndex;
        fwrite(&out, sizeof(out), 1, f);
    }

    fclose(f);
    printf("bfbb: tf2bridge -- wrote %u collision triangles to %s\n", (unsigned)count, path);
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

    // The first combat test uses TF2's existing IN_ATTACK bit. Treat the
    // transition from released -> pressed as one BFBB shot; weapon-specific
    // fire cadence will move to the TF2 weapon code once the basic hit path is
    // proven.
    static uint32_t sLastIntentSeq = 0;
    static bool sLastAttack = false;
    const BridgeIntentPacket* attackIn = iTF2BridgeGetIntent();
    if (playing && attackIn != NULL && attackIn->seq != sLastIntentSeq)
    {
        const bool attack = (attackIn->buttons & BRIDGE_IN_ATTACK) != 0;
        if (attack && !sLastAttack)
        {
            TF2Bridge_FireAtNPCs(attackIn);
        }
        sLastAttack = attack;
        sLastIntentSeq = attackIn->seq;
    }

    const BridgeIntentPacket* in = iTF2BridgeGetIntent();
    if (playing && in != NULL && !(in->flags & BRIDGE_INTENT_OWNS_MOVE))
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
        // Facing: atan2(at.x, at.z), degrees, 0 = BFBB +Z. Same number as TF2's yaw.
        st.yaw = atan2f(m.at.x, m.at.z) * (180.0f / 3.14159265f);
        st.health = (int32_t)globals.player.Health;
        st.sceneId = globals.sceneCur != NULL ? globals.sceneCur->sceneID : 0;
        st.flags |= BRIDGE_STATE_GAMEPLAY;
        if (BfbbOwnsPlayerNow())
        {
            st.flags |= BRIDGE_STATE_CONTROL_OFF;
        }

        // Once per scene, on the first frame it is in play.
        static uint32_t sDumpedScene = 0xFFFFFFFFu;
        if (st.sceneId != sDumpedScene)
        {
            sDumpedScene = st.sceneId;
            DumpSceneCollision(st.sceneId);
        }
    }

    iTF2BridgeSendState(&st);
}

// ---------------------------------------------------------------------------
// TF2 weapon bridge (first combat milestone).
//
// TF2 already sends the user's IN_ATTACK bit with BridgeIntentPacket, so we can
// keep the first weapon test deliberately small: on the attack edge, trace the
// same aim ray through BFBB's live NPC bounds and hand the hit to BFBB's normal
// NPC damage system. This keeps health, hurt/death goals, rewards and scripts
// on the BFBB side instead of inventing a second health system in TF2.
//
// The first pass uses DMGTYP_SIDE as a generic robot hit. The damage amount is
// therefore still BFBB's normal one-hit/one-damage progression; crits,
// projectiles, knockback and weapon-specific damage will be layered on later.
static bool IsTF2BridgeRobot(const zNPCCommon* npc)
{
    if (npc == NULL)
        return false;

    switch (npc->SelfType())
    {
    case NPC_TYPE_FODDER:
    case NPC_TYPE_FODDERTOUGH:
    case NPC_TYPE_FODBOMB:
    case NPC_TYPE_CHOMPER:
    case NPC_TYPE_FODBZZT:
    case NPC_TYPE_HAMMER:
    case NPC_TYPE_HAMSPIN:
    case NPC_TYPE_TARTAR:
    case NPC_TYPE_GLOVE:
    case NPC_TYPE_MONSOON:
    case NPC_TYPE_SLEEPY:
    case NPC_TYPE_ARFDOG:
    case NPC_TYPE_ARFARF:
    case NPC_TYPE_CHUCK:
    case NPC_TYPE_TUBELET:
    case NPC_TYPE_TUBESLAVE:
    case NPC_TYPE_SLICK:
    case NPC_TYPE_SLICK_TOUHOU:
    case NPC_TYPE_DUPLOTRON:
        return true;
    default:
        return false;
    }
}

static void TF2Bridge_FireAtNPCs(const BridgeIntentPacket* in)
{
    if (in == NULL || !(in->buttons & BRIDGE_IN_ATTACK) || globals.sceneCur == NULL)
        return;

    // Use the TF2 eye transform that is already driving BFBB's camera. This
    // means the bullet goes exactly where the player is looking in TF2.
    const xVec3 origin = FromSource(in->ex, in->ey, in->ez, in->scale);
    const float yaw = in->yaw * kDegToRad;
    const float pitch = in->pitch * kDegToRad;
    const float cp = cosf(pitch);
    xVec3 dir;
    dir.x = cp * sinf(yaw);
    dir.y = -sinf(pitch);
    dir.z = cp * cosf(yaw);

    xRay3 ray;
    ray.origin = origin;
    ray.dir = dir;
    ray.min_t = 0.0f;
    // Melee is deliberately short-range; ranged weapons keep the original
    // proof-of-concept long trace until their own projectile/hitscan paths land.
    ray.max_t = (in->weaponflags & BRIDGE_WEAPON_MELEE) ? (110.0f / in->scale) : 1000.0f;
    ray.flags = XRAY3_USE_MIN | XRAY3_USE_MAX;

    st_XORDEREDARRAY* npclist = zNPCMgr_GetNPCList();
    if (npclist == NULL)
        return;

    zNPCCommon* best = NULL;
    F32 bestDist = FLOAT_MAX;
    for (S32 i = 0; i < npclist->cnt; i++)
    {
        zNPCCommon* npc = (zNPCCommon*)npclist->list[i];
        if (!IsTF2BridgeRobot(npc) || !npc->IsAlive())
            continue;

        xCollis hit;
        memset(&hit, 0, sizeof(hit));
        hit.flags = XRAY3_USE_MIN | XRAY3_USE_MAX;
        xRayHitsBound(&ray, &npc->bound, &hit);
        if ((hit.flags & 0x1) && hit.dist < bestDist)
        {
            bestDist = hit.dist;
            best = npc;
        }
    }

    if (best == NULL)
        return;

    // BFBB's robot damage code expects the hit vector to describe the incoming
    // direction, not an absolute world position. Let its normal damage path do
    // the rest (HP, damage goal, death animation, rewards, etc.).
    best->Damage(DMGTYP_SIDE, NULL, &dir);
    printf("bfbb: tf2bridge -- shot hit NPC type %d at %.2f\n", (int)best->SelfType(),
           (double)bestDist);
}

// Called from zGameLoop right after the player entity updated. When TF2 is
// running the movement, put the player where TF2 says he is.
void zTF2Bridge_AfterPlayerUpdate()
{
    const BridgeIntentPacket* in = PuppetIntent();
    if (in == NULL)
    {
        return;
    }

    xEnt& e = globals.player.ent;
    if (e.frame == NULL || e.model == NULL)
    {
        return;
    }

    const xVec3 p = FromSource(in->px, in->py, in->pz, in->scale);

    e.frame->mat.pos = p;
    e.frame->oldmat.pos = p;
    *xEntGetPos(&e) = p; // the model's own matrix

    // TF2 owns the motion; BFBB's player must not also integrate any of his own.
    e.frame->vel.x = e.frame->vel.y = e.frame->vel.z = 0.0f;
    e.frame->dpos.x = e.frame->dpos.y = e.frame->dpos.z = 0.0f;
    e.frame->dvel.x = e.frame->dvel.y = e.frame->dvel.z = 0.0f;

    // Face where TF2 faces (heading only).
    const float yaw = in->yaw * kDegToRad;
    xMat4x3& m = e.frame->mat;
    m.at.x = sinf(yaw);
    m.at.y = 0.0f;
    m.at.z = cosf(yaw);
    m.up.x = 0.0f;
    m.up.y = 1.0f;
    m.up.z = 0.0f;
    m.right.x = cosf(yaw);
    m.right.y = 0.0f;
    m.right.z = -sinf(yaw);

    xBoundUpdate(&e.bound);
}

// Called from zGameLoop right after zCameraUpdate and before the camera is
// handed to the renderer. When TF2 is running the movement, look through TF2's
// eyes (first person).
void zTF2Bridge_AfterCameraUpdate()
{
    const BridgeIntentPacket* in = PuppetIntent();
    if (in == NULL || globals.camera.lo_cam == NULL)
    {
        return;
    }

    const float yaw = in->yaw * kDegToRad;
    const float pitch = in->pitch * kDegToRad; // positive looks down

    // Forward, then right = up x at (so an unrotated camera has right = +X,
    // which the renderer shows on screen-left; that is how it is meant to be),
    // then up = at x right.
    const float cp = cosf(pitch);
    float at[3] = { cp * sinf(yaw), -sinf(pitch), cp * cosf(yaw) };

    float right[3] = { at[2], 0.0f, -at[0] }; // (0,1,0) x at
    const float rl = sqrtf(right[0] * right[0] + right[2] * right[2]);
    if (rl < 0.0001f)
    {
        return; // looking straight up or down
    }
    right[0] /= rl;
    right[2] /= rl;

    float up[3] = { at[1] * right[2] - at[2] * right[1], at[2] * right[0] - at[0] * right[2],
                    at[0] * right[1] - at[1] * right[0] };

    xMat4x3& cm = globals.camera.mat;
    cm.right.x = right[0];
    cm.right.y = right[1];
    cm.right.z = right[2];
    cm.up.x = up[0];
    cm.up.y = up[1];
    cm.up.z = up[2];
    cm.at.x = at[0];
    cm.at.y = at[1];
    cm.at.z = at[2];
    cm.pos = FromSource(in->ex, in->ey, in->ez, in->scale);

    iCameraUpdatePos(globals.camera.lo_cam, &cm);
}
