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
    uint32_t* outHitTriIndex = NULL, F32* outHitTriR = NULL, F32* outHitTriD = NULL,
    int32_t debugTargetType = -1)
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

    // Diagnostic only: describe the exact JSP surface that blocks a rocket
    // splash visibility ray. One ray is issued per in-radius robot, so this
    // emits at most one record per blocked target, not per projectile segment.
    if (validHit)
    {
        const xClumpCollBSPTree* tree =
            globals.sceneCur != NULL && globals.sceneCur->env != NULL &&
            globals.sceneCur->env->geom != NULL && globals.sceneCur->env->geom->jsp != NULL
                ? globals.sceneCur->env->geom->jsp->colltree
                : NULL;
        const xClumpCollBSPTriangle* tri =
            (const xClumpCollBSPTriangle*)(uintptr_t)worldHit.tri.index;

        if (tree != NULL && tri >= tree->triangles &&
            tri < tree->triangles + tree->numTriangles && tri->v.p != NULL)
        {
            const xVec3 hit = {
                ray.origin.x + dirX * hitDistance,
                ray.origin.y + dirY * hitDistance,
                ray.origin.z + dirZ * hitDistance
            };
            const xVec3 a = { tri->v.p[0].x, tri->v.p[0].y, tri->v.p[0].z };
            const xVec3 b = { tri->v.p[1].x, tri->v.p[1].y, tri->v.p[1].z };
            const xVec3 d = { tri->v.p[2].x, tri->v.p[2].y, tri->v.p[2].z };

            // Point-to-segment distance: indicates whether the ray hit close
            // to a triangle edge or well inside its face.
            const auto edgeDistance = [&hit](const xVec3& p0, const xVec3& p1) -> F32
            {
                const F32 ex = p1.x - p0.x, ey = p1.y - p0.y, ez = p1.z - p0.z;
                const F32 px = hit.x - p0.x, py = hit.y - p0.y, pz = hit.z - p0.z;
                const F32 lenSq = ex * ex + ey * ey + ez * ez;
                F32 t = lenSq > 0.000001f ? (px * ex + py * ey + pz * ez) / lenSq : 0.0f;
                if (t < 0.0f) t = 0.0f;
                if (t > 1.0f) t = 1.0f;
                const F32 qx = p0.x + ex * t, qy = p0.y + ey * t, qz = p0.z + ez * t;
                const F32 dx = hit.x - qx, dy = hit.y - qy, dz = hit.z - qz;
                return sqrtf(dx * dx + dy * dy + dz * dz);
            };

            const F32 edgeAB = edgeDistance(a, b);
            const F32 edgeBD = edgeDistance(b, d);
            const F32 edgeDA = edgeDistance(d, a);
            const F32 sideOrigin =
                worldHit.norm.x * (origin.x - hit.x) +
                worldHit.norm.y * (origin.y - hit.y) +
                worldHit.norm.z * (origin.z - hit.z);
            const F32 sideTarget =
                worldHit.norm.x * (sample.x - hit.x) +
                worldHit.norm.y * (sample.y - hit.y) +
                worldHit.norm.z * (sample.z - hit.z);

            printf(
                "bfbb: tf2bridge -- rocket splash BLOCKER targetType=%d tri=%u flags=0x%02x mat=%u "
                "hit=(%.3f %.3f %.3f) hitDist=%.3f rayDist=%.3f "
                "normal=(%.3f %.3f %.3f) edgeDist=(%.3f %.3f %.3f) "
                "side=(%.3f %.3f) origin=(%.3f %.3f %.3f) target=(%.3f %.3f %.3f) "
                "v0=(%.3f %.3f %.3f) v1=(%.3f %.3f %.3f) v2=(%.3f %.3f %.3f)\\n",
                (int)debugTargetType, (unsigned)(tri - tree->triangles), (unsigned)tri->flags,
                (unsigned)tri->matIndex,
                (double)hit.x, (double)hit.y, (double)hit.z,
                (double)hitDistance, (double)distance,
                (double)worldHit.norm.x, (double)worldHit.norm.y, (double)worldHit.norm.z,
                (double)edgeAB, (double)edgeBD, (double)edgeDA,
                (double)sideOrigin, (double)sideTarget,
                (double)origin.x, (double)origin.y, (double)origin.z,
                (double)sample.x, (double)sample.y, (double)sample.z,
                (double)a.x, (double)a.y, (double)a.z,
                (double)b.x, (double)b.y, (double)b.z,
                (double)d.x, (double)d.y, (double)d.z);
        }
        else
        {
            printf("bfbb: tf2bridge -- rocket splash BLOCKER hitDist=%.3f rayDist=%.3f "
                   "triangle=unavailable flags=0x%08x\\n",
                   (double)hitDistance, (double)distance, (unsigned)worldHit.flags);
        }
    }

    return !validHit;
}

static xVec3 TF2Bridge_RocketNearestBoundPoint(const xBound& bound, const xVec3& point)
{
    if (bound.type == XBOUND_TYPE_SPHERE)
    {
        const xVec3 center = bound.sph.center;
        const F32 dx = point.x - center.x;
        const F32 dy = point.y - center.y;
        const F32 dz = point.z - center.z;
        const F32 lenSq = dx * dx + dy * dy + dz * dz;

        if (lenSq <= 0.000001f || lenSq <= bound.sph.r * bound.sph.r)
            return center;

        const F32 invLen = bound.sph.r / sqrtf(lenSq);
        return {
            center.x + dx * invLen,
            center.y + dy * invLen,
            center.z + dz * invLen
        };
    }

    const bool obb = bound.type == XBOUND_TYPE_OBB && bound.mat != NULL;
    xBox box;
    if (obb)
        box = bound.box.box;
    else
        xBoundGetBox(box, bound);

    xVec3 localPoint = point;
    if (obb)
        xMat4x3Tolocal(&localPoint, bound.mat, &point);

    xVec3 nearest = {
        localPoint.x < box.lower.x ? box.lower.x :
            (localPoint.x > box.upper.x ? box.upper.x : localPoint.x),
        localPoint.y < box.lower.y ? box.lower.y :
            (localPoint.y > box.upper.y ? box.upper.y : localPoint.y),
        localPoint.z < box.lower.z ? box.lower.z :
            (localPoint.z > box.upper.z ? box.upper.z : localPoint.z)
    };

    if (obb)
    {
        xVec3 worldPoint;
        xMat4x3Toworld(&worldPoint, bound.mat, &nearest);
        return worldPoint;
    }

    return nearest;
}

// BFBB NPCs use a simple sphere as their active collision bound. Keep the
// nearest point for the TF2-style radius broadphase, but use a body target at
// the NPC's collision height for visibility. This lets the target move toward
// the explosion around side cover without moving vertically over a ledge.
static xVec3 TF2Bridge_RocketBodyTargetPoint(
    const xBound& bound, const xVec3& point)
{
    if (bound.type != XBOUND_TYPE_SPHERE)
        return *xBoundCenter(&bound);

    const xVec3 center = bound.sph.center;
    const F32 dx = point.x - center.x;
    const F32 dz = point.z - center.z;
    const F32 horizontalDistSq = dx * dx + dz * dz;

    if (horizontalDistSq <= 0.000001f)
        return center;

    const F32 horizontalDist = sqrtf(horizontalDistSq);
    const F32 scale = bound.sph.r / horizontalDist;

    return {
        center.x + dx * scale,
        center.y,
        center.z + dz * scale
    };
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
    const bool applyDamage = TF2Bridge_RocketApplyDamage();

    // TF2's rocket Explode() moves the explosion origin one Source unit
    // along the impact normal before RadiusDamage(). Convert that to BFBB
    // units using the same scale as the rest of the bridge.
    const F32 kSplashOriginOffset = 1.0f / 40.0f;
    xVec3 splashOrigin = rocket->impact;
    splashOrigin.x += rocket->impactNormal.x * kSplashOriginOffset;
    splashOrigin.y += rocket->impactNormal.y * kSplashOriginOffset;
    splashOrigin.z += rocket->impactNormal.z * kSplashOriginOffset;

    for (S32 i = 0; i < npclist->cnt; ++i)
    {
        zNPCCommon* npc = (zNPCCommon*)npclist->list[i];
        if (!IsTF2BridgeRobot(npc) || !npc->IsAlive())
            continue;

        if (rocket->explosionTargetCount >= BRIDGE_MAX_ROCKETS)
            break;

        // TF2 RadiusDamage first asks the collision system for the nearest
        // point on the entity's collision bounds. If that point is outside
        // the radius, the entity is not considered for splash damage.
        const xVec3 nearest = TF2Bridge_RocketNearestBoundPoint(
            npc->bound, splashOrigin);

        const F32 ndx = nearest.x - splashOrigin.x;
        const F32 ndy = nearest.y - splashOrigin.y;
        const F32 ndz = nearest.z - splashOrigin.z;
        const F32 nearestDistSq = ndx * ndx + ndy * ndy + ndz * ndz;

        if (nearestDistSq > radiusSq)
            continue;

        TF2BridgeDebugExplosionTarget& target =
            rocket->explosionTargets[rocket->explosionTargetCount++];

        // TF2 uses the nearest collision point for the radius broadphase,
        // not as the visibility target. BFBB NPCs use a simple sphere bound,
        // so aim the visibility ray at the closest horizontal point on the
        // NPC while keeping its normal collision height.
        const xVec3 targetPoint = TF2Bridge_RocketBodyTargetPoint(
            npc->bound, splashOrigin);

        // Focused geometry diagnostics for the common robot type used in the
        // cave, rock, and ledge regression tests. Keep other NPCs out of this
        // line so simultaneous test targets cannot be confused.
        const bool logTargetGeometry = npc->SelfType() == 1314148924;
        if (logTargetGeometry)
        {
            const xVec3 center = *xBoundCenter(&npc->bound);
            const F32 cx = targetPoint.x - center.x;
            const F32 cy = targetPoint.y - center.y;
            const F32 cz = targetPoint.z - center.z;

            printf(
                "bfbb: tf2bridge -- rocket splash targetGeom type=%d boundType=%u "
                "center=(%.3f %.3f %.3f) nearest=(%.3f %.3f %.3f) "
                "body=(%.3f %.3f %.3f) bodyOffset=(%.3f %.3f %.3f) "
                "sphereRadius=%.3f\\n",
                (int)npc->SelfType(), (unsigned)npc->bound.type,
                (double)center.x, (double)center.y, (double)center.z,
                (double)nearest.x, (double)nearest.y, (double)nearest.z,
                (double)targetPoint.x, (double)targetPoint.y, (double)targetPoint.z,
                (double)cx, (double)cy, (double)cz,
                (double)(npc->bound.type == XBOUND_TYPE_SPHERE ? npc->bound.sph.r : -1.0f));
        }

        target.pos = targetPoint;
        target.distance = sqrtf(
            SQR(targetPoint.x - splashOrigin.x) +
            SQR(targetPoint.y - splashOrigin.y) +
            SQR(targetPoint.z - splashOrigin.z));
        target.npcType = (int32_t)npc->SelfType();
        target.visible = false;

        F32 targetDx = targetPoint.x - splashOrigin.x;
        F32 targetDy = targetPoint.y - splashOrigin.y;
        F32 targetDz = targetPoint.z - splashOrigin.z;
        const F32 targetDistSq = targetDx * targetDx +
            targetDy * targetDy + targetDz * targetDz;
        const F32 targetDistance = sqrtf(targetDistSq);

        if (targetDistance <= 0.000001f)
        {
            target.visible = true;
            target.distance = 0.0f;
        }
        else
        {
            // Trace directly to the nearest point on the NPC bound. This is
            // intentionally not exact Source BodyTarget behavior; it tests
            // whether an exposed part of the BFBB collision bound can receive
            // the blast without adding arbitrary wraparound or sampling rules.
            const bool visible = TF2Bridge_RocketSampleVisible(
                splashOrigin, targetPoint, NULL, NULL, NULL, NULL, NULL, NULL,
                NULL, NULL, NULL, (int32_t)target.npcType);

            if (!visible)
            {
                printf(
                    "bfbb: tf2bridge -- rocket explosion target type %d "
                    "nearest=%.2f body=%.2f BLOCKED damage=0\\n",
                    (int)target.npcType, (double)sqrtf(nearestDistSq),
                    (double)targetDistance);
                continue;
            }

            target.visible = true;
            // Use the visible target point as the falloff distance.
            target.distance = targetDistance;
        }

        F32 t = radius > 0.0001f
            ? target.distance / radius
            : 1.0f;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;

        const F32 damageScale = 1.0f - t;
        const F32 damage = baseDamage * damageScale;

        if (damage > 0.0f && applyDamage)
            npc->Damage(DMGTYP_SIDE, NULL, &rocket->impact);

        printf(
            "bfbb: tf2bridge -- rocket explosion target type %d "
            "nearest=%.2f body=%.2f VISIBLE damage=%.2f scale=%.3f "
            "applyDamage=%d\\n",
            (int)target.npcType, (double)sqrtf(nearestDistSq),
            (double)target.distance, (double)damage, (double)damageScale,
            applyDamage ? 1 : 0);
    }

    printf(
        "bfbb: tf2bridge -- rocket explosion radius=%.2f baseDamage=%.2f "
        "occlusion=nearest-bound-point applyDamage=%d targets=%u\\n",
        (double)radius, (double)baseDamage,
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
            sRocketDebug[i].sweepTime = 0.0f;
        }
        return;
    }

    bool seen[BRIDGE_MAX_ROCKETS] = {};
    const uint32_t count = in->rocketCount > BRIDGE_MAX_ROCKETS ? BRIDGE_MAX_ROCKETS : in->rocketCount;

    for (uint32_t i = 0; i < count; ++i)
    {
        const int32_t entIndex = in->rocketEntIndex[i];
        if (entIndex <= 0)
            continue;

        TF2BridgeDebugRocket* rocket = TF2Bridge_FindRocketTrack(entIndex);
        if (rocket == NULL)
            rocket = TF2Bridge_AllocRocketTrack(entIndex);
        if (rocket == NULL)
            continue;

        for (uint32_t k = 0; k < BRIDGE_MAX_ROCKETS; ++k)
        {
            if (&sRocketDebug[k] == rocket)
            {
                seen[k] = true;
                break;
            }
        }

        const xVec3 pos = FromSource(in->rocketPos[i][0], in->rocketPos[i][1],
                                     in->rocketPos[i][2], in->scale);
        rocket->radius = in->rocketRadius[i] / in->scale;
        rocket->damage = in->rocketDamage[i];
        rocket->active = true;

        // If this entity index was reused after its old diagnostic expired,
        // start a fresh trajectory instead of inheriting the previous one.
        if (rocket->impacted && rocket->impactTime <= 0.0f)
        {
            rocket->hasPrevious = false;
            rocket->impacted = false;
        }

        if (!rocket->impacted && rocket->hasPrevious)
        {
            const xVec3 delta = {
                pos.x - rocket->prevPos.x,
                pos.y - rocket->prevPos.y,
                pos.z - rocket->prevPos.z
            };
            const float len = sqrtf(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);

            if (len > 0.0001f)
            {
                rocket->sweepStart = rocket->prevPos;
                rocket->sweepEnd = pos;
                rocket->sweepTime = 5.0f;
                rocket->sweepHit = false;

                xRay3 ray;
                ray.origin = rocket->prevPos;
                ray.dir.x = delta.x / len;
                ray.dir.y = delta.y / len;
                ray.dir.z = delta.z / len;
                ray.min_t = 0.0f;
                ray.max_t = len;
                ray.flags = XRAY3_USE_MIN | XRAY3_USE_MAX;

                // Use BFBB's native scene ray query rather than only
                // the JSP environment. This includes the environment plus
                // collision-bearing scene entities/NPCs, which is the same
                // collision path BFBB itself uses for scene ray tests.
                xCollis sceneHit;
                memset(&sceneHit, 0, sizeof(sceneHit));
                sceneHit.flags = k_HIT_0x200;

                xRayHitsScene(globals.sceneCur, &ray, &sceneHit);

                const bool hitScene =
                    (sceneHit.flags & k_HIT_IT) != 0 &&
                    sceneHit.dist >= 0.0f &&
                    sceneHit.dist <= len;

                const bool hitEntity = hitScene && sceneHit.optr != NULL;
                rocket->sweepHit = hitScene;

                if (hitEntity)
                {
                    printf("bfbb: tf2bridge -- rocket %d hit scene entity id %u at %.2f\n",
                        entIndex, (unsigned)sceneHit.oid,
                        (double)sceneHit.dist);
                }

                if (!hitScene)
                {
                    xCollis envHit;
                    memset(&envHit, 0, sizeof(envHit));
                    envHit.flags = k_HIT_0x200;

                    const bool hitEnv =
                        iRayHitsEnv(&ray, globals.sceneCur->env, &envHit) != 0 &&
                        envHit.dist >= 0.0f && envHit.dist <= len;

                    // Diagnostic-only long probe. Keep the actual rocket sweep unchanged;
                    // this tells us whether the JSP query can see the same surface when given
                    // a longer segment from the exact same starting point and direction.
                    const float probeLen = 8.0f;
                    xRay3 probeRay;
                    probeRay.origin = rocket->prevPos;
                    probeRay.dir = ray.dir;
                    probeRay.min_t = 0.0f;
                    probeRay.max_t = probeLen;
                    probeRay.flags = XRAY3_USE_MIN | XRAY3_USE_MAX;

                    xCollis probeHit;
                    memset(&probeHit, 0, sizeof(probeHit));
                    probeHit.flags = k_HIT_0x200;

                    const bool probeHitEnv =
                        iRayHitsEnv(&probeRay, globals.sceneCur->env, &probeHit) != 0 &&
                        probeHit.dist >= 0.0f && probeHit.dist <= probeLen;

                    printf("bfbb: tf2bridge -- rocket %d sweep MISS len=%.3f start=(%.3f %.3f %.3f) end=(%.3f %.3f %.3f) sceneFlags=0x%08x sceneDist=%.3f env=%s envDist=%.3f longProbe=%s probeDist=%.3f\n",
                        entIndex, (double)len,
                        (double)rocket->sweepStart.x, (double)rocket->sweepStart.y, (double)rocket->sweepStart.z,
                        (double)rocket->sweepEnd.x, (double)rocket->sweepEnd.y, (double)rocket->sweepEnd.z,
                        (unsigned)sceneHit.flags, (double)sceneHit.dist,
                        hitEnv ? "HIT" : "MISS", hitEnv ? (double)envHit.dist : -1.0,
                        probeHitEnv ? "HIT" : "MISS", probeHitEnv ? (double)probeHit.dist : -1.0);
                }

                if (hitScene)
                {
                    rocket->impact.x = rocket->prevPos.x + ray.dir.x * sceneHit.dist;
                    rocket->impact.y = rocket->prevPos.y + ray.dir.y * sceneHit.dist;
                    rocket->impact.z = rocket->prevPos.z + ray.dir.z * sceneHit.dist;

                    // Keep the actual JSP collision normal. Splash visibility
                    // should begin just outside the surface we struck, not
                    // merely along the projectile's incoming direction.
                    rocket->impactNormal = sceneHit.norm;
                    const F32 normalLen = sqrtf(
                        rocket->impactNormal.x * rocket->impactNormal.x +
                        rocket->impactNormal.y * rocket->impactNormal.y +
                        rocket->impactNormal.z * rocket->impactNormal.z);
                    if (normalLen > 0.0001f)
                    {
                        rocket->impactNormal.x /= normalLen;
                        rocket->impactNormal.y /= normalLen;
                        rocket->impactNormal.z /= normalLen;

                        // Ensure the normal points back toward the projectile
                        // side of the surface.
                        const F32 normalDotIncoming =
                            rocket->impactNormal.x * ray.dir.x +
                            rocket->impactNormal.y * ray.dir.y +
                            rocket->impactNormal.z * ray.dir.z;
                        if (normalDotIncoming > 0.0f)
                        {
                            rocket->impactNormal.x = -rocket->impactNormal.x;
                            rocket->impactNormal.y = -rocket->impactNormal.y;
                            rocket->impactNormal.z = -rocket->impactNormal.z;
                        }
                    }
                    else
                    {
                        rocket->impactNormal = {-ray.dir.x, -ray.dir.y, -ray.dir.z};
                    }

                    rocket->impacted = true;
                    rocket->terminated = true;
                    rocket->active = false;
                    rocket->impactTime = 5.0f;
                    rocket->exposureSampleTime = 5.0f;

                    // Build the splash diagnostic once, at the moment the
                    // rocket impacts. This deliberately does not apply damage.
                    TF2Bridge_BuildRocketExplosionDiagnostics(rocket);

                    // BFBB is authoritative for world collision. Tell the
                    // actual TF2 rocket to terminate at this exact impact
                    // point; this is still not an explosion/damage event.
                    iTF2BridgeSendRocketImpact(entIndex, rocket->impact.x,
                                               rocket->impact.y, rocket->impact.z);

                    printf("bfbb: tf2bridge -- rocket %d impact source=(%.2f %.2f %.2f) bfbb=(%.2f %.2f %.2f) radius=%.2f\n",
                        entIndex,
                        (double)in->rocketPos[i][0], (double)in->rocketPos[i][1],
                        (double)in->rocketPos[i][2],
                        (double)rocket->impact.x, (double)rocket->impact.y,
                        (double)rocket->impact.z, (double)rocket->radius);
                }
            }
        }

        rocket->pos = pos;
        rocket->prevPos = pos;
        rocket->hasPrevious = true;
    }

    // A rocket no longer present in the TF2 packet is no longer drawn as a
    // live diagnostic. An impact marker, if one exists, is allowed to finish
    // its configured lifetime.
    for (uint32_t i = 0; i < BRIDGE_MAX_ROCKETS; ++i)
    {
        if (sRocketDebug[i].entIndex != 0 && !seen[i])
        {
            // The entity has disappeared from TF2's projectile list. This is
            // the point at which its diagnostic track may finally be reused.
            sRocketDebug[i] = {};
        }
    }

    // Numeric source/BFBB comparison, throttled so one fast rocket does not
    // flood the console.
    if (count > 0)
    {
        sRocketDebugPrintTimer -= gSceneUpdateTime;
        if (sRocketDebugPrintTimer <= 0.0f)
        {
            const uint32_t i = 0;
            const xVec3 bfbbPos = FromSource(in->rocketPos[i][0], in->rocketPos[i][1],
                                             in->rocketPos[i][2], in->scale);
            printf("bfbb: tf2bridge -- rocket %d source=(%.2f %.2f %.2f) bfbb=(%.3f %.3f %.3f) radius=%.2f\n",
                in->rocketEntIndex[i],
                (double)in->rocketPos[i][0], (double)in->rocketPos[i][1],
                (double)in->rocketPos[i][2],
                (double)bfbbPos.x, (double)bfbbPos.y, (double)bfbbPos.z,
                (double)(in->rocketRadius[i] / in->scale));
            sRocketDebugPrintTimer = 0.25f;
        }
    }
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

    // Temporary combat bridge: prefer the real TF2 weapon-fire event when
    // present, but retain the old attack-edge path as a fallback for weapons
    // that do not yet report through the common weapon bases. A packet with
    // BRIDGE_WEAPON_FIRED is consumed instead of the attack edge, so one TF2
    // attack cannot produce two BFBB hits.
    static uint32_t sLastIntentSeq = 0;
    static bool sLastAttack = false;
    const BridgeIntentPacket* attackIn = iTF2BridgeGetIntent();
    if (playing && attackIn != NULL && attackIn->seq != sLastIntentSeq)
    {
        const bool attack = (attackIn->buttons & BRIDGE_IN_ATTACK) != 0;
        const bool fired = (attackIn->weaponflags & BRIDGE_WEAPON_FIRED) != 0;

        TF2Bridge_ProcessRocketDiagnostics(attackIn);

        if (attackIn->hitscanCount > 0)
        {
            const uint32_t count = attackIn->hitscanCount > BRIDGE_MAX_HITSCAN_RAYS
                ? BRIDGE_MAX_HITSCAN_RAYS : attackIn->hitscanCount;
            sHitscanDebugCount = count;
            sHitscanDebugTime = 2.0f;

            // One diagnostic line per hitscan packet. This compares the exact
            // Source fire point/direction with the BFBB coordinates used by
            // collision and with the camera that BFBB is actually rendering.
            const xVec3 debugOrigin = FromSource(attackIn->hitscanOrigin[0],
                attackIn->hitscanOrigin[1], attackIn->hitscanOrigin[2], attackIn->scale);
            const xVec3 debugDir = FromSource(attackIn->hitscanDir[0][0],
                attackIn->hitscanDir[0][1], attackIn->hitscanDir[0][2], 1.0f);
            const xMat4x3& debugCam = globals.camera.mat;
            printf("bfbb: tf2bridge -- hitscan debug sourceOrigin %.2f %.2f %.2f sourceDir %.3f %.3f %.3f\\n",
                (double)attackIn->hitscanOrigin[0], (double)attackIn->hitscanOrigin[1],
                (double)attackIn->hitscanOrigin[2], (double)attackIn->hitscanDir[0][0],
                (double)attackIn->hitscanDir[0][1], (double)attackIn->hitscanDir[0][2]);
            printf("bfbb: tf2bridge -- hitscan debug bfbbOrigin %.2f %.2f %.2f bfbbDir %.3f %.3f %.3f camera %.2f %.2f %.2f at %.3f %.3f %.3f\\n",
                (double)debugOrigin.x, (double)debugOrigin.y, (double)debugOrigin.z,
                (double)debugDir.x, (double)debugDir.y, (double)debugDir.z,
                (double)debugCam.pos.x, (double)debugCam.pos.y, (double)debugCam.pos.z,
                (double)debugCam.at.x, (double)debugCam.at.y, (double)debugCam.at.z);

            for (uint32_t i = 0; i < count; ++i)
            {
                const xVec3 origin = FromSource(attackIn->hitscanOrigin[0],
                    attackIn->hitscanOrigin[1], attackIn->hitscanOrigin[2], attackIn->scale);
                const xVec3 dir = FromSource(attackIn->hitscanDir[i][0],
                    attackIn->hitscanDir[i][1], attackIn->hitscanDir[i][2], 1.0f);
                sHitscanDebugRays[i].origin = origin;
                sHitscanDebugRays[i].end.x = origin.x + dir.x * (attackIn->hitscanRange / attackIn->scale);
                sHitscanDebugRays[i].end.y = origin.y + dir.y * (attackIn->hitscanRange / attackIn->scale);
                sHitscanDebugRays[i].end.z = origin.z + dir.z * (attackIn->hitscanRange / attackIn->scale);
                TF2Bridge_FireHitscanRay(attackIn, attackIn->hitscanOrigin,
                    attackIn->hitscanDir[i], attackIn->hitscanRange, i);
            }
        }
        else if (fired)
        {
            TF2Bridge_FireAtNPCs(attackIn);
        }
        else if (attack && !sLastAttack)
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
    case NPC_TYPE_HAMSPIN:    case NPC_TYPE_TARTAR:
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

static void TF2Bridge_FireHitscanRay(const BridgeIntentPacket* in, const float sourceOrigin[3],
    const float sourceDir[3], float sourceRange, uint32_t debugIndex)
{
    if (in == NULL || globals.sceneCur == NULL || in->scale <= 0.0f)
        return;

    const xVec3 origin = FromSource(sourceOrigin[0], sourceOrigin[1], sourceOrigin[2], in->scale);
    xVec3 dir = FromSource(sourceDir[0], sourceDir[1], sourceDir[2], 1.0f);

    xRay3 ray;
    ray.origin = origin;
    ray.dir = dir;
    ray.min_t = 0.0f;
    ray.max_t = sourceRange / in->scale;
    ray.flags = XRAY3_USE_MIN | XRAY3_USE_MAX;

    // Let BFBB perform the world trace against the same environment collision
    // it uses for gameplay. On PC levels this is the JSP collision tree, so this
    // is not a second copy of the level geometry and it stays authoritative to
    // BFBB. Keep the normal requested now as groundwork for projectile bounces.
    xCollis worldHit;
    memset(&worldHit, 0, sizeof(worldHit));
    worldHit.flags = k_HIT_0x200;
    const bool hitWorld = iRayHitsEnv(&ray, globals.sceneCur->env, &worldHit) != 0;
    const F32 worldDist = hitWorld ? worldHit.dist : FLOAT_MAX;

    // Make the diagnostic ray stop at the first BFBB world surface. This makes
    // walls/ground immediately visible in the temporary debug renderer.
    if (debugIndex < BRIDGE_MAX_HITSCAN_RAYS && hitWorld)
    {
        sHitscanDebugRays[debugIndex].end.x = origin.x + dir.x * worldDist;
        sHitscanDebugRays[debugIndex].end.y = origin.y + dir.y * worldDist;
        sHitscanDebugRays[debugIndex].end.z = origin.z + dir.z * worldDist;
    }

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

    // The BFBB environment is opaque to a hitscan shot. If the first JSP/world
    // surface is at or before the first NPC bound, the shot stops there.
    if (best == NULL || worldDist <= bestDist)
    {
        if (hitWorld)
        {
            printf("bfbb: tf2bridge -- hitscan ray blocked by world at %.2f mat %u\n",
                (double)worldDist, (unsigned)worldHit.oid);
        }
        else
        {
            printf("bfbb: tf2bridge -- hitscan ray no NPC hit dir %.3f %.3f %.3f range %.1f\n",
                (double)dir.x, (double)dir.y, (double)dir.z, (double)sourceRange);
        }
        return;
    }

    best->Damage(DMGTYP_SIDE, NULL, &dir);
    printf("bfbb: tf2bridge -- hitscan ray hit NPC type %d at %.2f dir %.3f %.3f %.3f\n",
        (int)best->SelfType(), (double)bestDist,
        (double)dir.x, (double)dir.y, (double)dir.z);
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


void zTF2Bridge_DebugRenderRockets()
{
    bool any = false;
    for (uint32_t i = 0; i < BRIDGE_MAX_ROCKETS; ++i)
    {
        if (sRocketDebug[i].active || (sRocketDebug[i].impacted && sRocketDebug[i].impactTime > 0.0f))
        {
            any = true;
            break;
        }
    }

    if (!any)
        return;

    void* oldTexture = NULL;
    void* oldSrcBlend = NULL;
    void* oldDstBlend = NULL;
    void* oldVertexAlpha = NULL;
    void* oldZWrite = NULL;
    void* oldZTest = NULL;
    RwRenderStateGet(rwRENDERSTATETEXTURERASTER, &oldTexture);
    RwRenderStateGet(rwRENDERSTATESRCBLEND, &oldSrcBlend);
    RwRenderStateGet(rwRENDERSTATEDESTBLEND, &oldDstBlend);
    RwRenderStateGet(rwRENDERSTATEVERTEXALPHAENABLE, &oldVertexAlpha);
    RwRenderStateGet(rwRENDERSTATEZWRITEENABLE, &oldZWrite);
    RwRenderStateGet(rwRENDERSTATEZTESTENABLE, &oldZTest);

    RwRenderStateSet(rwRENDERSTATETEXTURERASTER, NULL);
    RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)TRUE);
    RwRenderStateSet(rwRENDERSTATESRCBLEND, (void*)rwBLENDSRCALPHA);
    RwRenderStateSet(rwRENDERSTATEDESTBLEND, (void*)rwBLENDINVSRCALPHA);
    RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)FALSE);
    RwRenderStateSet(rwRENDERSTATEZTESTENABLE, (void*)FALSE);

    const float pi2 = 6.283185307f;
    const float marker = 1.5f;

    // Each rocket gets a small cross while alive. An impacted rocket becomes
    // a persistent impact cross plus three orthogonal radius rings.
    for (uint32_t i = 0; i < BRIDGE_MAX_ROCKETS; ++i)
    {
        TF2BridgeDebugRocket& rocket = sRocketDebug[i];

        if (rocket.sweepTime > 0.0f)
        {
            const float endpointMarker = 0.65f;
            RwIm3DVertex endpoints[12];

            const xVec3& a = rocket.sweepStart;
            const xVec3& b = rocket.sweepEnd;

            RwIm3DVertexSetPos(&endpoints[0], a.x - endpointMarker, a.y, a.z);
            RwIm3DVertexSetPos(&endpoints[1], a.x + endpointMarker, a.y, a.z);
            RwIm3DVertexSetPos(&endpoints[2], a.x, a.y - endpointMarker, a.z);
            RwIm3DVertexSetPos(&endpoints[3], a.x, a.y + endpointMarker, a.z);
            RwIm3DVertexSetPos(&endpoints[4], a.x, a.y, a.z - endpointMarker);
            RwIm3DVertexSetPos(&endpoints[5], a.x, a.y, a.z + endpointMarker);

            RwIm3DVertexSetPos(&endpoints[6], b.x - endpointMarker, b.y, b.z);
            RwIm3DVertexSetPos(&endpoints[7], b.x + endpointMarker, b.y, b.z);
            RwIm3DVertexSetPos(&endpoints[8], b.x, b.y - endpointMarker, b.z);
            RwIm3DVertexSetPos(&endpoints[9], b.x, b.y + endpointMarker, b.z);
            RwIm3DVertexSetPos(&endpoints[10], b.x, b.y, b.z - endpointMarker);
            RwIm3DVertexSetPos(&endpoints[11], b.x, b.y, b.z + endpointMarker);

            const uint8_t cr = rocket.sweepHit ? 255 : 80;
            const uint8_t cg = rocket.sweepHit ? 80 : 220;
            const uint8_t cb = rocket.sweepHit ? 80 : 255;
            for (int v = 0; v < 12; ++v)
                RwIm3DVertexSetRGBA(&endpoints[v], cr, cg, cb, 255);

            if (RwIm3DTransform(endpoints, 12, NULL, rwIM3D_VERTEXXYZ | rwIM3D_VERTEXRGBA) != NULL)
            {
                RwIm3DRenderPrimitive(rwPRIMTYPELINELIST);
                RwIm3DEnd();
            }

            RwIm3DVertex sweep[2];
            RwIm3DVertexSetPos(&sweep[0], rocket.sweepStart.x, rocket.sweepStart.y, rocket.sweepStart.z);
            RwIm3DVertexSetRGBA(&sweep[0],
                rocket.sweepHit ? 255 : 80,
                rocket.sweepHit ? 80 : 220,
                rocket.sweepHit ? 80 : 255, 255);
            RwIm3DVertexSetPos(&sweep[1], rocket.sweepEnd.x, rocket.sweepEnd.y, rocket.sweepEnd.z);
            RwIm3DVertexSetRGBA(&sweep[1],
                rocket.sweepHit ? 255 : 80,
                rocket.sweepHit ? 80 : 220,
                rocket.sweepHit ? 80 : 255, 255);

            if (RwIm3DTransform(sweep, 2, NULL, rwIM3D_VERTEXXYZ | rwIM3D_VERTEXRGBA) != NULL)
            {
                RwIm3DRenderPrimitive(rwPRIMTYPELINELIST);
                RwIm3DEnd();
            }

            rocket.sweepTime -= gSceneUpdateTime;
            if (rocket.sweepTime < 0.0f)
                rocket.sweepTime = 0.0f;
        }

        if (rocket.active && !rocket.impacted)
        {
            RwIm3DVertex verts[6];
            const xVec3& p = rocket.pos;

            RwIm3DVertexSetPos(&verts[0], p.x - marker, p.y, p.z);
            RwIm3DVertexSetRGBA(&verts[0], 255, 220, 0, 255);
            RwIm3DVertexSetPos(&verts[1], p.x + marker, p.y, p.z);
            RwIm3DVertexSetRGBA(&verts[1], 255, 220, 0, 255);
            RwIm3DVertexSetPos(&verts[2], p.x, p.y - marker, p.z);
            RwIm3DVertexSetRGBA(&verts[2], 255, 220, 0, 255);
            RwIm3DVertexSetPos(&verts[3], p.x, p.y + marker, p.z);
            RwIm3DVertexSetRGBA(&verts[3], 255, 220, 0, 255);
            RwIm3DVertexSetPos(&verts[4], p.x, p.y, p.z - marker);
            RwIm3DVertexSetRGBA(&verts[4], 255, 220, 0, 255);
            RwIm3DVertexSetPos(&verts[5], p.x, p.y, p.z + marker);
            RwIm3DVertexSetRGBA(&verts[5], 255, 220, 0, 255);

            if (RwIm3DTransform(verts, 6, NULL, rwIM3D_VERTEXXYZ | rwIM3D_VERTEXRGBA) != NULL)
            {
                RwIm3DRenderPrimitive(rwPRIMTYPELINELIST);
                RwIm3DEnd();
            }
        }

        if (rocket.impacted && rocket.impactTime > 0.0f)
        {
            const xVec3& p = rocket.impact;
            RwIm3DVertex cross[6];

            RwIm3DVertexSetPos(&cross[0], p.x - marker, p.y, p.z);
            RwIm3DVertexSetRGBA(&cross[0], 255, 80, 80, 255);
            RwIm3DVertexSetPos(&cross[1], p.x + marker, p.y, p.z);
            RwIm3DVertexSetRGBA(&cross[1], 255, 80, 80, 255);
            RwIm3DVertexSetPos(&cross[2], p.x, p.y - marker, p.z);
            RwIm3DVertexSetRGBA(&cross[2], 255, 80, 80, 255);
            RwIm3DVertexSetPos(&cross[3], p.x, p.y + marker, p.z);
            RwIm3DVertexSetRGBA(&cross[3], 255, 80, 80, 255);
            RwIm3DVertexSetPos(&cross[4], p.x, p.y, p.z - marker);
            RwIm3DVertexSetRGBA(&cross[4], 255, 80, 80, 255);
            RwIm3DVertexSetPos(&cross[5], p.x, p.y, p.z + marker);
            RwIm3DVertexSetRGBA(&cross[5], 255, 80, 80, 255);

            if (RwIm3DTransform(cross, 6, NULL, rwIM3D_VERTEXXYZ | rwIM3D_VERTEXRGBA) != NULL)
            {
                RwIm3DRenderPrimitive(rwPRIMTYPELINELIST);
                RwIm3DEnd();
            }

            // Exposure sample markers:
            //   green  = visible to the explosion
            //   red    = inside the blast but directly blocked
            //   cyan   = direct ray blocked, but a bounded alternate path exists
            //   yellow = inside the NPC bound but outside the blast radius
            // These deliberately last five seconds so the individual sample
            // distribution can be inspected in screenshots.
            if (rocket.exposureSampleTime > 0.0f)
            {
                const float sampleMarker = 0.18f;
                for (uint32_t s = 0; s < rocket.exposureSampleCount; ++s)
                {
                    const xVec3& q = rocket.exposureSamplePos[s];
                    uint8_t cr = 120, cg = 120, cb = 120;
                    if (rocket.exposureSampleState[s] == 1)
                    {
                        cr = 255; cg = 220; cb = 40;
                    }
                    else if (rocket.exposureSampleState[s] == 2)
                    {
                        cr = 255; cg = 60; cb = 60;
                    }
                    else if (rocket.exposureSampleState[s] == 3)
                    {
                        cr = 60; cg = 255; cb = 80;
                    }
                    else if (rocket.exposureSampleState[s] == 4)
                    {
                        cr = 40; cg = 255; cb = 255;
                    }

                    RwIm3DVertex sampleVerts[6];
                    RwIm3DVertexSetPos(&sampleVerts[0], q.x - sampleMarker, q.y, q.z);
                    RwIm3DVertexSetRGBA(&sampleVerts[0], cr, cg, cb, 255);
                    RwIm3DVertexSetPos(&sampleVerts[1], q.x + sampleMarker, q.y, q.z);
                    RwIm3DVertexSetRGBA(&sampleVerts[1], cr, cg, cb, 255);
                    RwIm3DVertexSetPos(&sampleVerts[2], q.x, q.y - sampleMarker, q.z);
                    RwIm3DVertexSetRGBA(&sampleVerts[2], cr, cg, cb, 255);
                    RwIm3DVertexSetPos(&sampleVerts[3], q.x, q.y + sampleMarker, q.z);
                    RwIm3DVertexSetRGBA(&sampleVerts[3], cr, cg, cb, 255);
                    RwIm3DVertexSetPos(&sampleVerts[4], q.x, q.y, q.z - sampleMarker);
                    RwIm3DVertexSetRGBA(&sampleVerts[4], cr, cg, cb, 255);
                    RwIm3DVertexSetPos(&sampleVerts[5], q.x, q.y, q.z + sampleMarker);
                    RwIm3DVertexSetRGBA(&sampleVerts[5], cr, cg, cb, 255);

                    if (RwIm3DTransform(sampleVerts, 6, NULL,
                                        rwIM3D_VERTEXXYZ | rwIM3D_VERTEXRGBA) != NULL)
                    {
                        RwIm3DRenderPrimitive(rwPRIMTYPELINELIST);
                        RwIm3DEnd();
                    }
                }
            }

            // Explosion target markers: cyan means the NPC is
            // inside the blast radius and visible from the impact; orange
            // means the blast radius reaches it but BFBB world geometry blocks
            // the diagnostic line of sight.
            for (uint32_t t = 0; t < rocket.explosionTargetCount; ++t)
            {
                const TF2BridgeDebugExplosionTarget& target = rocket.explosionTargets[t];
                const xVec3& q = target.pos;
                const float targetMarker = 0.75f;
                const uint8_t r = target.visible ? 80 : 255;
                const uint8_t g = target.visible ? 220 : 150;
                const uint8_t b = target.visible ? 255 : 40;

                RwIm3DVertex targetCross[6];
                RwIm3DVertexSetPos(&targetCross[0], q.x - targetMarker, q.y, q.z);
                RwIm3DVertexSetRGBA(&targetCross[0], r, g, b, 255);
                RwIm3DVertexSetPos(&targetCross[1], q.x + targetMarker, q.y, q.z);
                RwIm3DVertexSetRGBA(&targetCross[1], r, g, b, 255);
                RwIm3DVertexSetPos(&targetCross[2], q.x, q.y - targetMarker, q.z);
                RwIm3DVertexSetRGBA(&targetCross[2], r, g, b, 255);
                RwIm3DVertexSetPos(&targetCross[3], q.x, q.y + targetMarker, q.z);
                RwIm3DVertexSetRGBA(&targetCross[3], r, g, b, 255);
                RwIm3DVertexSetPos(&targetCross[4], q.x, q.y, q.z - targetMarker);
                RwIm3DVertexSetRGBA(&targetCross[4], r, g, b, 255);
                RwIm3DVertexSetPos(&targetCross[5], q.x, q.y, q.z + targetMarker);
                RwIm3DVertexSetRGBA(&targetCross[5], r, g, b, 255);

                if (RwIm3DTransform(targetCross, 6, NULL,
                                    rwIM3D_VERTEXXYZ | rwIM3D_VERTEXRGBA) != NULL)
                {
                    RwIm3DRenderPrimitive(rwPRIMTYPELINELIST);
                    RwIm3DEnd();
                }
            }

            const int segments = 32;
            RwIm3DVertex rings[segments * 6];
            int n = 0;
            for (int s = 0; s < segments; ++s)
            {
                const float a0 = pi2 * (float)s / (float)segments;
                const float a1 = pi2 * (float)(s + 1) / (float)segments;
                const float c0 = cosf(a0);
                const float s0 = sinf(a0);
                const float c1 = cosf(a1);
                const float s1 = sinf(a1);

                // XY plane.
                RwIm3DVertexSetPos(&rings[n], p.x + c0 * rocket.radius,
                                   p.y + s0 * rocket.radius, p.z);
                RwIm3DVertexSetRGBA(&rings[n], 80, 255, 120, 220); n++;
                RwIm3DVertexSetPos(&rings[n], p.x + c1 * rocket.radius,
                                   p.y + s1 * rocket.radius, p.z);                RwIm3DVertexSetRGBA(&rings[n], 80, 255, 120, 220); n++;

                // XZ plane.
                RwIm3DVertexSetPos(&rings[n], p.x + c0 * rocket.radius,
                                   p.y, p.z + s0 * rocket.radius);
                RwIm3DVertexSetRGBA(&rings[n], 80, 255, 120, 220); n++;
                RwIm3DVertexSetPos(&rings[n], p.x + c1 * rocket.radius,
                                   p.y, p.z + s1 * rocket.radius);
                RwIm3DVertexSetRGBA(&rings[n], 80, 255, 120, 220); n++;

                // YZ plane.
                RwIm3DVertexSetPos(&rings[n], p.x, p.y + c0 * rocket.radius,
                                   p.z + s0 * rocket.radius);
                RwIm3DVertexSetRGBA(&rings[n], 80, 255, 120, 220); n++;
                RwIm3DVertexSetPos(&rings[n], p.x, p.y + c1 * rocket.radius,
                                   p.z + s1 * rocket.radius);
                RwIm3DVertexSetRGBA(&rings[n], 80, 255, 120, 220); n++;
            }

            if (RwIm3DTransform(rings, n, NULL, rwIM3D_VERTEXXYZ | rwIM3D_VERTEXRGBA) != NULL)
            {
                RwIm3DRenderPrimitive(rwPRIMTYPELINELIST);
                RwIm3DEnd();
            }

            rocket.impactTime -= gSceneUpdateTime;
            rocket.exposureSampleTime -= gSceneUpdateTime;
            if (rocket.impactTime <= 0.0f)
            {
                rocket.impactTime = 0.0f;
                rocket.impacted = false;
                rocket.active = false;
                rocket.hasPrevious = false;
                // Keep entIndex/terminated until TF2 stops reporting this
                // entity. This prevents a lingering projectile from being
                // mistaken for a fresh trajectory after the marker expires.
            }
        }
    }

    RwRenderStateSet(rwRENDERSTATETEXTURERASTER, oldTexture);
    RwRenderStateSet(rwRENDERSTATESRCBLEND, oldSrcBlend);
    RwRenderStateSet(rwRENDERSTATEDESTBLEND, oldDstBlend);
    RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, oldVertexAlpha);
    RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, oldZWrite);
    RwRenderStateSet(rwRENDERSTATEZTESTENABLE, oldZTest);
}

void zTF2Bridge_DebugRenderHitscan()
{
    if (sHitscanDebugCount == 0)
        return;

    // Diagnostic only. Do not try to make the full 8192-unit ray look pretty
    // yet; first prove that the fire origin and direction agree with BFBB's
    // actual camera. The origin and camera get bright cross markers, and the
    // first ray gets a short, easy-to-see direction stub.
    const TF2BridgeDebugRay& ray = sHitscanDebugRays[0];
    const xMat4x3& cam = globals.camera.mat;

    void* oldTexture = NULL;
    void* oldSrcBlend = NULL;
    void* oldDstBlend = NULL;
    void* oldVertexAlpha = NULL;
    void* oldZWrite = NULL;
    void* oldZTest = NULL;
    RwRenderStateGet(rwRENDERSTATETEXTURERASTER, &oldTexture);
    RwRenderStateGet(rwRENDERSTATESRCBLEND, &oldSrcBlend);
    RwRenderStateGet(rwRENDERSTATEDESTBLEND, &oldDstBlend);
    RwRenderStateGet(rwRENDERSTATEVERTEXALPHAENABLE, &oldVertexAlpha);
    RwRenderStateGet(rwRENDERSTATEZWRITEENABLE, &oldZWrite);
    RwRenderStateGet(rwRENDERSTATEZTESTENABLE, &oldZTest);

    RwRenderStateSet(rwRENDERSTATETEXTURERASTER, NULL);
    RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)TRUE);
    RwRenderStateSet(rwRENDERSTATESRCBLEND, (void*)rwBLENDSRCALPHA);
    RwRenderStateSet(rwRENDERSTATEDESTBLEND, (void*)rwBLENDINVSRCALPHA);
    RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)FALSE);
    RwRenderStateSet(rwRENDERSTATEZTESTENABLE, (void*)FALSE);

    // Three visual tests:
    //   1. yellow cross = exact TF2 fire origin after Source -> BFBB mapping
    //   2. cyan cross   = BFBB camera position currently used for rendering
    //   3. red stub    = first TF2 ray direction, only 8 BFBB units long
    //
    // A fourth, longer yellow line connects camera -> fire origin. This lets us
    // immediately see whether the Source fire point is merely a muzzle/eye
    // offset rather than being wildly displaced.
    const float markerOrigin = 2.5f;
    const float markerCamera = 2.5f;
    const float stubLength = 20.0f;

    RwIm3DVertex verts[20];
    int n = 0;

    const xVec3 o = ray.origin;
    const xVec3 c = cam.pos;

    const float vx = ray.end.x - o.x;
    const float vy = ray.end.y - o.y;
    const float vz = ray.end.z - o.z;
    const float len = sqrtf(vx * vx + vy * vy + vz * vz);
    const float invLen = len > 0.000001f ? 1.0f / len : 0.0f;

    const xVec3 d = {
        o.x + vx * stubLength * invLen,
        o.y + vy * stubLength * invLen,
        o.z + vz * stubLength * invLen
    };

    // The current BFBB camera is rebuilt from the TF2 eye every frame.

    // Origin cross: yellow.
    RwIm3DVertexSetPos(&verts[n], o.x - markerOrigin, o.y, o.z);
    RwIm3DVertexSetRGBA(&verts[n], 255, 255, 0, 255); n++;
    RwIm3DVertexSetPos(&verts[n], o.x + markerOrigin, o.y, o.z);
    RwIm3DVertexSetRGBA(&verts[n], 255, 255, 0, 255); n++;
    RwIm3DVertexSetPos(&verts[n], o.x, o.y - markerOrigin, o.z);
    RwIm3DVertexSetRGBA(&verts[n], 255, 255, 0, 255); n++;
    RwIm3DVertexSetPos(&verts[n], o.x, o.y + markerOrigin, o.z);
    RwIm3DVertexSetRGBA(&verts[n], 255, 255, 0, 255); n++;
    RwIm3DVertexSetPos(&verts[n], o.x, o.y, o.z - markerOrigin);
    RwIm3DVertexSetRGBA(&verts[n], 255, 255, 0, 255); n++;
    RwIm3DVertexSetPos(&verts[n], o.x, o.y, o.z + markerOrigin);
    RwIm3DVertexSetRGBA(&verts[n], 255, 255, 0, 255); n++;

    // Camera cross: cyan.
    RwIm3DVertexSetPos(&verts[n], c.x - markerCamera, c.y, c.z);
    RwIm3DVertexSetRGBA(&verts[n], 0, 255, 255, 255); n++;
    RwIm3DVertexSetPos(&verts[n], c.x + markerCamera, c.y, c.z);
    RwIm3DVertexSetRGBA(&verts[n], 0, 255, 255, 255); n++;
    RwIm3DVertexSetPos(&verts[n], c.x, c.y - markerCamera, c.z);
    RwIm3DVertexSetRGBA(&verts[n], 0, 255, 255, 255); n++;
    RwIm3DVertexSetPos(&verts[n], c.x, c.y + markerCamera, c.z);
    RwIm3DVertexSetRGBA(&verts[n], 0, 255, 255, 255); n++;
    RwIm3DVertexSetPos(&verts[n], c.x, c.y, c.z - markerCamera);
    RwIm3DVertexSetRGBA(&verts[n], 0, 255, 255, 255); n++;
    RwIm3DVertexSetPos(&verts[n], c.x, c.y, c.z + markerCamera);
    RwIm3DVertexSetRGBA(&verts[n], 0, 255, 255, 255); n++;

    // Camera -> fire-origin connector: magenta.
    RwIm3DVertexSetPos(&verts[n], c.x, c.y, c.z);
    RwIm3DVertexSetRGBA(&verts[n], 255, 0, 255, 255); n++;
    RwIm3DVertexSetPos(&verts[n], o.x, o.y, o.z);
    RwIm3DVertexSetRGBA(&verts[n], 255, 0, 255, 255); n++;

    // Full hitscan ray: red. The endpoint is the actual 8192-Source-unit
    // endpoint (about 204.8 BFBB units at the current scale of 40).
    RwIm3DVertexSetPos(&verts[n], o.x, o.y, o.z);
    RwIm3DVertexSetRGBA(&verts[n], 255, 0, 0, 255); n++;
    RwIm3DVertexSetPos(&verts[n], ray.end.x, ray.end.y, ray.end.z);
    RwIm3DVertexSetRGBA(&verts[n], 255, 0, 0, 255); n++;

    if (RwIm3DTransform(verts, n, NULL, rwIM3D_VERTEXXYZ | rwIM3D_VERTEXRGBA) != NULL)
    {
        RwIm3DRenderPrimitive(rwPRIMTYPELINELIST);
        RwIm3DEnd();
    }

    RwRenderStateSet(rwRENDERSTATETEXTURERASTER, oldTexture);
    RwRenderStateSet(rwRENDERSTATESRCBLEND, oldSrcBlend);
    RwRenderStateSet(rwRENDERSTATEDESTBLEND, oldDstBlend);
    RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, oldVertexAlpha);
    RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, oldZWrite);
    RwRenderStateSet(rwRENDERSTATEZTESTENABLE, oldZTest);

    if (sHitscanDebugTime > 0.0f)
    {
        sHitscanDebugTime -= gSceneUpdateTime;
        if (sHitscanDebugTime <= 0.0f)
        {
            sHitscanDebugTime = 0.0f;
            sHitscanDebugCount = 0;
        }
    }
}