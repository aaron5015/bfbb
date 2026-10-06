#include "zTF2Bridge.h"

#include "iTF2Bridge.h"
#include "iCamera.h"
#include "iEnv.h"
#include "xClumpColl.h"
#include "iCollide.h"
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

struct TF2BridgeDebugRay
{
    xVec3 origin;
    xVec3 end;
};

static TF2BridgeDebugRay sHitscanDebugRays[BRIDGE_MAX_HITSCAN_RAYS];
static uint32_t sHitscanDebugCount = 0;
static float sHitscanDebugTime = 0.0f;

struct TF2BridgeDebugExplosionTarget
{
    xVec3 pos;
    float distance;
    int32_t npcType;
    bool visible;
};

struct TF2BridgeDebugRocket
{
    static const uint32_t kMaxExposureSamples = 512;

    int32_t entIndex;
    xVec3 pos;
    xVec3 prevPos;
    xVec3 impact;
    xVec3 impactNormal;
    float radius;
    float damage;
    float impactTime;
    xVec3 sweepStart;
    xVec3 sweepEnd;
    float sweepTime;
    bool sweepHit;
    uint32_t explosionTargetCount;
    TF2BridgeDebugExplosionTarget explosionTargets[BRIDGE_MAX_ROCKETS];

    // Per-sample exposure visualization:
    // 0 = outside NPC bound, 1 = outside blast, 2 = blocked, 3 = visible,
    // 4 = direct ray blocked, but a bounded alternate path exists.
    uint32_t exposureSampleCount;
    xVec3 exposureSamplePos[kMaxExposureSamples];
    uint8_t exposureSampleState[kMaxExposureSamples];
    float exposureSampleTime;
    bool active;
    bool hasPrevious;
    bool impacted;
    bool terminated;
};

static TF2BridgeDebugRocket sRocketDebug[BRIDGE_MAX_ROCKETS] = {};
static float sRocketDebugPrintTimer = 0.0f;
static void TF2Bridge_FireHitscanRay(const BridgeIntentPacket* in, const float origin[3], const float dir[3], float range, uint32_t debugIndex);

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

static TF2BridgeDebugRocket* TF2Bridge_FindRocketTrack(int32_t entIndex)
{
    for (uint32_t i = 0; i < BRIDGE_MAX_ROCKETS; ++i)
    {
        if (sRocketDebug[i].entIndex == entIndex)
            return &sRocketDebug[i];
    }
    return NULL;
}

static TF2BridgeDebugRocket* TF2Bridge_AllocRocketTrack(int32_t entIndex)
{
    TF2BridgeDebugRocket* freeTrack = NULL;
    for (uint32_t i = 0; i < BRIDGE_MAX_ROCKETS; ++i)
    {
        if (sRocketDebug[i].entIndex == entIndex)
            return &sRocketDebug[i];
        if (sRocketDebug[i].entIndex == 0 && freeTrack == NULL)
            freeTrack = &sRocketDebug[i];
        if (!sRocketDebug[i].active && sRocketDebug[i].impactTime <= 0.0f && freeTrack == NULL)
            freeTrack = &sRocketDebug[i];
    }

    if (freeTrack != NULL)
    {
        *freeTrack = {};
        freeTrack->entIndex = entIndex;
    }
    return freeTrack;
}

static float TF2Bridge_GetBoundRadius(const xBound& bound)
{
    if (bound.type == XBOUND_TYPE_SPHERE)
        return bound.sph.r;

    xBox box;
    xBoundGetBox(box, bound);

    const float ex = 0.5f * (box.upper.x - box.lower.x);
    const float ey = 0.5f * (box.upper.y - box.lower.y);
    const float ez = 0.5f * (box.upper.z - box.lower.z);
    return sqrtf(ex * ex + ey * ey + ez * ez);
}

static bool IsTF2BridgeRobot(const zNPCCommon* npc);

static F32 TF2Bridge_RocketExposureThreshold()
{
    const char* value = getenv("BFBB_TF2BRIDGE_ROCKET_EXPOSURE_THRESHOLD");
    if (value != NULL && value[0] != '\0')
    {
        const F32 threshold = (F32)atof(value);
        if (threshold >= 0.0f && threshold <= 1.0f)
            return threshold;
    }

    // Require a meaningful amount of the sampled NPC volume to be directly
    // visible to the explosion. This keeps walls strict while letting corners
    // and rounded cover naturally transition from blocked to exposed.
    return 0.35f;
}

static bool TF2Bridge_RocketApplyDamage()
{
    const char* value = getenv("BFBB_TF2BRIDGE_ROCKET_APPLY_DAMAGE");
    if (value != NULL && value[0] != '\0')
        return atoi(value) != 0;

    return true;
}

static bool TF2Bridge_RocketSampleVisible(const xVec3& origin, const xVec3& sample,
    F32* outHitDistance = NULL, int* outQueryResult = NULL, uint32_t* outHitFlags = NULL,
    F32* outRayDistance = NULL, uint32_t* outHitOid = NULL, xVec3* outHitNormal = NULL,
    uint32_t* outHitTriIndex = NULL, F32* outHitTriR = NULL, F32* outHitTriD = NULL)
{
    const F32 dx = sample.x - origin.x;
    const F32 dy = sample.y - origin.y;
    const F32 dz = sample.z - origin.z;
    const F32 distanceSq = dx * dx + dy * dy + dz * dz;

    if (distanceSq <= 0.000001f)
        return true;

    const F32 distance = sqrtf(distanceSq);

    if (outQueryResult != NULL)
        *outQueryResult = 0;
    if (outHitFlags != NULL)
        *outHitFlags = 0;
    if (outRayDistance != NULL)
        *outRayDistance = distance;
    if (outHitOid != NULL)
        *outHitOid = 0;
    if (outHitNormal != NULL)
    {
        outHitNormal->x = 0.0f;
        outHitNormal->y = 0.0f;
        outHitNormal->z = 0.0f;
    }
    if (outHitTriIndex != NULL)
        *outHitTriIndex = 0;
    if (outHitTriR != NULL)
        *outHitTriR = 0.0f;
    if (outHitTriD != NULL)
        *outHitTriD = 0.0f;

    const F32 rayEpsilon = 0.05f;
    if (distance <= rayEpsilon)
        return true;

    const F32 invDistance = 1.0f / distance;
    const F32 dirX = dx * invDistance;
    const F32 dirY = dy * invDistance;
    const F32 dirZ = dz * invDistance;

    // The explosion is born on a world collision surface. Do not let that
    // immediate contact count as an occluder of the explosion itself.
    // Advance the visibility ray a small distance toward the sample; any
    // geometry encountered after this point is a genuine occluder.
    xRay3 ray;
    ray.origin.x = origin.x + dirX * rayEpsilon;
    ray.origin.y = origin.y + dirY * rayEpsilon;
    ray.origin.z = origin.z + dirZ * rayEpsilon;
    ray.dir.x = dirX;
    ray.dir.y = dirY;
    ray.dir.z = dirZ;
    ray.min_t = 0.0f;
    ray.max_t = distance - rayEpsilon;
    ray.flags = XRAY3_USE_MIN | XRAY3_USE_MAX;

    xCollis worldHit;
    memset(&worldHit, 0, sizeof(worldHit));
    worldHit.flags = k_HIT_0x200;

    const int queryResult = iRayHitsEnv(&ray, globals.sceneCur->env, &worldHit);
    const bool hitEnv = queryResult != 0;

    if (outQueryResult != NULL)
        *outQueryResult = queryResult;
    if (outHitFlags != NULL)
        *outHitFlags = worldHit.flags;
    if (outHitOid != NULL)
        *outHitOid = worldHit.oid;
    if (outHitNormal != NULL)
        *outHitNormal = worldHit.norm;
    if (outHitTriIndex != NULL)
    {
        // iCollide stores the JSP triangle pointer in RpCollisionTriangle::index
        // while the JSP query is active. Convert that pointer back to the
        // actual collision-tree array index so the debug output identifies a
        // real triangle rather than printing the pointer value.
        const xClumpCollBSPTree* tree =
            globals.sceneCur != NULL && globals.sceneCur->env != NULL &&
            globals.sceneCur->env->geom != NULL && globals.sceneCur->env->geom->jsp != NULL
                ? globals.sceneCur->env->geom->jsp->colltree
                : NULL;
        const xClumpCollBSPTriangle* hitTri =
            (const xClumpCollBSPTriangle*)(uintptr_t)worldHit.tri.index;
        if (tree != NULL && hitTri >= tree->triangles &&
            hitTri < tree->triangles + tree->numTriangles)
            *outHitTriIndex = (uint32_t)(hitTri - tree->triangles);
        else
            *outHitTriIndex = 0xffffffffu;
    }
    if (outHitTriR != NULL)
        *outHitTriR = worldHit.tri.r;
    if (outHitTriD != NULL)
        *outHitTriD = worldHit.tri.d;

    // iRayHitsEnv can leave a non-useful/sentinel distance when the query does
    // not produce a collision within the requested segment. Never interpret a
    // huge distance as visibility through the world; only a finite hit inside
    // this exact ray segment can block the sample.
    const F32 hitDistance = worldHit.dist;
    const bool validHit = hitEnv && isfinite(hitDistance) &&
        hitDistance >= 0.0f && hitDistance < ray.max_t - 0.01f;

    if (outHitDistance != NULL)
        *outHitDistance = validHit ? hitDistance : -1.0f;

    return !validHit;
}

static bool TF2Bridge_RocketSampleHasAlternatePath(const xVec3& origin,
    const xVec3& sample, F32 blastRadius)
{
    const F32 dx = sample.x - origin.x;
    const F32 dy = sample.y - origin.y;
    const F32 dz = sample.z - origin.z;
    const F32 sampleDistanceSq = dx * dx + dy * dy + dz * dz;
    if (sampleDistanceSq <= 0.000001f || blastRadius <= 0.000001f)
        return false;

    const F32 sampleDistance = sqrtf(sampleDistanceSq);

    // Diagnostic-only alternate blast paths. Stop at the first valid route;
    // we only care whether one exists.
    const F32 directions[26][3] = {
        {-1.0f, -1.0f, -1.0f}, {-1.0f, -1.0f, 0.0f}, {-1.0f, -1.0f, 1.0f},
        {-1.0f,  0.0f, -1.0f}, {-1.0f,  0.0f, 0.0f}, {-1.0f,  0.0f, 1.0f},
        {-1.0f,  1.0f, -1.0f}, {-1.0f,  1.0f, 0.0f}, {-1.0f,  1.0f, 1.0f},
        { 0.0f, -1.0f, -1.0f}, { 0.0f, -1.0f, 0.0f}, { 0.0f, -1.0f, 1.0f},
        { 0.0f,  0.0f, -1.0f},                         { 0.0f,  0.0f, 1.0f},
        { 0.0f,  1.0f, -1.0f}, { 0.0f,  1.0f, 0.0f}, { 0.0f,  1.0f, 1.0f},
        { 1.0f, -1.0f, -1.0f}, { 1.0f, -1.0f, 0.0f}, { 1.0f, -1.0f, 1.0f},
        { 1.0f,  0.0f, -1.0f}, { 1.0f,  0.0f, 0.0f}, { 1.0f,  0.0f, 1.0f},
        { 1.0f,  1.0f, -1.0f}, { 1.0f,  1.0f, 0.0f}, { 1.0f,  1.0f, 1.0f}
    };

    const F32 waypointDistance = 0.5f * fminf(blastRadius, sampleDistance);
    if (waypointDistance <= 0.000001f)
        return false;

    for (int i = 0; i < 26; ++i)
    {
        const F32 len = sqrtf(
            directions[i][0] * directions[i][0] +
            directions[i][1] * directions[i][1] +
            directions[i][2] * directions[i][2]);
        const F32 invLen = len > 0.000001f ? 1.0f / len : 0.0f;

        const xVec3 waypoint = {
            origin.x + directions[i][0] * invLen * waypointDistance,
            origin.y + directions[i][1] * invLen * waypointDistance,
            origin.z + directions[i][2] * invLen * waypointDistance
        };

        if (!TF2Bridge_RocketSampleVisible(origin, waypoint))
            continue;

        const F32 waypointToSampleX = sample.x - waypoint.x;
        const F32 waypointToSampleY = sample.y - waypoint.y;
        const F32 waypointToSampleZ = sample.z - waypoint.z;
        const F32 waypointToSample =
            sqrtf(waypointToSampleX * waypointToSampleX +
                  waypointToSampleY * waypointToSampleY +
                  waypointToSampleZ * waypointToSampleZ);

        if (waypointDistance + waypointToSample > blastRadius + 0.001f)
            continue;

        if (TF2Bridge_RocketSampleVisible(waypoint, sample))
            return true;
    }

    return false;
}

static void TF2Bridge_LogRocketCollisionNeighborhood(uint32_t hitTriIndex)
{
    if (globals.sceneCur == NULL || globals.sceneCur->env == NULL ||
        globals.sceneCur->env->geom == NULL || globals.sceneCur->env->geom->jsp == NULL ||
        globals.sceneCur->env->geom->jsp->colltree == NULL || hitTriIndex == 0xffffffffu)
        return;

    const xClumpCollBSPTree* tree = globals.sceneCur->env->geom->jsp->colltree;
    if (hitTriIndex >= tree->numTriangles)
        return;

    const xClumpCollBSPTriangle& base = tree->triangles[hitTriIndex];
    if (base.v.p == NULL)
        return;

    const float kSharedVertexEpsilonSq = 0.0001f;
    int logged = 0;
    printf("bfbb: tf2bridge -- rocket collision neighborhood base=%u flags=0x%02x mat=%u\\n",
        (unsigned)hitTriIndex, (unsigned)base.flags, (unsigned)base.matIndex);

    for (uint32_t i = 0; i < tree->numTriangles && logged < 16; ++i)
    {
        if (i == hitTriIndex)
            continue;
        const xClumpCollBSPTriangle& t = tree->triangles[i];
        if (t.v.p == NULL)
            continue;
        bool sharesVertex = false;
        for (int a = 0; a < 3 && !sharesVertex; ++a)
        {
            for (int b = 0; b < 3; ++b)
            {
                const float dx = base.v.p[a].x - t.v.p[b].x;
                const float dy = base.v.p[a].y - t.v.p[b].y;
                const float dz = base.v.p[a].z - t.v.p[b].z;
                if (dx * dx + dy * dy + dz * dz <= kSharedVertexEpsilonSq)
                {
                    sharesVertex = true;
                    break;
                }
            }
        }
        if (!sharesVertex)
            continue;
        printf("bfbb: tf2bridge --   tri=%u flags=0x%02x mat=%u v0=(%.3f %.3f %.3f) v1=(%.3f %.3f %.3f) v2=(%.3f %.3f %.3f)\\n",
            (unsigned)i, (unsigned)t.flags, (unsigned)t.matIndex,
            t.v.p[0].x, t.v.p[0].y, t.v.p[0].z,
            t.v.p[1].x, t.v.p[1].y, t.v.p[1].z,
            t.v.p[2].x, t.v.p[2].y, t.v.p[2].z);
        ++logged;
    }
}

static void TF2Bridge_BuildRocketExplosionDiagnostics(TF2BridgeDebugRocket* rocket)
{
    if (rocket == NULL || globals.sceneCur == NULL)
        return;

    rocket->explosionTargetCount = 0;
    rocket->exposureSampleCount = 0;
    rocket->exposureSampleTime = 5.0f;

    st_XORDEREDARRAY* npclist = zNPCMgr_GetNPCList();
    if (npclist == NULL)
        return;

    const F32 baseDamage = rocket->damage;
    const F32 radius = rocket->radius;
    const F32 radiusSq = radius * radius;
    const F32 exposureThreshold = TF2Bridge_RocketExposureThreshold();
    const bool applyDamage = TF2Bridge_RocketApplyDamage();

    for (S32 i = 0; i < npclist->cnt; ++i)
    {
        zNPCCommon* npc = (zNPCCommon*)npclist->list[i];
        if (!IsTF2BridgeRobot(npc) || !npc->IsAlive())
            continue;

        xBox sampleBox;
        xBoundGetBox(sampleBox, npc->bound);

        const xVec3* center = xBoundCenter(&npc->bound);
        if (center == NULL)
            continue;

        // The broad-phase bound test is only a cheap way to reject NPCs that
        // cannot possibly overlap the blast.  The actual exposure decision
        // below is made from the sampled bound volume.
        xSphere blastSphere;
        blastSphere.center = rocket->impact;
        blastSphere.r = radius;

        xCollis blastHit;
        memset(&blastHit, 0, sizeof(blastHit));
        blastHit.flags = k_HIT_0x200;

        xSphereHitsBound(&blastSphere, &npc->bound, &blastHit);
        if ((blastHit.flags & 0x1) == 0)
            continue;

        if (rocket->explosionTargetCount >= BRIDGE_MAX_ROCKETS)
            break;

        TF2BridgeDebugExplosionTarget& target =
            rocket->explosionTargets[rocket->explosionTargetCount++];

        target.pos = *center;
        target.distance = 0.0f;
        target.npcType = (int32_t)npc->SelfType();
        target.visible = false;

        // Sample the NPC's actual bound volume.  A sample only contributes to
        // splash exposure if it is BOTH inside the rocket's radius and visible
        // from the explosion.  This prevents a large NPC bound from producing
        // 100% exposure when the actual robot is outside the blast sphere.
        S32 blastSamples = 0;
        S32 visibleSamples = 0;
        S32 alternatePathSamples = 0;
        S32 totalSamples = 0;
        S32 exposureRayDebugCount = 0;
        F32 nearestVisibleDistance = FLOAT_MAX;

        const F32 sampleFrac[8] = {
            0.0625f, 0.1875f, 0.3125f, 0.4375f,
            0.5625f, 0.6875f, 0.8125f, 0.9375f
        };

        for (S32 sx = 0; sx < 8; ++sx)
        {
            for (S32 sy = 0; sy < 8; ++sy)
            {
                for (S32 sz = 0; sz < 8; ++sz)
                {
                    xVec3 sample;
                    sample.x = sampleBox.lower.x +
                        (sampleBox.upper.x - sampleBox.lower.x) * sampleFrac[sx];
                    sample.y = sampleBox.lower.y +
                        (sampleBox.upper.y - sampleBox.lower.y) * sampleFrac[sy];
                    sample.z = sampleBox.lower.z +
                        (sampleBox.upper.z - sampleBox.lower.z) * sampleFrac[sz];

                    xCollis inside;
                    memset(&inside, 0, sizeof(inside));
                    xVecHitsBound(&sample, &npc->bound, &inside);

                    if ((inside.flags & 0x1) == 0)
                        continue;

                    const uint32_t sampleIndex = totalSamples;
                    if (sampleIndex < TF2BridgeDebugRocket::kMaxExposureSamples)
                    {
                        rocket->exposureSamplePos[sampleIndex] = sample;
                        rocket->exposureSampleState[sampleIndex] = 1; // outside blast
                        rocket->exposureSampleCount = sampleIndex + 1;
                    }

                    ++totalSamples;                    const F32 dx = sample.x - rocket->impact.x;
                    const F32 dy = sample.y - rocket->impact.y;
                    const F32 dz = sample.z - rocket->impact.z;
                    const F32 sampleDistSq = dx * dx + dy * dy + dz * dz;

                    if (sampleDistSq > radiusSq)
                        continue;

                    ++blastSamples;

                    if (sampleIndex < TF2BridgeDebugRocket::kMaxExposureSamples)
                        rocket->exposureSampleState[sampleIndex] = 2; // blocked

                    // The rocket impact lies on the collision surface.
                    // Start splash visibility just outside that exact surface
                    // using the triangle's collision normal. This avoids
                    // starting rays inside floors, rocks, walls, or other JSP
                    // triangles when the projectile strikes at an angle.
                    const F32 kSplashOriginEpsilon = 0.02f;
                    xVec3 splashOrigin = rocket->impact;
                    splashOrigin.x += rocket->impactNormal.x * kSplashOriginEpsilon;
                    splashOrigin.y += rocket->impactNormal.y * kSplashOriginEpsilon;
                    splashOrigin.z += rocket->impactNormal.z * kSplashOriginEpsilon;

                    int exposureQueryResult = 0;
                    uint32_t exposureHitFlags = 0;
                    F32 exposureHitDistance = -1.0f;
                    F32 exposureRayDistance = 0.0f;
                    uint32_t exposureHitOid = 0;
                    xVec3 exposureHitNormal = { 0.0f, 0.0f, 0.0f };
                    uint32_t exposureHitTriIndex = 0;
                    F32 exposureHitTriR = 0.0f;
                    F32 exposureHitTriD = 0.0f;
                    const bool sampleVisible = TF2Bridge_RocketSampleVisible(
                        splashOrigin, sample, &exposureHitDistance,
                        &exposureQueryResult, &exposureHitFlags, &exposureRayDistance,
                        &exposureHitOid, &exposureHitNormal, &exposureHitTriIndex,
                        &exposureHitTriR, &exposureHitTriD);

                    if (!sampleVisible && exposureRayDebugCount < 12)
                    {
                        if (exposureHitTriIndex != 0xffffffffu)
                            TF2Bridge_LogRocketCollisionNeighborhood(exposureHitTriIndex);
                        const F32 rayInvDistance =
                            exposureRayDistance > 0.000001f ? 1.0f / exposureRayDistance : 0.0f;
                        const xVec3 exposureHitPos = {
                            splashOrigin.x + (sample.x - splashOrigin.x) * rayInvDistance * exposureHitDistance,
                            splashOrigin.y + (sample.y - splashOrigin.y) * rayInvDistance * exposureHitDistance,
                            splashOrigin.z + (sample.z - splashOrigin.z) * rayInvDistance * exposureHitDistance
                        };

                        const xClumpCollBSPTriangle* debugTri = NULL;
                        const xClumpCollBSPTree* debugTree =
                            globals.sceneCur != NULL && globals.sceneCur->env != NULL &&
                            globals.sceneCur->env->geom != NULL && globals.sceneCur->env->geom->jsp != NULL
                                ? globals.sceneCur->env->geom->jsp->colltree
                                : NULL;
                        if (debugTree != NULL && exposureHitTriIndex < debugTree->numTriangles)
                            debugTri = &debugTree->triangles[exposureHitTriIndex];

                        printf("bfbb: tf2bridge -- rocket exposure-ray target=%d sample=%u "
                            "result=%d flags=0x%08x rayDist=%.3f hitDist=%.3f "
                            "origin=(%.3f %.3f %.3f) hit=(%.3f %.3f %.3f) "
                            "sample=(%.3f %.3f %.3f) oid=%u triIndex=%u triR=%.3f triD=%.3f "
                            "normal=(%.3f %.3f %.3f) "
                            "triFlags=0x%02x triMat=%u "
                            "v0=(%.3f %.3f %.3f) v1=(%.3f %.3f %.3f) v2=(%.3f %.3f %.3f)\\n",
                            (int)target.npcType, (unsigned)sampleIndex,
                            exposureQueryResult, (unsigned)exposureHitFlags,
                            (double)exposureRayDistance, (double)exposureHitDistance,
                            (double)splashOrigin.x, (double)splashOrigin.y, (double)splashOrigin.z,
                            (double)exposureHitPos.x, (double)exposureHitPos.y, (double)exposureHitPos.z,
                            (double)sample.x, (double)sample.y, (double)sample.z,
                            (unsigned)exposureHitOid, (unsigned)exposureHitTriIndex,
                            (double)exposureHitTriR, (double)exposureHitTriD,
                            (double)exposureHitNormal.x, (double)exposureHitNormal.y,
                            (double)exposureHitNormal.z,
                            debugTri != NULL ? (unsigned)debugTri->flags : 0u,
                            debugTri != NULL ? (unsigned)debugTri->matIndex : 0u,
                            debugTri != NULL && debugTri->v.p != NULL ? (double)debugTri->v.p[0].x : 0.0,
                            debugTri != NULL && debugTri->v.p != NULL ? (double)debugTri->v.p[0].y : 0.0,
                            debugTri != NULL && debugTri->v.p != NULL ? (double)debugTri->v.p[0].z : 0.0,
                            debugTri != NULL && debugTri->v.p != NULL ? (double)debugTri->v.p[1].x : 0.0,
                            debugTri != NULL && debugTri->v.p != NULL ? (double)debugTri->v.p[1].y : 0.0,
                            debugTri != NULL && debugTri->v.p != NULL ? (double)debugTri->v.p[1].z : 0.0,
                            debugTri != NULL && debugTri->v.p != NULL ? (double)debugTri->v.p[2].x : 0.0,
                            debugTri != NULL && debugTri->v.p != NULL ? (double)debugTri->v.p[2].y : 0.0,
                            debugTri != NULL && debugTri->v.p != NULL ? (double)debugTri->v.p[2].z : 0.0);
                        ++exposureRayDebugCount;
                    }

                    if (sampleVisible)
                    {
                        ++visibleSamples;

                        if (sampleIndex < TF2BridgeDebugRocket::kMaxExposureSamples)
                            rocket->exposureSampleState[sampleIndex] = 3;

                        const F32 sampleDistance = sqrtf(sampleDistSq);
                        if (sampleDistance < nearestVisibleDistance)
                            nearestVisibleDistance = sampleDistance;
                    }
                    else
                    {
                        const bool hasAlternatePath =
                            TF2Bridge_RocketSampleHasAlternatePath(
                                splashOrigin, sample, radius);
                        if (hasAlternatePath)
                        {
                            ++alternatePathSamples;
                            if (sampleIndex < TF2BridgeDebugRocket::kMaxExposureSamples)
                                rocket->exposureSampleState[sampleIndex] = 4;
                        }
                    }
                }
            }
        }

        // Exposure is the visible fraction of the NPC volume that is actually
        // inside the blast. Samples outside the radius must not count as hidden
        // splash exposure.
        const F32 exposure =
            blastSamples > 0
                ? (F32)visibleSamples / (F32)blastSamples
                : 0.0f;

        printf("bfbb: tf2bridge -- rocket alternate-path target type %d samples=%d/%d\n",
            (int)target.npcType, (int)alternatePathSamples, (int)blastSamples);

        target.distance =
            nearestVisibleDistance < FLOAT_MAX
                ? nearestVisibleDistance
                : radius;

        if (totalSamples == 0 || blastSamples == 0 || exposure <= exposureThreshold)
        {
            printf("bfbb: tf2bridge -- rocket explosion target type %d dist=%.2f exposure=%d/%d (%.1f%%) blastSamples=%d/%d BLOCKED threshold=%.1f%% damage=0\n",
                (int)target.npcType, (double)target.distance,
                (int)visibleSamples, (int)blastSamples,
                (double)(exposure * 100.0f),
                (int)blastSamples, (int)totalSamples,
                (double)(exposureThreshold * 100.0f));
            continue;
        }

        // Any exposed portion of the target is a valid splash hit. Fully
        // occluded targets still fail the check above, while partial exposure
        // is preserved instead of being discarded by a hard 35% gate.
        target.visible = true;

        // TF2 RadiusDamage uses linear distance falloff:
        // damage - distance * (damage / radius).
        // Use the nearest actually exposed point of the sampled bound rather
        // than the NPC center, which may be outside the blast while part of
        // the NPC is legitimately inside it.
        F32 t = radius > 0.0001f
            ? target.distance / radius
            : 1.0f;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;

        const F32 damageScale = 1.0f - t;
        const F32 damage = baseDamage * damageScale;

        if (damage > 0.0f && applyDamage)
            npc->Damage(DMGTYP_SIDE, NULL, &rocket->impact);

        printf("bfbb: tf2bridge -- rocket explosion target type %d dist=%.2f exposure=%d/%d (%.1f%%) blastSamples=%d/%d damage=%.2f scale=%.3f VISIBLE applyDamage=%d\n",
            (int)target.npcType, (double)target.distance,
            (int)visibleSamples, (int)blastSamples,
            (double)(exposure * 100.0f),
            (int)blastSamples, (int)totalSamples,
            (double)damage, (double)damageScale,
            applyDamage ? 1 : 0);
    }

    printf("bfbb: tf2bridge -- rocket explosion radius=%.2f baseDamage=%.2f exposureThreshold=%.1f%% applyDamage=%d targets=%u\n",
        (double)radius, (double)baseDamage,
        (double)(exposureThreshold * 100.0f),
        applyDamage ? 1 : 0,
        (unsigned)rocket->explosionTargetCount);
}

static void TF2Bridge_ProcessRocketDiagnostics(const BridgeIntentPacket* in)
{
    if (in == NULL || globals.sceneCur == NULL || in->scale <= 0.0f)
        return;

    if (in->rocketDebugLifetime <= 0.0f)
    {
        for (uint32_t i = 0; i < BRIDGE_MAX_ROCKETS; ++i)
        {
            sRocketDebug[i].active = false;
            sRocketDebug[i].impacted = false;
            sRocketDebug[i].impactTime = 0.0f;
            sRocketDebug[i].entIndex = 0;
            sRocketDebug[i].hasPrevious = false;