#include "zTF2Bridge.h"
#include "zTF2Splash.h"

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

static void* sPendingKnockbackTarget = NULL;
static float sPendingKnockbackDirX = 0.0f;
static float sPendingKnockbackDirZ = 1.0f;
static float sPendingKnockbackMultiplier = 1.0f;
static bool sPendingKnockbackOverride = false;

int zTF2Bridge_ConsumeKnockbackOverride(void* npc, float* dirX, float* dirZ, float* multiplier)
{
    if (!sPendingKnockbackOverride || sPendingKnockbackTarget != npc)
        return 0;

    if (dirX != NULL) *dirX = sPendingKnockbackDirX;
    if (dirZ != NULL) *dirZ = sPendingKnockbackDirZ;
    if (multiplier != NULL) *multiplier = sPendingKnockbackMultiplier;

    sPendingKnockbackTarget = NULL;
    sPendingKnockbackOverride = false;
    return 1;
}

struct TF2BridgeDebugExplosionTarget
{
    xVec3 pos;
    float distance;
    int32_t npcType;
    bool visible;
};

struct TF2BridgeDebugRocket
{
    static const uint32_t kMaxExposureSamples = 64;

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
    xVec3 incoming; // unit travel direction of the move that hit
    xVec3 explosionOrigin; // impact pulled off the surface; the blast is centred here
    float debugTime; // how long diagnostic markers stay up (bfbb_rocket_debug_lifetime)
    uint32_t explosionTargetCount;
    TF2BridgeDebugExplosionTarget explosionTargets[BRIDGE_MAX_ROCKETS];

    // Per-sample exposure visualization:
    // 1 = outside blast, 2 = blocked, 3 = straight line, 4 = reached by bending around geometry.
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
        if (sRocketDebug[i].entIndex == 0 && sRocketDebug[i].impactTime <= 0.0f &&
            sRocketDebug[i].sweepTime <= 0.0f && sRocketDebug[i].exposureSampleTime <= 0.0f &&
            freeTrack == NULL)
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

// ---------------------------------------------------------------------------
// Rocket explosion, TF2 rules (see zTF2Splash.h for the rules and what is added).
//
// Tunables, all optional environment variables:
//   BFBB_TF2BRIDGE_ROCKET_APPLY_DAMAGE   0 = compute and draw only (default 1)
//   BFBB_TF2BRIDGE_SPLASH_WRAP           longest bent blast path as a multiple of
//                                        the radius; 0 = pure TF2, no bending
//                                        (default 1.6)
//   BFBB_TF2BRIDGE_ROCKET_VERBOSE        1 = also print every sweep miss
//   BFBB_TF2BRIDGE_LEGACY_ROCKET_HITSCAN 1 = a fired rocket also does the old
//                                        instant crosshair hit (default 0)
static bool TF2Bridge_RocketApplyDamage()
{
    const char* value = getenv("BFBB_TF2BRIDGE_ROCKET_APPLY_DAMAGE");
    if (value != NULL && value[0] != '\0')
        return atoi(value) != 0;

    return true;
}

static F32 TF2Bridge_SplashWrapScale()
{
    const char* value = getenv("BFBB_TF2BRIDGE_SPLASH_WRAP");
    if (value != NULL && value[0] != '\0')
    {
        const F32 v = (F32)atof(value);
        return v < 0.0f ? 0.0f : v;
    }

    return 1.6f;
}

static bool TF2Bridge_EnvFlag(const char* name)
{
    const char* value = getenv(name);
    return value != NULL && value[0] != '\0' && atoi(value) != 0;
}

static tf2splash::V3 ToSplash(const xVec3& v)
{
    return tf2splash::Make(v.x, v.y, v.z);
}

// tf2splash callback: is the straight segment from..to blocked by the level?
// Only the world (JSP) counts, like TF2's MASK_RADIUS_DAMAGE ignoring players
// and projectiles.
static bool TF2Bridge_SplashBlocked(void*, const tf2splash::V3& from, const tf2splash::V3& to)
{
    const F32 dx = to.x - from.x;
    const F32 dy = to.y - from.y;
    const F32 dz = to.z - from.z;
    const F32 len = sqrtf(dx * dx + dy * dy + dz * dz);
    if (len < 0.0005f)
        return false;

    xRay3 ray;
    ray.origin.x = from.x;
    ray.origin.y = from.y;
    ray.origin.z = from.z;
    ray.dir.x = dx / len;
    ray.dir.y = dy / len;
    ray.dir.z = dz / len;
    ray.min_t = 0.0f;
    ray.max_t = len;
    ray.flags = XRAY3_USE_MIN | XRAY3_USE_MAX;

    xCollis hit;
    memset(&hit, 0, sizeof(hit));
    return iRayHitsEnv(&ray, globals.sceneCur->env, &hit) != 0;
}

// What a rocket's last move ran into.
struct TF2BridgeSweepHit
{
    F32 dist;
    xVec3 normal; // unit, facing back toward where the rocket came from
    void* ent; // the entity struck, or NULL for level geometry
};

// Sweeps one rocket move. worldOnly skips entities (used for the first move, which
// starts at the player's eye and must not hit the player or his own muzzle area).
static bool TF2Bridge_SweepRocket(const xVec3& from, const xVec3& to, bool worldOnly,
                                  TF2BridgeSweepHit* out)
{
    const F32 dx = to.x - from.x;
    const F32 dy = to.y - from.y;
    const F32 dz = to.z - from.z;
    const F32 len = sqrtf(dx * dx + dy * dy + dz * dz);
    if (len < 0.0001f)
        return false;

    xRay3 ray;
    ray.origin = from;
    ray.dir.x = dx / len;
    ray.dir.y = dy / len;
    ray.dir.z = dz / len;
    ray.min_t = 0.0f;
    ray.max_t = len;
    ray.flags = XRAY3_USE_MIN | XRAY3_USE_MAX;

    // World hit WITH its triangle normal. xRayHitsScene cannot provide it: it
    // queries the environment into an uninitialised local xCollis, never asks for
    // the normal, and does not copy one back, so every world normal it returned
    // was zero. That made the old splash origin fall back to "back along the
    // rocket's path", which sits almost inside the surface on a grazing hit.
    xCollis envHit;
    memset(&envHit, 0, sizeof(envHit));
    envHit.flags = k_HIT_0x200;
    const bool hitEnv = iRayHitsEnv(&ray, globals.sceneCur->env, &envHit) != 0 &&
                        envHit.dist >= 0.0f && envHit.dist <= len;

    F32 dist = hitEnv ? envHit.dist : FLOAT_MAX;
    void* ent = NULL;

    if (!worldOnly)
    {
        xCollis sceneHit;
        memset(&sceneHit, 0, sizeof(sceneHit));
        sceneHit.flags = k_HIT_0x200;
        xRayHitsScene(globals.sceneCur, &ray, &sceneHit);

        if ((sceneHit.flags & k_HIT_IT) != 0 && sceneHit.optr != NULL && sceneHit.dist >= 0.0f &&
            sceneHit.dist <= len && sceneHit.dist < dist)
        {
            dist = sceneHit.dist;
            ent = sceneHit.optr;
        }
    }

    if (dist == FLOAT_MAX)
        return false;

    xVec3 n = { -ray.dir.x, -ray.dir.y, -ray.dir.z };
    if (ent == NULL)
    {
        const F32 nl = sqrtf(envHit.norm.x * envHit.norm.x + envHit.norm.y * envHit.norm.y +
                             envHit.norm.z * envHit.norm.z);
        if (nl > 0.5f)
        {
            n.x = envHit.norm.x / nl;
            n.y = envHit.norm.y / nl;
            n.z = envHit.norm.z / nl;
            if (n.x * ray.dir.x + n.y * ray.dir.y + n.z * ray.dir.z > 0.0f)
            {
                n.x = -n.x;
                n.y = -n.y;
                n.z = -n.z;
            }
        }
    }
    else
    {
        // Hit an entity: push the explosion out along centre -> impact.
        const xBound& bound = ((xEnt*)ent)->bound;
        const xVec3* c = xBoundCenter(&bound);
        const F32 hx = from.x + ray.dir.x * dist - c->x;
        const F32 hy = from.y + ray.dir.y * dist - c->y;
        const F32 hz = from.z + ray.dir.z * dist - c->z;
        const F32 hl = sqrtf(hx * hx + hy * hy + hz * hz);
        if (hl > 0.0001f)
        {
            n.x = hx / hl;
            n.y = hy / hl;
            n.z = hz / hl;
        }
    }

    out->dist = dist;
    out->normal = n;
    out->ent = ent;
    return true;
}

// Detonates a rocket at rocket->impact with TF2's rules.
//   directEnt: the entity the rocket struck, if any. TF2 gives that one full
//   damage with no line-of-sight test (CTFRadiusDamageInfo::ApplyToEntity).
static void TF2Bridge_RocketExplode(TF2BridgeDebugRocket* rocket, void* directEnt)
{
    if (rocket == NULL || globals.sceneCur == NULL)
        return;

    rocket->explosionTargetCount = 0;
    rocket->exposureSampleCount = 0;
    rocket->exposureSampleTime = rocket->debugTime;

    st_XORDEREDARRAY* npclist = zNPCMgr_GetNPCList();
    if (npclist == NULL)
        return;

    const F32 baseDamage = rocket->damage;
    const F32 radius = rocket->radius;
    const bool applyDamage = TF2Bridge_RocketApplyDamage();
    const F32 wrapScale = TF2Bridge_SplashWrapScale();
    const xVec3 origin = rocket->explosionOrigin;

    tf2splash::Explosion boom(ToSplash(origin), ToSplash(rocket->impactNormal), radius, wrapScale,
                              TF2Bridge_SplashBlocked, NULL);

    for (S32 i = 0; i < npclist->cnt; ++i)
    {
        zNPCCommon* npc = (zNPCCommon*)npclist->list[i];
        if (!IsTF2BridgeRobot(npc) || !npc->IsAlive())
            continue;

        const xVec3* center = xBoundCenter(&npc->bound);
        if (center == NULL)
            continue;

        // TF2 candidate test: the nearest point of the bound must be within the radius.
        xSphere blastSphere;
        blastSphere.center = origin;
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

        const bool direct = directEnt != NULL && (void*)static_cast<xEnt*>(npc) == directEnt;

        // Body points: centre first (TF2's BodyTarget), then lower and upper, then
        // four around the middle. Only the first three are tried for a bent path.
        xBox box;
        xBoundGetBox(box, npc->bound);
        const F32 cx = center->x, cy = center->y, cz = center->z;
        const F32 ex = 0.5f * (box.upper.x - box.lower.x);
        const F32 ey = 0.5f * (box.upper.y - box.lower.y);
        const F32 ez = 0.5f * (box.upper.z - box.lower.z);
        const S32 kPoints = 7;
        tf2splash::V3 pts[7] = {
            tf2splash::Make(cx, cy, cz),
            tf2splash::Make(cx, cy - 0.7f * ey, cz),
            tf2splash::Make(cx, cy + 0.7f * ey, cz),
            tf2splash::Make(cx + 0.4f * ex, cy, cz),
            tf2splash::Make(cx - 0.4f * ex, cy, cz),
            tf2splash::Make(cx, cy, cz + 0.4f * ez),
            tf2splash::Make(cx, cy, cz - 0.4f * ez),
        };

        uint8_t states[7] = {};
        F32 reach;
        bool wrapped = false;

        if (direct)
        {
            for (S32 k = 0; k < kPoints; ++k)
                states[k] = tf2splash::kStateVisible;
            reach = 0.0f;
        }
        else
        {
            reach = boom.Evaluate(pts, kPoints, 3, states);
            wrapped = boom.LastWasWrapped();
        }

        for (S32 k = 0; k < kPoints; ++k)
        {
            const uint32_t idx = rocket->exposureSampleCount;
            if (idx >= TF2BridgeDebugRocket::kMaxExposureSamples)
                break;
            rocket->exposureSamplePos[idx].x = pts[k].x;
            rocket->exposureSamplePos[idx].y = pts[k].y;
            rocket->exposureSamplePos[idx].z = pts[k].z;
            rocket->exposureSampleState[idx] = states[k];
            rocket->exposureSampleCount = idx + 1;
        }

        if (reach < 0.0f)
        {
            printf("bfbb: tf2bridge -- rocket splash type %d BLOCKED (no clear path inside radius %.2f) "
                   "wrap=%.2f damage=0\n",
                   (int)target.npcType, (double)radius, (double)wrapScale);
            continue;
        }

        target.visible = true;
        target.distance = reach;

        const F32 scale = tf2splash::RocketFalloffScale(reach, radius);
        const F32 damage = baseDamage * scale;

        // BFBB reads the vector as the direction the hit travelled; the knock goal
        // sends the robot along it (horizontally). Blast direction = origin -> robot.
        // (The old code passed the impact POSITION here, so knockback went in a
        // direction that depended on where in the level the rocket landed.)
        xVec3 push = { center->x - origin.x, center->y - origin.y, center->z - origin.z };
        F32 pl = sqrtf(push.x * push.x + push.y * push.y + push.z * push.z);
        if (pl < 0.001f || (push.x * push.x + push.z * push.z) < 0.0004f * pl * pl)
        {
            push = rocket->incoming; // on top of / under the blast: use the rocket's heading
            pl = 1.0f;
        }
        push.x /= pl;
        push.y /= pl;
        push.z /= pl;

        if (damage > 0.0f && applyDamage)
            npc->Damage(DMGTYP_SIDE, NULL, &push);

        printf("bfbb: tf2bridge -- rocket splash type %d %s dist=%.2f/%.2f scale=%.2f damage=%.1f "
               "applyDamage=%d\n",
               (int)target.npcType, direct ? "DIRECT" : (wrapped ? "VISIBLE(wrapped)" : "VISIBLE"),
               (double)reach, (double)radius, (double)scale, (double)damage, applyDamage ? 1 : 0);
    }

    printf("bfbb: tf2bridge -- rocket explosion radius=%.2f baseDamage=%.2f wrap=%.2f applyDamage=%d "
           "targets=%u waypoints=%d\n",
           (double)radius, (double)baseDamage, (double)wrapScale, applyDamage ? 1 : 0,
           (unsigned)rocket->explosionTargetCount, boom.WaypointCount());
}

static void TF2Bridge_ProcessRockets(const BridgeIntentPacket* in)
{
    if (in == NULL || globals.sceneCur == NULL || in->scale <= 0.0f)
        return;

    const bool verbose = TF2Bridge_EnvFlag("BFBB_TF2BRIDGE_ROCKET_VERBOSE");
    const F32 debugTime = in->rocketDebugLifetime > 0.0f ? in->rocketDebugLifetime : 0.0f;

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
        rocket->debugTime = debugTime;

        // Already exploded in BFBB. TF2 may not have removed the entity yet (or the
        // impact datagram was lost), so keep telling it until it is gone. The track
        // itself is only recycled once the entity leaves the packet (below). The
        // old code restarted the track when its marker timer ran out, which could
        // explode a rocket twice.
        if (rocket->terminated)
        {
            iTF2BridgeSendRocketImpact(entIndex, rocket->impact.x, rocket->impact.y, rocket->impact.z);
            continue;
        }

        rocket->active = true;

        // First sighting: TF2 spawns the rocket at the muzzle, ahead of the eye. Start
        // this first move at the eye so a player pressed against a wall (muzzle behind
        // it) still hits that wall. World only, so SpongeBob's own bound is ignored.
        bool worldOnly = false;
        xVec3 from = rocket->prevPos;
        if (!rocket->hasPrevious)
        {
            from = FromSource(in->ex, in->ey, in->ez, in->scale);
            worldOnly = true;
        }

        rocket->sweepStart = from;
        rocket->sweepEnd = pos;
        rocket->sweepTime = debugTime;
        rocket->sweepHit = false;

        TF2BridgeSweepHit sweep;
        if (TF2Bridge_SweepRocket(from, pos, worldOnly, &sweep))
        {
            const F32 dx = pos.x - from.x, dy = pos.y - from.y, dz = pos.z - from.z;
            const F32 len = sqrtf(dx * dx + dy * dy + dz * dz);
            rocket->incoming.x = dx / len;
            rocket->incoming.y = dy / len;
            rocket->incoming.z = dz / len;

            rocket->impact.x = from.x + rocket->incoming.x * sweep.dist;
            rocket->impact.y = from.y + rocket->incoming.y * sweep.dist;
            rocket->impact.z = from.z + rocket->incoming.z * sweep.dist;
            rocket->impactNormal = sweep.normal;

            // TF2 (CTFBaseRocket::Explode) pulls out of the surface by 1 Source unit.
            const F32 pull = 1.0f / in->scale;
            rocket->explosionOrigin.x = rocket->impact.x + sweep.normal.x * pull;
            rocket->explosionOrigin.y = rocket->impact.y + sweep.normal.y * pull;
            rocket->explosionOrigin.z = rocket->impact.z + sweep.normal.z * pull;

            rocket->sweepHit = true;
            rocket->impacted = true;
            rocket->terminated = true;
            rocket->active = false;
            rocket->impactTime = debugTime;

            TF2Bridge_RocketExplode(rocket, sweep.ent);

            iTF2BridgeSendRocketImpact(entIndex, rocket->impact.x, rocket->impact.y, rocket->impact.z);

            printf("bfbb: tf2bridge -- rocket %d impact %s source=(%.2f %.2f %.2f) bfbb=(%.2f %.2f %.2f) "
                   "normal=(%.2f %.2f %.2f) radius=%.2f\n",
                   entIndex, sweep.ent != NULL ? "ENTITY" : "world",
                   (double)in->rocketPos[i][0], (double)in->rocketPos[i][1], (double)in->rocketPos[i][2],
                   (double)rocket->impact.x, (double)rocket->impact.y, (double)rocket->impact.z,
                   (double)sweep.normal.x, (double)sweep.normal.y, (double)sweep.normal.z,
                   (double)rocket->radius);
        }
        else if (verbose)
        {
            printf("bfbb: tf2bridge -- rocket %d sweep clear (%.2f %.2f %.2f)->(%.2f %.2f %.2f)\n",
                   entIndex, (double)from.x, (double)from.y, (double)from.z, (double)pos.x,
                   (double)pos.y, (double)pos.z);
        }

        rocket->pos = pos;
        rocket->prevPos = pos;
        rocket->hasPrevious = true;
    }

    // Recycle the track of any rocket TF2 no longer reports. An impact marker that
    // is still being drawn is left to finish its lifetime.
    for (uint32_t i = 0; i < BRIDGE_MAX_ROCKETS; ++i)
    {
        if (sRocketDebug[i].entIndex != 0 && !seen[i])
        {
            sRocketDebug[i].active = false;
            sRocketDebug[i].entIndex = 0;
            sRocketDebug[i].hasPrevious = false;
            sRocketDebug[i].terminated = false;
            sRocketDebug[i].impacted = sRocketDebug[i].impactTime > 0.0f;
        }
    }
}

// One weapon-fire event from TF2: a hitscan volley, a melee swing, or a gun shot.
// Every such packet is queued by the platform layer and handled here exactly once
// (the old code only looked at the newest packet each frame and silently dropped
// the shots in any older one).
static bool sLastAttack = false;

static void TF2Bridge_ProcessWeaponEvent(const BridgeIntentPacket* ev)
{
    const bool melee = (ev->weaponflags & BRIDGE_WEAPON_MELEE) != 0;
    const bool fired = (ev->weaponflags & BRIDGE_WEAPON_FIRED) != 0;
    // A fired rocket is a real projectile now; it damages on impact. It must not
    // also do the old instant "ray at the crosshair" stand-in, which ignored walls.
    const bool rocketGun = (ev->weaponflags & BRIDGE_WEAPON_ROCKET) != 0 &&
                           !TF2Bridge_EnvFlag("BFBB_TF2BRIDGE_LEGACY_ROCKET_HITSCAN");

    if (ev->hitscanCount > 0)
    {
        const uint32_t count = ev->hitscanCount > BRIDGE_MAX_HITSCAN_RAYS
            ? BRIDGE_MAX_HITSCAN_RAYS : ev->hitscanCount;
        sHitscanDebugCount = count;
        sHitscanDebugTime = 2.0f;

        // One diagnostic line per hitscan packet. This compares the exact
        // Source fire point/direction with the BFBB coordinates used by
        // collision and with the camera that BFBB is actually rendering.
        const xVec3 debugOrigin = FromSource(ev->hitscanOrigin[0],
            ev->hitscanOrigin[1], ev->hitscanOrigin[2], ev->scale);
        const xVec3 debugDir = FromSource(ev->hitscanDir[0][0],
            ev->hitscanDir[0][1], ev->hitscanDir[0][2], 1.0f);
        const xMat4x3& debugCam = globals.camera.mat;
        printf("bfbb: tf2bridge -- hitscan debug sourceOrigin %.2f %.2f %.2f sourceDir %.3f %.3f %.3f\\n",
            (double)ev->hitscanOrigin[0], (double)ev->hitscanOrigin[1],
            (double)ev->hitscanOrigin[2], (double)ev->hitscanDir[0][0],
            (double)ev->hitscanDir[0][1], (double)ev->hitscanDir[0][2]);
        printf("bfbb: tf2bridge -- hitscan debug bfbbOrigin %.2f %.2f %.2f bfbbDir %.3f %.3f %.3f camera %.2f %.2f %.2f at %.3f %.3f %.3f\\n",
            (double)debugOrigin.x, (double)debugOrigin.y, (double)debugOrigin.z,
            (double)debugDir.x, (double)debugDir.y, (double)debugDir.z,
            (double)debugCam.pos.x, (double)debugCam.pos.y, (double)debugCam.pos.z,
            (double)debugCam.at.x, (double)debugCam.at.y, (double)debugCam.at.z);

        for (uint32_t i = 0; i < count; ++i)
        {
            const xVec3 origin = FromSource(ev->hitscanOrigin[0],
                ev->hitscanOrigin[1], ev->hitscanOrigin[2], ev->scale);
            const xVec3 dir = FromSource(ev->hitscanDir[i][0],
                ev->hitscanDir[i][1], ev->hitscanDir[i][2], 1.0f);
            sHitscanDebugRays[i].origin = origin;
            sHitscanDebugRays[i].end.x = origin.x + dir.x * (ev->hitscanRange / ev->scale);
            sHitscanDebugRays[i].end.y = origin.y + dir.y * (ev->hitscanRange / ev->scale);
            sHitscanDebugRays[i].end.z = origin.z + dir.z * (ev->hitscanRange / ev->scale);
            TF2Bridge_FireHitscanRay(ev, ev->hitscanOrigin,
                ev->hitscanDir[i], ev->hitscanRange, i);
        }
    }
    else if (fired && !rocketGun)
    {
        TF2Bridge_FireAtNPCs(ev);
    }
    (void)melee;

    sLastAttack = (ev->buttons & BRIDGE_IN_ATTACK) != 0;
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
    // Weapon events first, oldest to newest.
    BridgeIntentPacket ev;
    while (iTF2BridgePopEvent(&ev))
    {
        if (playing)
        {
            TF2Bridge_ProcessWeaponEvent(&ev);
        }
    }

    static uint32_t sLastIntentSeq = 0;
    const BridgeIntentPacket* attackIn = iTF2BridgeGetIntent();
    if (playing && attackIn != NULL && attackIn->seq != sLastIntentSeq)
    {
        const bool attack = (attackIn->buttons & BRIDGE_IN_ATTACK) != 0;
        const bool event = attackIn->hitscanCount > 0 || (attackIn->weaponflags & BRIDGE_WEAPON_FIRED) != 0;
        const bool rocketGun = (attackIn->weaponflags & BRIDGE_WEAPON_ROCKET) != 0 &&
                               !TF2Bridge_EnvFlag("BFBB_TF2BRIDGE_LEGACY_ROCKET_HITSCAN");

        TF2Bridge_ProcessRockets(attackIn);

        // Fallback for weapons that never report a fire event: the attack edge.
        // (Events, including this packet if it is one, were handled above.)
        if (!event && !rocketGun && attack && !sLastAttack)
        {
            TF2Bridge_FireAtNPCs(attackIn);
        }
        if (!event)
        {
            sLastAttack = attack;
        }
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

    const bool melee = (in->weaponflags & BRIDGE_WEAPON_MELEE) != 0;

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

    // The first knockback prototype is intentionally melee-only. Preserve
    // BFBB's damage/death path, but let the native knockback goal consume an
    // explicit horizontal push direction and a tunable impulse multiplier.
    // Raw Source force is not directly comparable to BFBB velocity; calibrate
    // this controlled multiplier before forwarding raw force values.
    sPendingKnockbackOverride = false;
    sPendingKnockbackTarget = NULL;
    // Offer the same calibrated override to every robot the bridge can
    // damage. Only NPCs whose native damage handling enters zNPCGoalKnock
    // will consume it; custom wound/launch goals remain unchanged for now.
    if (melee && IsTF2BridgeRobot(best))
    {
        float pushLen = sqrtf(dir.x * dir.x + dir.z * dir.z);
        if (pushLen < 0.001f)
        {
            sPendingKnockbackDirX = 0.0f;
            sPendingKnockbackDirZ = 1.0f;
        }
        else
        {
            sPendingKnockbackDirX = dir.x / pushLen;
            sPendingKnockbackDirZ = dir.z / pushLen;
        }

        sPendingKnockbackMultiplier = 1.0f;
        const char* forceScale = getenv("BFBB_TF2BRIDGE_MELEE_KNOCKBACK_MULT");
        if (forceScale != NULL && forceScale[0] != '\0')
        {
            const float requested = (float)atof(forceScale);
            if (requested >= 0.1f && requested <= 5.0f)
                sPendingKnockbackMultiplier = requested;
        }

        sPendingKnockbackTarget = best;
        sPendingKnockbackOverride = true;
    }

    // BFBB's normal damage path still owns HP, damage goal, death animation,
    // rewards, and native collision/bounce handling.
    best->Damage(DMGTYP_SIDE, NULL, &dir);
    if (melee && IsTF2BridgeRobot(best))
        printf("bfbb: tf2bridge -- melee knockback offered type=%d target=%p mult=%.2f\n",
               (int)best->SelfType(), (void*)best,
               (double)sPendingKnockbackMultiplier);
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
            //   green = visible to the explosion
            //   cyan  = reached by bending around geometry (wrap)
            //   red   = inside the blast but blocked by world geometry
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
                        cr = 60; cg = 200; cb = 255; // reached by bending around geometry
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
