#include "zTF2Bridge.h"
#include "zTF2Splash.h"

#include "iTF2Bridge.h"
#include "iCamera.h"
#include "iEnv.h"
#include "xClumpColl.h"
#include "iCollide.h"
#include "iModel.h"
#include "xBound.h"
#include "xEnt.h"
#include "xEnv.h"
#include "xJSP.h"
#include "xMemMgr.h"
#include "zBase.h"
#include "zCamera.h"
#include "zEntPlayer.h"
#include "zGameState.h"
#include "zGlobals.h"
#include "zScene.h"
#include "zTalkBox.h"
#include "zNPCMgr.h"
#include "zNPCTypeCommon.h"
#include "zNPCTypeRobot.h"
#include "xBound.h"
#include "xScene.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#include <sys/timeb.h>
#endif

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
static void TF2Bridge_ProcessNpcHits();
// Defined with the hit windows, which it needs the clock of; called from SendState.
static void TF2Bridge_AuditFrame(uint32_t sceneId, bool playing);
// The session log's per-frame half: level changes, the position timeline, and the
// trail of level entities the player walked past. Defined with the logger itself.
static void TF2Bridge_SessionLogFrame(bool playing);
// One line into the same log, from anywhere in the frame.
static void TF2Bridge_Logf(const char* kind, const char* fmt, ...);
// 'TRIG', 'PLAT', ... Defined with the audit, which also names them.
static const char* TF2Bridge_BaseTypeName(int t);

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
static float sPendingKnockbackDirY = 0.0f;
static float sPendingKnockbackMultiplier = 1.0f;
static bool sPendingKnockbackOverride = false;

int zTF2Bridge_ConsumeKnockbackOverride(void* npc, float* dirX, float* dirY, float* dirZ, float* multiplier)
{
    if (!sPendingKnockbackOverride || sPendingKnockbackTarget != npc)
        return 0;

    if (dirX != NULL) *dirX = sPendingKnockbackDirX;
    if (dirY != NULL) *dirY = sPendingKnockbackDirY;
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
    xVec3 attackerPos;
    float sourceUnitScale;
    xVec3 prevPos;
    xVec3 impact;
    xVec3 impactNormal;
    float radius;
    float damage;
    float knockbackMultiplier;
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

    // BFBB's position is the authority until TF2 says it has placed its host on
    // SpongeBob for this level. TF2 sends a position from the moment its map loads,
    // and its spawn point is the Valve BSP's own -- under the level, not in Bikini
    // Bottom -- so adopting it on the first packet dragged SpongeBob there whenever
    // TF2 happened to come up second. Depending on the order the two processes
    // started, the player either appeared correctly or SpongeBob was pulled under
    // the map. Now the starting position is fixed: BFBB keeps its player, TF2 moves
    // its host onto him, and only then does TF2's position mean anything.
    if (!(in->flags & BRIDGE_INTENT_PLACED))
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

// --- NPC model meshes -------------------------------------------------------
//
// A robot's collision in BFBB is a box (xBound), and a box is what TF2's proxy
// used to be: faithful to the game's own ray test, but not to the robot. This
// exports the robot's actual model triangles, once per model asset, so TF2 can
// show the shape a proxy really covers and later hit-test it.
//
// Triangles are written in the robot's own local space, which is what makes them
// reusable: the root bone matrix is set to identity for the evaluation, so the
// pose is the robot's (whatever it was the first time the bridge saw it) with the
// robot's position and facing removed. TF2 puts them back with the position and
// yaw from the state packet, so a robot that turns turns its hull too.
//
// Format, written to <bfbb dir>/bfbb_npcmodels.bfnm:
//
//     "BFNM" u32 version(1) u32 reserved
//     repeat { u32 modelId, u32 triCount, float verts[triCount][9] }
//
// Appended one record at a time, so a model that only appears mid-level costs one
// small write. Records continue to EOF and TF2 ignores a truncated tail (a
// half-written record is picked up on the next poll). The file is truncated on the
// first model of each run so a stale record can never be read as this run's.
#define BFNM_MAX_TRIS 4000
#define BFNM_MAX_VERTS 4096
#define BFNM_MAX_MODELS 128

static xVec3 sNpcModelVerts[BFNM_MAX_VERTS];
static uint32_t sNpcModelDumped[BFNM_MAX_MODELS];
static int sNpcModelDumpedCount = 0;
static bool sNpcModelFileStarted = false;

// The model asset that identifies this robot's mesh. The NPC asset is the most
// stable id (it is what the level referenced); the instance's modelID is the
// fallback, and the type is the last resort so two robots of one type still share
// a shape rather than exporting nothing.
static uint32_t NpcModelKey(const zNPCCommon* npc)
{
    if (npc->asset != NULL && npc->asset->modelInfoID != 0)
        return npc->asset->modelInfoID;
    if (npc->model != NULL && npc->model->modelID != 0)
        return npc->model->modelID;
    return 0x80000000u | (uint32_t)npc->SelfType();
}

static bool NpcModelWasDumped(uint32_t key)
{
    for (int i = 0; i < sNpcModelDumpedCount; ++i)
    {
        if (sNpcModelDumped[i] == key)
            return true;
    }
    return false;
}

// One attempt per model per run: a model that fails to export (no geometry, or a
// file that cannot be opened) must not be retried every frame.
static void NpcModelMarkDumped(uint32_t key)
{
    if (sNpcModelDumpedCount < BFNM_MAX_MODELS)
        sNpcModelDumped[sNpcModelDumpedCount++] = key;
}

// Triangles of one atomic geometry that fit in the buffer, counted the same way
// the writer counts them so the header's count is exact.
static uint32_t NpcModelCountAtomic(const RpGeometry* geom, uint32_t* outTake, uint32_t* outStride)
{
    *outTake = (uint32_t)geom->numVertices;
    if (*outTake > BFNM_MAX_VERTS)
        *outTake = BFNM_MAX_VERTS;

    const uint32_t tris = (uint32_t)geom->numTriangles;
    *outStride = (tris > BFNM_MAX_TRIS) ? ((tris + BFNM_MAX_TRIS - 1) / BFNM_MAX_TRIS) : 1;

    uint32_t count = 0;
    const RpTriangle* list = geom->triangles;
    if (list == NULL)
        return 0;
    for (uint32_t t = 0; t < tris; t += *outStride)
    {
        if (list[t].vertIndex[0] >= *outTake || list[t].vertIndex[1] >= *outTake ||
            list[t].vertIndex[2] >= *outTake)
        {
            continue;
        }
        count++;
    }
    return count;
}

static void DumpNpcModel(zNPCCommon* npc)
{
    if (npc == NULL || npc->model == NULL)
        return;

    const uint32_t key = NpcModelKey(npc);
    if (key == 0 || NpcModelWasDumped(key))
        return;
    NpcModelMarkDumped(key);

    // Pass 1: how many triangles, without evaluating any vertices.
    uint32_t total = 0;
    for (xModelInstance* inst = npc->model; inst != NULL; inst = inst->Next)
    {
        if (inst->Data == NULL || inst->Mat == NULL)
            continue;
        RpGeometry* geom = RpAtomicGetGeometry(inst->Data);
        if (geom == NULL || geom->triangles == NULL || geom->numVertices <= 0 ||
            geom->numTriangles <= 0)
            continue;
        uint32_t take = 0, stride = 1;
        total += NpcModelCountAtomic(geom, &take, &stride);
    }
    if (total == 0)
        return;

    const char* dir = getenv("BFBB_TF2BRIDGE_DIR");
    char path[512];
    snprintf(path, sizeof(path), "%s/bfbb_npcmodels.bfnm", dir != NULL ? dir : ".");

    FILE* f = fopen(path, sNpcModelFileStarted ? "ab" : "wb");
    if (f == NULL)
    {
        printf("bfbb: tf2bridge -- cannot write %s\n", path);
        return;
    }

    if (!sNpcModelFileStarted)
    {
        sNpcModelFileStarted = true;
        const uint32_t header[3] = { 0x4d4e4642u /* "BFNM" */, 1, 0 };
        fwrite(header, sizeof(header), 1, f);
    }

    const uint32_t record[2] = { key, total };
    fwrite(record, sizeof(record), 1, f);

    // Pass 2: evaluate this atomic's vertices with its root bone at the origin,
    // which leaves them in the robot's local space, and write the triangles that
    // index them.
    uint32_t written = 0;
    for (xModelInstance* inst = npc->model; inst != NULL; inst = inst->Next)
    {
        if (inst->Data == NULL || inst->Mat == NULL)
            continue;
        RpGeometry* geom = RpAtomicGetGeometry(inst->Data);
        if (geom == NULL || geom->triangles == NULL || geom->numVertices <= 0 ||
            geom->numTriangles <= 0)
            continue;

        uint32_t take = 0, stride = 1;
        NpcModelCountAtomic(geom, &take, &stride);

        xMat4x3 saved = *(xMat4x3*)inst->Mat;
        *(xMat4x3*)inst->Mat = g_I3;
        iModelVertEval(inst->Data, 0, take, inst->Mat, NULL, sNpcModelVerts);
        *(xMat4x3*)inst->Mat = saved;

        const RpTriangle* list = geom->triangles;
        const uint32_t tris = (uint32_t)geom->numTriangles;
        for (uint32_t t = 0; t < tris && written < total; t += stride)
        {
            const uint16_t* vi = list[t].vertIndex;
            if (vi[0] >= take || vi[1] >= take || vi[2] >= take)
                continue;

            float out[9];
            for (int k = 0; k < 3; ++k)
            {
                out[k * 3 + 0] = sNpcModelVerts[vi[k]].x;
                out[k * 3 + 1] = sNpcModelVerts[vi[k]].y;
                out[k * 3 + 2] = sNpcModelVerts[vi[k]].z;
            }
            fwrite(out, sizeof(out), 1, f);
            written++;
        }
    }

    fclose(f);
    printf("bfbb: tf2bridge -- npc model %08x: %u triangles (%s)\n", (unsigned)key,
        (unsigned)written, path);
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
static bool IsTF2BridgeCritter(const zNPCCommon* npc);
static bool IsTF2BridgeTarget(const zNPCCommon* npc);

// Convert TF2 weapon damage into a rough BFBB HP scale. Chuck's horizontal
// launch gets a capped bonus only for damage beyond his remaining HP. This is
// intentionally a tuning estimate: TF2 damage and BFBB HP are not identical.
static float TF2Bridge_ImpactKnockbackMultiplier(zNPCCommon* npc, float requested, float sourceDamage)
{
    if (!(requested >= 0.1f && requested <= 5.0f))
        requested = 1.5f;

    if (npc == NULL || npc->SelfType() != NPC_TYPE_CHUCK || sourceDamage <= 0.0f)
        return requested;

    const float damageInBfbbHp = sourceDamage / 35.0f;
    const float remainingHp = (float)MAX(1, ((zNPCRobot*)npc)->hitpoints);
    const float overflowDamage = MAX(0.0f, damageInBfbbHp - remainingHp);
    const float overflowScale = 1.0f + MIN(2.0f, overflowDamage * 0.5f);
    return MIN(5.0f, requested * overflowScale);
}


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
        if (!IsTF2BridgeTarget(npc) || !npc->IsAlive())
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

        const F32 splashScale = tf2splash::RocketFalloffScale(reach, radius);
        const F32 attackerDx = (center->x - rocket->attackerPos.x) * rocket->sourceUnitScale;
        const F32 attackerDy = (center->y - rocket->attackerPos.y) * rocket->sourceUnitScale;
        const F32 attackerDz = (center->z - rocket->attackerPos.z) * rocket->sourceUnitScale;
        const F32 attackerDistance = sqrtf(attackerDx * attackerDx + attackerDy * attackerDy + attackerDz * attackerDz);
        const F32 rangeScale = tf2splash::RocketRangeDamageScale(attackerDistance);
        const F32 damage = baseDamage * rangeScale * splashScale;
        const F32 maxOutputDamage = baseDamage * 1.25f;
        const F32 damageKnockbackScale = damage > 0.0f
            ? MIN(2.0f, MAX(1.0f, maxOutputDamage / damage)) : 1.0f;
        const F32 weaponKnockbackScale = TF2Bridge_ImpactKnockbackMultiplier(
            npc, rocket->knockbackMultiplier, damage);
        const F32 finalKnockbackScale = MIN(5.0f, weaponKnockbackScale * damageKnockbackScale);

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
        {
            sPendingKnockbackOverride = false;
            sPendingKnockbackTarget = NULL;
            if (IsTF2BridgeRobot(npc))
            {
                const float pushLen = sqrtf(push.x * push.x + push.z * push.z);
                sPendingKnockbackDirX = pushLen > 0.001f ? push.x / pushLen : 0.0f;
                sPendingKnockbackDirY = push.y;
                sPendingKnockbackDirZ = pushLen > 0.001f ? push.z / pushLen : 1.0f;
                sPendingKnockbackMultiplier = finalKnockbackScale;
                sPendingKnockbackTarget = npc;
                sPendingKnockbackOverride = true;
            }
            npc->Damage(DMGTYP_SIDE, NULL, &push);
            if (npc->SelfType() == NPC_TYPE_CHUCK && npc->IsAlive() && npc->frame != NULL &&
                sPendingKnockbackOverride && sPendingKnockbackTarget == npc)
            {
                npc->frame->vel.x += push.x * 2.5f * sPendingKnockbackMultiplier;
                npc->frame->vel.z += push.z * 2.5f * sPendingKnockbackMultiplier;
                sPendingKnockbackTarget = NULL;
                sPendingKnockbackOverride = false;
            }
        }

        printf("bfbb: tf2bridge -- rocket splash type %d %s dist=%.2f/%.2f attackerDist=%.1f range=%.3f splash=%.3f damage=%.1f kb=%.2f "
               "applyDamage=%d\n",
               (int)target.npcType, direct ? "DIRECT" : (wrapped ? "VISIBLE(wrapped)" : "VISIBLE"),
               (double)reach, (double)radius, (double)attackerDistance, (double)rangeScale, (double)splashScale,
               (double)damage, (double)finalKnockbackScale, applyDamage ? 1 : 0);
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
        rocket->attackerPos = FromSource(in->rocketAttackerPos[i][0], in->rocketAttackerPos[i][1],
                                         in->rocketAttackerPos[i][2], in->scale);
        rocket->sourceUnitScale = in->scale;
        rocket->radius = in->rocketRadius[i] / in->scale;
        rocket->damage = in->rocketDamage[i];
        rocket->knockbackMultiplier = in->rocketKnockbackMultiplier[i];
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

static void BridgeHitWindowsTick();

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
    BridgeHitWindowsTick();

    // A talk box -- a text box, and every "press R" prompt -- is answered by BFBB's
    // own pad and by nothing else, and while TF2 drives the movement BFBB's pad is
    // closed to TF2's buttons. Tell the platform layer when one is waiting, so the
    // confirm key can go through and nothing else does. See iTF2BridgeApplyPad.
    //
    // This is the one interaction TF2 cannot show on its own, so it leaves a line in
    // the session log: when a box starts and stops waiting, and when a confirm press
    // is actually there to be forwarded. The next "I could not get past this text" is
    // then answered by whether it was waiting and whether the key arrived, instead of
    // by another guess.
    {
        const int padWanted = zTalkBoxWantsPad();
        iTF2BridgeSetPadWanted(padWanted);

        const U32 scene = globals.sceneCur != NULL ? globals.sceneCur->sceneID : 0u;
        static int sPadWantedLogged = 0;
        if (padWanted != sPadWantedLogged)
        {
            sPadWantedLogged = padWanted;
            TF2Bridge_Logf("TALK", "wants_pad=%d scene=%08x", padWanted, scene);
        }

        const BridgeIntentPacket* talkIn = iTF2BridgeGetIntent();
        const U32 confirm = (talkIn != NULL) ? (talkIn->buttons & BRIDGE_IN_USE) : 0u;
        static U32 sConfirmHeld = 0;
        if (padWanted && confirm && !sConfirmHeld)
        {
            TF2Bridge_Logf("TALK", "confirm forwarded scene=%08x", scene);
        }
        sConfirmHeld = confirm;
    }

    const bool playing = zGameModeGet() == eGameMode_Game;
    const bool legacyNpcTrace = TF2Bridge_EnvFlag("BFBB_TF2BRIDGE_LEGACY_NPC_TRACE");

    // The session log's per-frame half: level changes, the timeline and the trail of
    // level entities. Independent of the audit's env flag, because this is the one
    // meant to be left on for a whole play session.
    TF2Bridge_SessionLogFrame(playing);

    // While the bridge drives the player, TF2 owns the player's health and BFBB
    // owns dying. When the host reports the player down, run BFBB's own death
    // once, so the fade, the rewards, the checkpoint and the respawn all stay the
    // game's; it hands control back when it is done, and TF2 follows the position.
    static bool sTf2DeathApplied = false;
    static bool sTf2DeathReported = false;
    const BridgeIntentPacket* healthIn = iTF2BridgeGetIntent();
    const bool tf2PlayerDown = healthIn != NULL && healthIn->tfhealth <= 0;
    if (playing && tf2PlayerDown && !sTf2DeathApplied)
    {
        if (globals.player.Health > 0)
        {
            zEntPlayer_ForceDeathByBridge();
        }
        // Latch only once BFBB's player is actually down: a frame with no model
        // yet (still loading, or a scene change in progress) has to be allowed to
        // retry. The force kill skips the hit funnel entirely, so the only way it
        // can be refused is a missing model -- say so once if that lasts.
        sTf2DeathApplied = globals.player.Health == 0;
        if (!sTf2DeathApplied && !sTf2DeathReported)
        {
            sTf2DeathReported = true;
            printf("bfbb: tf2bridge -- TF2 is dead but the player death did not run "
                   "(health %d, frame %p, model %p); retrying\n",
                   globals.player.Health, (void*)globals.player.ent.frame,
                   (void*)globals.player.ent.model);
        }
    }
    else if (!tf2PlayerDown)
    {
        sTf2DeathApplied = false;
        sTf2DeathReported = false;
    }

    // Proxy hits are the authoritative combat events once NPC placeholders are
    // mirrored into TF2. Keep BFBB's damage/death path, but no longer synthesize
    // a second hit from the same fire command on BFBB's side.
    if (playing)
        TF2Bridge_ProcessNpcHits();
    else
    {
        BridgeNpcHitPacket discardedHit;
        while (iTF2BridgePopNpcHit(&discardedHit))
        {
        }
    }

    // Weapon events still carry ray and projectile state for BFBB's existing
    // rocket sweep. Direct hits now arrive separately from the matching TF2
    // NPC proxy so each shot has its real damage and crit metadata.
    BridgeIntentPacket ev;
    while (iTF2BridgePopEvent(&ev))
    {
        if (playing && legacyNpcTrace)
            TF2Bridge_ProcessWeaponEvent(&ev);
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
        if (legacyNpcTrace && !event && !rocketGun && attack && !sLastAttack)
            TF2Bridge_FireAtNPCs(attackIn);
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

}

// The state packet, sent after the scene and the player have updated rather than
// from zTF2Bridge_Frame. That hook runs before zSceneUpdate, so building the
// packet there reported every robot and the player as they were one frame ago;
// TF2's proxies -- and the collision cage drawn on one -- sat a frame behind the
// models for no reason but call order. Intent stays in zTF2Bridge_Frame: the
// player's own movement and weapon events must be read before the update runs.
// Where BFBB puts its player: the level's PLYR asset, which is the entity
// zGameSetupPlayer hands to zEntPlayer_Init (it takes the *last* PLYR asset, so do
// the same, or a level with more than one would report a different spot than the
// one the player is actually spawned on). `ang.x` is the yaw in radians, going by
// how zGameSetupPlayer writes sPlayerStartAngle there.
static bool GetPlayerSpawnPoint(xVec3* outPos, float* outYaw)
{
    const U32 count = xSTAssetCountByType('PLYR');
    if (count == 0 || outPos == NULL)
        return false;

    xEntAsset* asset = (xEntAsset*)xSTFindAssetByType('PLYR', count - 1, NULL);
    if (asset == NULL)
        return false;

    *outPos = asset->pos;
    if (outYaw != NULL)
        *outYaw = asset->ang.x * (180.0f / 3.14159265f);
    return true;
}

void zTF2Bridge_SendState()
{
    if (!iTF2BridgeActive())
    {
        return;
    }

    const bool playing = zGameModeGet() == eGameMode_Game;
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
        st.flags |= BRIDGE_STATE_NPC_TARGETS;
        // Only Play means the player is actually back in the level: the death
        // sequence, the fade and the checkpoint reload all run in other states, and
        // TF2 waits for this before it lets its host respawn.
        if (zGameStateGet() == eGameState_Play)
        {
            st.flags |= BRIDGE_STATE_PLAYING;
        }
        if (BfbbOwnsPlayerNow())
        {
            st.flags |= BRIDGE_STATE_CONTROL_OFF;
        }

        // The level's own spawn point, so TF2 can put a real player there when it
        // has no live BFBB player to copy -- a TF2 respawn that lands before
        // SpongeBob is back. Sending it every frame keeps it correct across a
        // checkpoint reload without TF2 having to ask for it at the right moment.
        xVec3 spawnPos;
        float spawnYaw = 0.0f;
        if (GetPlayerSpawnPoint(&spawnPos, &spawnYaw))
        {
            st.spawnX = spawnPos.x;
            st.spawnY = spawnPos.y;
            st.spawnZ = spawnPos.z;
            st.spawnYaw = spawnYaw;
            st.flags |= BRIDGE_STATE_HAS_SPAWN;
        }

        // Once per scene, on the first frame it is in play.
        static uint32_t sDumpedScene = 0xFFFFFFFFu;
        if (st.sceneId != sDumpedScene)
        {
            sDumpedScene = st.sceneId;
            DumpSceneCollision(st.sceneId);
        }

        // Mirror live BFBB NPCs as hittable TF2-side target proxies. The scene
        // asset id is stable across frames and is returned with damage.
        //
        // Two passes, robots first: a jellyfish field can hold more critters than
        // the packet has slots, and they must not push a robot out of the list. The
        // order within a pass is the NPC manager's, which is stable enough.
        st.npcCount = 0;
        st_XORDEREDARRAY* npclist = zNPCMgr_GetNPCList();
        for (S32 pass = 0; npclist != NULL && pass < 2 && st.npcCount < BRIDGE_MAX_NPC_TARGETS; ++pass)
        {
            for (S32 i = 0; i < npclist->cnt && st.npcCount < BRIDGE_MAX_NPC_TARGETS; ++i)
            {
                zNPCCommon* npc = (zNPCCommon*)npclist->list[i];
                const bool wanted = (pass == 0) ? IsTF2BridgeRobot(npc) : IsTF2BridgeCritter(npc);
                if (!wanted || npc == NULL || npc->frame == NULL || !npc->IsAlive())
                    continue;

                BridgeNpcTarget& target = st.npcs[st.npcCount++];
                target.id = npc->id;
                target.type = (int32_t)npc->SelfType();
                target.x = npc->frame->mat.pos.x;
                target.y = npc->frame->mat.pos.y;
                target.z = npc->frame->mat.pos.z;

                // The robot's own collision box, relative to its origin, so the TF2
                // proxy is the shape the robot actually is. xBoundGetBox turns the
                // sphere and oriented-box cases into the world AABB the game's own
                // rays are tested against (xRayHitsBound in TF2Bridge_FireAtNPCs), so
                // a shot that would hit in BFBB hits the proxy at the same place.
                memset(&target.minX, 0, sizeof(float) * 6);
                if (npc->bound.type != XBOUND_TYPE_NA)
                {
                    xBox bounds;
                    xBoundGetBox(bounds, npc->bound);
                    target.minX = bounds.lower.x - target.x;
                    target.minY = bounds.lower.y - target.y;
                    target.minZ = bounds.lower.z - target.z;
                    target.maxX = bounds.upper.x - target.x;
                    target.maxY = bounds.upper.y - target.y;
                    target.maxZ = bounds.upper.z - target.z;
                }

                // The box above is what BFBB collides with; the model below is what
                // the robot looks like. Exported once per model asset, then merely
                // referenced by id every frame (see DumpNpcModel). The yaw lets TF2
                // turn the local-space mesh the way the robot is facing.
                target.modelId = NpcModelKey(npc);
                target.yaw = atan2f(npc->frame->mat.at.x, npc->frame->mat.at.z) *
                    (180.0f / 3.14159265f);
                DumpNpcModel(npc);
            }
        }
    }

    // Read from the scene rather than from the packet: st.sceneId is only filled
    // while the player is in play, and a level change is exactly the stretch where
    // he is not -- which is the stretch this is here to show.
    TF2Bridge_AuditFrame(globals.sceneCur != NULL ? globals.sceneCur->sceneID : 0,
                         zGameStateGet() == eGameState_Play);

    iTF2BridgeSendState(&st);
}

// One accepted hit on the BFBB player, forwarded to TF2. Damage stays in BFBB
// units so the health-per-unit conversion lives on TF2's side, with the cvar.
// Per-robot damage, in TF2 health points. The bridge owns the host's health, so a
// robot does not have to spend BFBB's chunks: a fodder prod costs 25 health and a
// bomb bot's burst costs 80. Anything absent from this table keeps the chunk path,
// which TF2 scales with bfbb_player_damage_unit -- that is also what hazards use.
//
// Only robots with one clear attack are listed. Cases where the attack matters more
// than the robot wait for a hit packet that can say which attack landed: chuck's
// direct hit against his splash pool, bzzt's laser, sleepy's beam and tartar's
// puddle tick. Tubelet is listed anyway -- its flame tick is the hit that matters,
// and its resurrection blast is separated by hit kind in the report below.
//
// These are per hit, and the hit rate is the bridge's, not g.DamageTimeHit: a
// continuously re-applied attack is given its own cadence by BridgeHitCooldown, so
// the number has to be read together with that rate. Tubelet's flame tick is 0.075 s
// and chomper's gas 0.5 s, which is why both read small here and why the gas keeps
// the ten health per second it had at fifteen per 1.5 s.
struct BridgeRobotDamage
{
    int selfType;
    U32 health;
};

static const BridgeRobotDamage sRobotDamage[] =
{
    { NPC_TYPE_FODDER, 25 },  // cattleprod
    { NPC_TYPE_HAMMER, 50 },  // ham
    { NPC_TYPE_HAMSPIN, 50 },
    { NPC_TYPE_TARTAR, 40 },  // acid spit, direct
    { NPC_TYPE_GLOVE, 40 },   // spin contact
    { NPC_TYPE_CHUCK, 60 },   // direct hit
    { NPC_TYPE_MONSOON, 45 }, // lightning
    { NPC_TYPE_ARFARF, 60 },  // swing
    { NPC_TYPE_ARFDOG, 30 },  // dawg that reached the player
    { NPC_TYPE_FODBOMB, 80 }, // the burst
    { NPC_TYPE_FODBZZT, 40 }, // laser
    { NPC_TYPE_SLICK, 40 },   // spit
    { NPC_TYPE_TUBELET, 6 },  // flame spiral tick, at 0.075 s (about 80 per second)
    { NPC_TYPE_TUBESLAVE, 6 }, // the spinning part of the same flame
    { NPC_TYPE_CHOMPER, 5 },  // gas breath, at 0.5 s (ten per second, as before)
    // Jellyfish are ambients that zap on a contact attack, so their name and
    // direction reach the bridge on their own; these are only the numbers. A zap
    // lands once per g.DamageTimeHit anyway, because BFBB's own hit timer is what
    // gates the repeated ZapperUpdate calls.
    { NPC_TYPE_JELLYPINK, 10 },
    { NPC_TYPE_JELLYBLUE, 12 }, // faster chaser in the jelly-only scenes
};

// Tubelet's resurrection blast is the same robot as its flame, so the kind has to
// separate them: one big hit, not a tick.
static const U32 kTubeletBlastHealth = 12;

// Robots whose damage reaches the player as particles rather than as an NPC-owned
// hazard. Tubelet's flame spiral and chomper's gas breath both hurt through
// xParCmdDamagePlayer, which names no source at all, so the bridge read them as
// hazards -- goo, a pit, a spike trap -- and TF2 kept them silent (no pain sound,
// no damage arrow) and never knew which robot they were.
//
// Nothing links a particle back to the robot that emitted it, so the particle's own
// position is the lookup: both attacks are short range and the robot is standing in
// its own flame or gas. The radius has to cover a tubelet's spiral (the blast ball
// reaches seven units from the tower) plus the spread of the particles themselves,
// so twenty-four rather than twelve -- at twelve the lookup missed and the hit stayed
// a silent hazard, which is why the flame still had no pain sound. Testing only those
// three types keeps a real hazard that happens to land near a robot from being
// blamed on it.
static const F32 kParticleSourceRange = 24.0f;

// Set by whatever is dealing the damage immediately before it does so, consumed by
// the damage funnel. This is what separates a tubelet's flame (burns, never shoves)
// from the same tubelet's blast (throws like a bomb bot): the robot type cannot, and
// the NPC could not even be named for the flame. Kept as a kind rather than as one
// boolean so a hazard can say "explosion" without the particle looked-up source
// being involved at all.
static int sHitKind = BRIDGE_HITKIND_NONE;
static xVec3 sHitKindPos = { 0.0f, 0.0f, 0.0f };
static bool sHitKindHasPos = false;

void zTF2Bridge_NoteHitKind(int kind, const xVec3* pos)
{
    sHitKind = kind;
    sHitKindHasPos = (pos != NULL);
    if (pos != NULL)
    {
        sHitKindPos = *pos;
    }
}

// ---------------------------------------------------------------------------
// Hit cadence.
//
// BFBB gates every hit on a single timer: zEntPlayer_Damage refuses to run while
// globals.player.DamageTimer is up, and a hit sets it to g.DamageTimeHit (1.5 s in
// sb.ini). That is the right shape for a melee swing and the wrong shape for an
// attack the game re-applies every frame -- chomper's gas, tubelet's flame spiral and
// sleepy's beam all call the funnel continuously and were only 1.5 s apart because
// of that one timer, not because of anything about the attack.
//
// The bridge keeps its own window per (source, hit kind) instead, so each attack
// carries its own rate and two robots can no longer swallow each other's hits.
// Anything the bridge does not tune keeps the stock timer, which is also what the
// whole game falls back to when the bridge is off.
struct BridgeHitWindow
{
    const xBase* src;
    int kind;
    F32 cooldown;
    F32 lastHit; // bridge clock at the last accepted hit, for afterburn exposure
};

static const int kHitWindowCount = 16;
static BridgeHitWindow sHitWindows[kHitWindowCount];
static int sHitWindowNext = 0;

// Seconds since the bridge came up, ticked with the windows. The windows need a clock
// that only ever moves forward, and a hit's exposure is a difference of two readings.
static F32 sBridgeClock = 0.0f;

// Tubelet's spiral is a flamethrower: re-applied every frame, and this is the rate
// that reads as one continuous fire rather than as a row of hits.
static const F32 kTubeletFlameCadence = 0.075f;

// One lightning strike on its own is the minimum ignition: a monsoon bolt is a burst,
// not an exposure the player can stand in.
static const F32 kMonsoonIgnite = 3.0f;

// The exposure this hit carries for afterburn, read by the report. Set by
// zTF2Bridge_NotePlayerHit, which is the only place that sees how long it has been
// since the last hit of the same attack.
static F32 sPendingBurn = 0.0f;

// The robot type behind a hit, or 0 for a hazard, a surface, or NULL. 'struct', not
// 'class', for the cast below: see the note in zTF2Bridge.h about the mangler.
static int BridgeSelfType(const xBase* src)
{
    if (src == NULL || (src->baseType != eBaseTypeNPC && src->baseType != eBaseTypeVillain))
    {
        return 0;
    }

    return (int)((const zNPCCommon*)src)->SelfType();
}

// How long one hit from a continuously re-applied attack holds off the next one, in
// seconds. 0 means "not tuned: use g.DamageTimeHit".
static F32 BridgeHitCooldown(const xBase* src, int kind)
{
    switch (BridgeSelfType(src))
    {
    case NPC_TYPE_TUBELET:
    case NPC_TYPE_TUBESLAVE:
        // The spiral is a flamethrower: re-applied every frame, and 0.075 s is what
        // makes it read as one continuous fire rather than as a row of hits. Only
        // the flame is gated -- the resurrection blast is one big hit and must not be
        // swallowed by a tick that landed a frame earlier.
        return kind == BRIDGE_HITKIND_FIRE ? kTubeletFlameCadence : 0.0f;

    case NPC_TYPE_CHOMPER:
        // The gas is a cloud, so it damages on TF2's afterburn tick instead of a
        // flamethrower's particle rate.
        return kind == BRIDGE_HITKIND_FIRE ? 0.5f : 0.0f;

    case NPC_TYPE_SLEEPY:
        // The beam holds the player under it, so it damages at the same rate.
        return 0.5f;

    default:
        return 0.0f;
    }
}

// The timer left behind after a hit. A continuously re-applied attack leaves a short
// flinch instead of g.DamageTimeHit, because that timer is also what holds off the
// untuned hits: a flame tick that left 1.5 s behind it would stop a fodder's prod
// landing, and the flinch is what keeps BFBB's own hit reaction (ouch, shine, model
// animation, rumble) from strobing at thirteen ticks a second.
static const F32 kBridgeFlinch = 0.25f;

static BridgeHitWindow* BridgeHitWindowArm(const xBase* src, int kind, F32 cooldown)
{
    int freeSlot = -1;
    for (int i = 0; i < kHitWindowCount; ++i)
    {
        if (sHitWindows[i].src == src && sHitWindows[i].kind == kind)
        {
            sHitWindows[i].cooldown = MAX(sHitWindows[i].cooldown, cooldown);
            return &sHitWindows[i];
        }

        if (freeSlot < 0 && !(sHitWindows[i].cooldown > 0.0f))
        {
            freeSlot = i;
        }
    }

    // Every slot busy: recycle. Sixteen attacks landing inside the same window is
    // already more than the player can be standing in.
    if (freeSlot < 0)
    {
        freeSlot = sHitWindowNext;
        sHitWindowNext = (sHitWindowNext + 1) % kHitWindowCount;
    }

    sHitWindows[freeSlot].src = src;
    sHitWindows[freeSlot].kind = kind;
    sHitWindows[freeSlot].cooldown = cooldown;
    // A slot that has just been reused must not report the gap since whatever attack
    // held it last: the whole clock's worth would arrive as one jump of exposure.
    sHitWindows[freeSlot].lastHit = sBridgeClock;
    return &sHitWindows[freeSlot];
}

bool zTF2Bridge_PlayerHitAllowed(xBase* src)
{
    if (!iTF2BridgeActive())
    {
        return !(globals.player.DamageTimer > 0.0f);
    }

    // A blast is one hit by definition, so it must not be swallowed by the flinch a
    // flame tick left behind a quarter of a second earlier -- which is exactly the
    // moment a tubelet is about to go off, since it flames right up to the blast.
    if (sHitKind == BRIDGE_HITKIND_EXPLOSION)
    {
        return true;
    }

    const F32 cooldown = BridgeHitCooldown(src, sHitKind);
    if (cooldown <= 0.0f)
    {
        // A one-shot attack, or a source with no cadence of its own: the stock timer.
        return !(globals.player.DamageTimer > 0.0f);
    }

    for (int i = 0; i < kHitWindowCount; ++i)
    {
        if (sHitWindows[i].src == src && sHitWindows[i].kind == sHitKind)
        {
            return !(sHitWindows[i].cooldown > 0.0f);
        }
    }

    return true;
}

F32 zTF2Bridge_NotePlayerHit(xBase* src)
{
    const int selfType = BridgeSelfType(src);
    const F32 cooldown = BridgeHitCooldown(src, sHitKind);
    sPendingBurn = 0.0f;

    if (cooldown <= 0.0f)
    {
        // Monsoon's lightning is a strike, not an exposure: every bolt ignites for the
        // same three seconds whether or not the player is still burning.
        if (selfType == NPC_TYPE_MONSOON)
        {
            sPendingBurn = kMonsoonIgnite;
        }

        return globals.player.g.DamageTimeHit;
    }

    BridgeHitWindow* pWindow = BridgeHitWindowArm(src, sHitKind, cooldown);

    if ((selfType == NPC_TYPE_TUBELET || selfType == NPC_TYPE_TUBESLAVE) &&
        sHitKind == BRIDGE_HITKIND_FIRE)
    {
        // Exposure is the real time since the last tick, not the nominal cadence: the
        // spiral only touches the player when one of its particles reaches them, so a
        // fixed per-tick value would under-count and the burn would stop growing
        // instead of climbing. The gaps sum to the time actually spent in it whatever
        // the hit rate. Capped so a long absence cannot arrive as one big jump; the
        // first tick of a new window has no gap by construction.
        sPendingBurn = CLAMP(sBridgeClock - pWindow->lastHit, 0.0f, 1.0f);
    }

    pWindow->lastHit = sBridgeClock;
    return kBridgeFlinch;
}

static void BridgeHitWindowsTick()
{
    const F32 dt = globals.update_dt;
    sBridgeClock += dt;

    for (int i = 0; i < kHitWindowCount; ++i)
    {
        if (sHitWindows[i].cooldown > 0.0f)
        {
            sHitWindows[i].cooldown -= dt;
        }
    }
}

// Everything the bridge keeps about the level, dropped on a scene change. The hit
// windows hold raw entity pointers, and a level load frees and reallocates those:
// a stale window would match a *different* NPC that happens to land on the same
// address, which is a hit silently swallowed in the new level. The pending
// knockback and the burn exposure are one-shot state for a hit that no longer
// exists, so they go the same way.
static void BridgeResetAfterSceneChange()
{
    memset(sHitWindows, 0, sizeof(sHitWindows));
    sHitWindowNext = 0;
    sPendingKnockbackTarget = NULL;
    sPendingKnockbackOverride = false;
    sPendingBurn = 0.0f;
    sHitKind = BRIDGE_HITKIND_NONE;
    sHitKindHasPos = false;
}

// ---------------------------------------------------------------------------
// Session log: one text file for a whole play session, written by both halves.
//
// A real level is the first thing the bridge has met that it was not built for: a
// PLAT is not a surface the player stands on here, a trigger volume is never
// entered, a hazard that hurts by collision never fires, and a BUTN cannot be
// pressed. Playing it and reading the console does not say which of those a given
// moment actually was, so the session writes down what the level had around the
// player, what tried to hurt him, and what came back. That is a list to read
// afterwards, not a fix.
//
// Both processes append to this one file, so a line is a single open/append/close of
// one formatted line: the writes interleave between lines, never inside them, and
// the wall clock at the front is what puts the two halves in order. The format is
// therefore fixed, and greppable rather than parsed:
//
//     HH:MM:SS.mmm <bfbb|tf2> <KIND> key=value ...
//
//   SESSION  a process started, and where it is writing
//   MAP      TF2 loaded a map
//   LEVEL    a BFBB scene change: the progression, and the gap the load took
//   PLACE    TF2 put its host on BFBB's player or on the level's PLYR spawn
//   SPOT     where the player is, every 15 s: the timeline
//   NEAR     a level entity came within reach: what the level is made of here
//   HIT      a hit BFBB accepted and sent (kind F fire/gas, X blast, ? other)
//   REFUSED  a hit BFBB saw and would not apply (cooldown, control, teleport)
//   HITNPC   a TF2 shot BFBB applied to a robot or critter
//   DEATH / RESPAWN   the BFBB player down, and back on his feet
//   NOTE     the user's own bookmark, typed in TF2: bfbb_note <text>
//   WARN     something the bridge wanted to do and could not
//
// Off is BFBB_TF2BRIDGE_LOG=0; any other value is the file to write instead. The
// default directory is the repo's: TF2 cannot learn BFBB's working directory, and
// this half cannot learn TF2's, so the path is absolute on both sides on purpose.
static const char* kTF2BridgeLogDir = "D:/bfbbpc/bfbb/logs";
static const long kTF2BridgeLogMax = 24 * 1024 * 1024;

// Resolved once: the file's size, so the cap survives the process, and how many
// scenery lines have gone in, so a kelp field cannot fill the file.
static long sLogBytes = -1;
static long sLogSceneryLines = 0;

static bool TF2Bridge_LogWanted()
{
    static int sCached = -1;
    if (sCached < 0)
    {
        const char* v = getenv("BFBB_TF2BRIDGE_LOG");
        // Off is "0" and nothing else: a path is a path, even one that starts with 0.
        sCached = (v != NULL && v[0] == '0' && v[1] == '\0') ? 0 : 1;
    }
    return sCached != 0;
}

static const char* TF2Bridge_LogPath()
{
    static char sPath[512];
    static bool sReady = false;
    if (sReady)
        return sPath;
    sReady = true;

    const char* env = getenv("BFBB_TF2BRIDGE_LOG");
    if (env != NULL && env[0] != '\0' && !(env[0] == '0' && env[1] == '\0'))
    {
        snprintf(sPath, sizeof(sPath), "%s", env);
        return sPath;
    }

    // One file a day, so a session is a file and yesterday's is not appended to.
    const time_t now = time(NULL);
    struct tm tmv;
    struct tm* t = NULL;
#ifdef _WIN32
    if (localtime_s(&tmv, &now) == 0)
        t = &tmv;
#else
    t = localtime(&now);
#endif
    if (t == NULL)
        snprintf(sPath, sizeof(sPath), "%s/session.log", kTF2BridgeLogDir);
    else
        snprintf(sPath, sizeof(sPath), "%s/session_%04d-%02d-%02d.log", kTF2BridgeLogDir,
            t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
    return sPath;
}

static void TF2Bridge_LogStamp(char* out, int size)
{
    time_t now = time(NULL);
    unsigned ms = 0;
#ifdef _WIN32
    struct __timeb64 tb;
    _ftime64(&tb);
    now = tb.time;
    ms = (unsigned)tb.millitm;
#endif

    struct tm tmv;
    struct tm* t = NULL;
#ifdef _WIN32
    if (localtime_s(&tmv, &now) == 0)
        t = &tmv;
#else
    t = localtime(&now);
#endif
    if (t == NULL)
        snprintf(out, size, "--:--:--.---");
    else
        snprintf(out, size, "%02d:%02d:%02d.%03u", t->tm_hour, t->tm_min, t->tm_sec, ms);
}

static long TF2Bridge_LogSize()
{
    if (sLogBytes >= 0)
        return sLogBytes;

    sLogBytes = 0;
    FILE* f = fopen(TF2Bridge_LogPath(), "rb");
    if (f != NULL)
    {
        if (fseek(f, 0, SEEK_END) == 0)
            sLogBytes = ftell(f);
        fclose(f);
    }
    return sLogBytes;
}

// One line, one open. A crash costs at most the line being written, and two
// processes cannot end up inside each other's line.
static void TF2Bridge_LogRaw(const char* text)
{
    if (text == NULL || !TF2Bridge_LogWanted())
        return;
    if (TF2Bridge_LogSize() >= kTF2BridgeLogMax)
        return;

    FILE* f = fopen(TF2Bridge_LogPath(), "ab");
    if (f == NULL)
    {
        // First run from a clean checkout: the directory does not exist yet.
#ifdef _WIN32
        _mkdir(kTF2BridgeLogDir);
#endif
        f = fopen(TF2Bridge_LogPath(), "ab");
        if (f == NULL)
            return;
    }

    const int len = (int)strlen(text);
    fwrite(text, 1, (size_t)len, f);
    fclose(f);
    sLogBytes += len;
}

static void TF2Bridge_Logf(const char* kind, const char* fmt, ...)
{
    if (!TF2Bridge_LogWanted())
        return;

    char body[704];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    char stamp[24];
    TF2Bridge_LogStamp(stamp, sizeof(stamp));

    char line[768];
    snprintf(line, sizeof(line), "%s bfbb %s %s\n", stamp, kind, body);
    TF2Bridge_LogRaw(line);
}

// The player's position in Source units, in Source's axes (see the axis mapping at
// the top of this file), so a log line and a bfbb_coll_info reading are the same
// numbers and can be compared without converting.
static void TF2Bridge_LogPos(char* out, int size, const xVec3* p, F32 scale)
{
    if (p == NULL)
    {
        snprintf(out, size, "(? ? ?)");
        return;
    }
    snprintf(out, size, "(%.0f %.0f %.0f)",
        (double)(p->z * scale), (double)(p->x * scale), (double)(p->y * scale));
}

// The player's frame, when it is still the game's own memory. The frame belongs to
// whichever scene created it, so between a level unload and the player coming back
// the pointer is not null, only wrong -- and this walk reads it every frame.
static const xVec3* TF2Bridge_PlayerPos()
{
    xEntFrame* f = globals.player.ent.frame;

    if (f == NULL || !xMemAddrInHeap(f))
        return NULL;

    return &f->mat.pos;
}

// An entity's position, if the entity and whatever holds the position are both the
// game's memory. The scene's entity array is longer lived than the objects it
// names: a level half loaded, or an entity freed with the level before it whose
// address has since been reused, both leave an entry that is not null and not an
// entity. Dereferencing one of those faults in the middle of a log line, so the
// address is checked and the entity skipped instead.
static const xVec3* TF2Bridge_EntPos(const xEnt* ent)
{
    if (ent == NULL || !xMemAddrInHeap(ent))
        return NULL;

    if (ent->frame != NULL && xMemAddrInHeap(ent->frame))
        return &ent->frame->mat.pos;

    if (ent->asset != NULL && xMemAddrInHeap(ent->asset))
        return &ent->asset->pos;

    return NULL;
}

// True only when this entity names something that cannot be followed: the entity
// itself, the frame that holds its position, or its asset. A NULL frame or asset is
// not that -- most of a level's objects have no model at all -- so it is not a
// warning, it simply has no position to report.
static bool TF2Bridge_EntUnreadable(const xEnt* ent)
{
    if (ent == NULL)
        return false;

    if (!xMemAddrInHeap(ent))
        return true;

    if (ent->frame != NULL && !xMemAddrInHeap(ent->frame))
        return true;

    if (ent->asset != NULL && !xMemAddrInHeap(ent->asset))
        return true;

    return false;
}

// The entity's axis-aligned bound, for the one line that wants to know whether the
// player is standing inside something. Asked in one place because xBoundGetBox
// reads an OBB's matrix pointer without checking it, and that pointer is as
// unchecked as the rest of the scene.
static bool TF2Bridge_EntBox(const xEnt* ent, xBox& box)
{
    const S32 type = ent->bound.type;

    if (type == XBOUND_TYPE_SPHERE || type == XBOUND_TYPE_BOX)
    {
        xBoundGetBox(box, ent->bound);
        return true;
    }

    if (type == XBOUND_TYPE_OBB && ent->bound.mat != NULL && xMemAddrInHeap(ent->bound.mat))
    {
        xBoundGetBox(box, ent->bound);
        return true;
    }

    return false;
}

// The scale TF2 sent this frame. 50 is the bridge's long-standing stand-in for
// "no packet yet" (before TF2 has placed the player), not a real ratio.
static F32 TF2Bridge_LiveScale()
{
    const BridgeIntentPacket* in = PuppetIntent();
    return (in != NULL && in->scale > 0.0f) ? in->scale : 50.0f;
}

static void TF2Bridge_SessionLogBegin()
{
    if (!TF2Bridge_LogWanted())
        return;

    if (TF2Bridge_LogSize() == 0)
    {
        TF2Bridge_LogRaw(
            "# BFBB <-> TF2 bridge session log. One line an event, both processes append.\n"
            "# HH:MM:SS.mmm <bfbb|tf2> <KIND> key=value ...   distances in Source units, scenes in hex\n"
            "# SESSION start/end | MAP tf2 map | LEVEL bfbb scene change | PLACE host put on bfbb\n"
            "# SPOT every 15 s | NEAR level entity in reach | HIT bfbb hit sent to tf2\n"
            "# REFUSED hit bfbb would not apply | HITNPC tf2 shot applied to a robot\n"
            "# DEATH/RESPAWN | NOTE from tf2 (bfbb_note <text>) | WARN\n"
            "# TALK a bfbb talk box started/stopped waiting for a pad press, and confirm presses\n");
    }

    char pos[64];
    const xVec3* p = TF2Bridge_PlayerPos();
    TF2Bridge_LogPos(pos, sizeof(pos), p, TF2Bridge_LiveScale());
    TF2Bridge_Logf("SESSION", "begin side=bfbb log=%s pos=%s scene=%08x",
        TF2Bridge_LogPath(), pos,
        globals.sceneCur != NULL ? globals.sceneCur->sceneID : 0u);
}

// Which kinds are worth naming in the trail, and how near counts as near. The
// gameplay kinds are the ones the bridge does not implement, so they get a wide
// reach; scenery gets a short one, because a STAT line means "you are in this",
// not "this exists". Both are deduped by asset id, so passing something once is
// one line, and coming back to it after leaving is a new one.
static const int sLogKinds[] = {
    eBaseTypeButton, eBaseTypeTrigger, eBaseTypePlatform, eBaseTypePickup,
    eBaseTypeDestructObj, eBaseTypeSavePoint, eBaseTypeTeleportBox, eBaseTypePortal,
    eBaseTypeVolume, eBaseTypeHangable, eBaseTypePendulum, eBaseTypeDoor,
    eBaseTypeDynamic, eBaseTypeStatic,
};
static const int kLogKindCount = (int)(sizeof(sLogKinds) / sizeof(sLogKinds[0]));

struct TF2BridgeLogSeen
{
    U32 id;
    U32 scan;
};
static TF2BridgeLogSeen sLogSeen[512];
static U32 sLogScan = 0;

static void TF2Bridge_SessionLogFrame(bool playing)
{
    static bool sBegun = false;
    if (!sBegun)
    {
        sBegun = true;
        TF2Bridge_SessionLogBegin();
    }

    if (!TF2Bridge_LogWanted())
        return;

    static U32 sLogScene = 0;
    static int sLogHealth = -1;
    static F32 sLogNextSpot = 0.0f;
    static F32 sLogNextScan = 0.0f;

    static F32 sLogNextRefuseWarn = 0.0f;

    zScene* sc = globals.sceneCur;
    const U32 sceneId = (sc != NULL) ? sc->sceneID : 0u;
    const xVec3* pos = TF2Bridge_PlayerPos();
    const F32 scale = TF2Bridge_LiveScale();

    if (sceneId != sLogScene)
    {
        sLogScene = sceneId;
        memset(sLogSeen, 0, sizeof(sLogSeen));
        sLogScan = 0;
        sLogNextScan = 0.0f; // the new level's first scan, at once
        sLogNextSpot = 0.0f;
    }

    // Deaths and respawns are the spine of a session: what killed the player is
    // asked about one line earlier (HIT) or nowhere at all (a hazard the bridge
    // never collided).
    const int health = (int)globals.player.Health;
    if (sLogHealth >= 0 && health != sLogHealth && (health == 0 || sLogHealth == 0))
    {
        char pstr[64];
        TF2Bridge_LogPos(pstr, sizeof(pstr), pos, scale);
        if (health == 0)
            TF2Bridge_Logf("DEATH", "pos=%s scene=%08x play=%d", pstr, sceneId, playing ? 1 : 0);
        else
            TF2Bridge_Logf("RESPAWN", "pos=%s scene=%08x hp=%d", pstr, sceneId, health);
    }
    sLogHealth = health;

    if (pos == NULL || sc == NULL)
        return;

    if (sBridgeClock >= sLogNextSpot)
    {
        sLogNextSpot = sBridgeClock + 15.0f;
        char pstr[64];
        TF2Bridge_LogPos(pstr, sizeof(pstr), pos, scale);
        TF2Bridge_Logf("SPOT", "pos=%s scene=%08x hp=%d ents=%d play=%d scale=%.0f",
            pstr, sceneId, health, (int)sc->num_ents, playing ? 1 : 0, (double)scale);
    }

    if (sBridgeClock < sLogNextScan)
        return;
    sLogNextScan = sBridgeClock + 0.5f;
    ++sLogScan;

    const F32 wide = 512.0f / scale; // Source units, in BFBB units
    const F32 near = 128.0f / scale;
    const F32 wide2 = wide * wide;
    const F32 near2 = near * near;

    int refused = 0;
    U32 firstRefused = 0;

    for (S32 i = 0; i < (S32)sc->num_ents; ++i)
    {
        xEnt* ent = sc->ents[i];
        const xVec3* pEnt = TF2Bridge_EntPos(ent);

        if (pEnt == NULL)
        {
            if (TF2Bridge_EntUnreadable(ent))
            {
                if (refused == 0)
                    firstRefused = (U32)(UPtr)ent;
                ++refused;
            }
            continue;
        }

        int kindIndex = -1;
        for (int k = 0; k < kLogKindCount; ++k)
        {
            if (sLogKinds[k] == (int)ent->baseType)
            {
                kindIndex = k;
                break;
            }
        }
        if (kindIndex < 0)
            continue;

        const bool scenery = (sLogKinds[kindIndex] == eBaseTypeStatic ||
                              sLogKinds[kindIndex] == eBaseTypeDynamic);
        if (scenery && sLogSceneryLines >= 4000)
            continue;

        const F32 dx = pEnt->x - pos->x;
        const F32 dy = pEnt->y - pos->y;
        const F32 dz = pEnt->z - pos->z;
        const F32 d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > (scenery ? near2 : wide2))
            continue;

        // Seen lately? Ten scans is five seconds of standing still out of range
        // before the same object is worth another line.
        bool seen = false;
        int slot = -1;
        int oldest = 0;
        for (int s = 0; s < (int)(sizeof(sLogSeen) / sizeof(sLogSeen[0])); ++s)
        {
            if (sLogSeen[s].id == 0)
            {
                if (slot < 0)
                    slot = s;
                continue;
            }
            if (sLogSeen[s].id == ent->id)
            {
                // Still here: restamp on sight, so the ten-scan window means "out
                // of range for five seconds" and not "five seconds since the line
                // was written". Without the restamp everything within reach was
                // written again every five seconds, which is what filled the first
                // real session log with the same sixty objects over and over.
                sLogSeen[s].scan = sLogScan;
                seen = true;
                slot = s;
                break;
            }
            if (sLogSeen[s].scan < sLogSeen[oldest].scan)
                oldest = s;
        }
        if (seen)
            continue;
        if (slot < 0)
            slot = oldest; // full: take the stalest
        sLogSeen[slot].id = ent->id;
        sLogSeen[slot].scan = sLogScan;

        // Is the player inside this object's box in XY, and is its top above him or
        // below him? That is the difference between "walked past a platform" and
        // "stood on one and fell through it".
        int inside = 0;
        F32 boxUp = 0.0f;
        xBox box;
        if (TF2Bridge_EntBox(ent, box))
        {
            inside = (pos->x >= box.lower.x && pos->x <= box.upper.x &&
                      pos->z >= box.lower.z && pos->z <= box.upper.z) ? 1 : 0;
            boxUp = (box.upper.y - pos->y) * scale;
        }

        const char* name = TF2Bridge_BaseTypeName(sLogKinds[kindIndex]);
        TF2Bridge_Logf("NEAR", "kind=%s id=%08x sub=%d d=%.0f up=%.0f in=%d top=%.0f col=%s",
            name != NULL ? name : "?", ent->id, (int)ent->subType,
            (double)(sqrtf(d2) * scale), (double)(dy * scale), inside, (double)boxUp,
            (ent->collType != 0) ? "col" : "-col");

        if (scenery)
            ++sLogSceneryLines;
    }

    // Not a fault any more, but worth a line: it means the scene handed the walk
    // something it could not read, and knowing how often is the difference
    // between a leftover pointer and a scene that is being torn down under it.
    if (refused > 0 && sBridgeClock >= sLogNextRefuseWarn)
    {
        sLogNextRefuseWarn = sBridgeClock + 10.0f;
        TF2Bridge_Logf("WARN", "walk skipped %d unreadable entity pointers (scene %08x ents=%d first=%08x)",
            refused, sceneId, (int)sc->num_ents, firstRefused);
    }
}

// ---------------------------------------------------------------------------
// Level audit (BFBB_TF2BRIDGE_AUDIT=1).
//
// The bridge mirrors JSP collision and NPC bounds, and nothing else. On the test
// level that is invisible; on a real one it is the whole difference between the
// game and a walkthrough -- a PLAT is not a surface you stand on here, a trigger
// volume is not entered, a hazard that hurts by collision never fires, and a warp
// box does not know you are in it. None of that is a wrong line of code: it is work
// that has not been done. So the useful instrument is not a fix, it is a list:
// what the level is made of where the player is standing, and what tried to hurt
// him and got refused.
//
// Off by default. On, it prints one line a second plus a line per scene change and
// per refused hit -- a test instrument, not something to leave in a play session.
// Distances are Source units (via the scale TF2 sends), so they compare with the
// tf2bridge convars instead of with BFBB's own numbers.
static bool TF2Bridge_AuditOn()
{
    static int sCached = -1;
    if (sCached < 0)
    {
        const char* v = getenv("BFBB_TF2BRIDGE_AUDIT");
        sCached = (v != NULL && v[0] != '\0' && v[0] != '0') ? 1 : 0;
    }
    return sCached != 0;
}

static const char* TF2Bridge_BaseTypeName(int t)
{
    switch (t)
    {
    case eBaseTypeTrigger: return "TRIG";
    case eBaseTypePickup: return "PICK";
    case eBaseTypePlatform: return "PLAT";
    case eBaseTypeDoor: return "DOOR";
    case eBaseTypeSavePoint: return "SAVE";
    case eBaseTypeItem: return "ITEM";
    case eBaseTypeStatic: return "STAT";
    case eBaseTypeDynamic: return "DYN";
    case eBaseTypePendulum: return "PEND";
    case eBaseTypeHangable: return "HANG";
    case eBaseTypeButton: return "BUTN";
    case eBaseTypeDestructObj: return "DSTR";
    case eBaseTypeVolume: return "VOL";
    case eBaseTypePortal: return "PORT";
    case eBaseTypeTeleportBox: return "TELE";
    case eBaseTypeBusStop: return "BUS";
    case eBaseTypeBoulder: return "BOLD";
    case eBaseTypeCruiseBubble: return "CRUI";
    case eBaseTypeNPC: return "NPC";
    case eBaseTypeVillain: return "VILL";
    default: return NULL;
    }
}

// The kinds the audit reports individually. Everything else is folded into 'other'
// so one line stays readable: these are the ones a real level is made of and the
// ones the bridge does not collide.
static const int sAuditKinds[] = {
    eBaseTypePlatform, eBaseTypeTrigger, eBaseTypePickup, eBaseTypeButton,
    eBaseTypeDestructObj, eBaseTypeSavePoint, eBaseTypeTeleportBox, eBaseTypePortal,
    eBaseTypeVolume, eBaseTypeHangable, eBaseTypePendulum, eBaseTypeDynamic,
    eBaseTypeVillain, eBaseTypeNPC,
};
static const int kAuditKindCount = (int)(sizeof(sAuditKinds) / sizeof(sAuditKinds[0]));

static void TF2Bridge_AuditFrame(uint32_t sceneId, bool playing)
{
    static uint32_t sAuditScene = 0;
    static F32 sAuditLastSeen = -1.0f; // clock when the current scene was last there
    static F32 sAuditNextLine = 0.0f;

    // The scene tracking runs whether or not the audit is on: the reset inside it is
    // a correctness fix rather than a diagnostic, and it has to happen on every level
    // load, including the ones nobody is watching the console for.
    if (sceneId != sAuditScene || sAuditLastSeen < 0.0f)
    {
        const bool first = (sAuditLastSeen < 0.0f);

        if (!TF2Bridge_AuditOn())
        {
            // Nothing to say; the reset below still has to happen.
        }
        else if (sAuditLastSeen < 0.0f)
        {
            printf("[bfbb] audit scene %08x (first)\n", sceneId);
        }
        else
        {
            // The gap is the load: from the last frame the old scene was there to the
            // first frame the new one is. 0 means no gap was seen at all.
            printf("[bfbb] audit scene %08x -> %08x, %.2f s unseen (playing=%d)\n",
                sAuditScene, sceneId, (double)MAX(0.0f, sBridgeClock - sAuditLastSeen),
                playing ? 1 : 0);
        }

        // The session log's progression line, and it is written whether or not the
        // audit is on: a level change is the one event a whole session is built on.
        {
            const int ents = globals.sceneCur != NULL ? (int)globals.sceneCur->num_ents : 0;
            if (first)
            {
                TF2Bridge_Logf("LEVEL", "to=%08x from=none ents=%d play=%d",
                    sceneId, ents, playing ? 1 : 0);
            }
            else
            {
                TF2Bridge_Logf("LEVEL", "to=%08x from=%08x gap=%.2fs ents=%d play=%d",
                    sceneId, sAuditScene,
                    (double)MAX(0.0f, sBridgeClock - sAuditLastSeen), ents, playing ? 1 : 0);
            }
        }

        sAuditScene = sceneId;
        sAuditNextLine = sBridgeClock; // report the new scene at once
        BridgeResetAfterSceneChange();
    }
    sAuditLastSeen = sBridgeClock;

    if (!TF2Bridge_AuditOn() || sBridgeClock < sAuditNextLine)
        return;
    sAuditNextLine = sBridgeClock + 1.0f;

    zScene* sc = globals.sceneCur;
    const xVec3* pPlayer = TF2Bridge_PlayerPos();
    if (sc == NULL || sc->ents == NULL || pPlayer == NULL)
        return;

    const BridgeIntentPacket* in = PuppetIntent();
    const F32 scale = (in != NULL && in->scale > 0.0f) ? in->scale : 50.0f;
    const F32 reach = 512.0f / scale; // 512 Source units, in BFBB units
    const F32 reach2 = reach * reach;

    const xVec3 pos = *pPlayer;

    int count[16];
    F32 best[16];
    F32 bestUp[16];
    int bestSub[16];
    int bestColl[16];
    for (int k = 0; k < kAuditKindCount; ++k)
    {
        count[k] = 0;
        best[k] = FLOAT_MAX;
        bestUp[k] = 0.0f;
        bestSub[k] = -1;
        bestColl[k] = 0;
    }
    int other = 0;

    for (S32 i = 0; i < (S32)sc->num_ents; ++i)
    {
        xEnt* ent = sc->ents[i];
        const xVec3* pEntPos = TF2Bridge_EntPos(ent);

        if (pEntPos == NULL)
            continue;

        const xVec3& q = *pEntPos;
        const F32 dx = q.x - pos.x;
        const F32 dy = q.y - pos.y;
        const F32 dz = q.z - pos.z;
        const F32 d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > reach2)
            continue;

        int kind = -1;
        for (int k = 0; k < kAuditKindCount; ++k)
        {
            if (sAuditKinds[k] == (int)ent->baseType)
            {
                kind = k;
                break;
            }
        }
        if (kind < 0)
        {
            other++;
            continue;
        }

        count[kind]++;
        const F32 d = sqrtf(d2) * scale;
        if (d < best[kind])
        {
            best[kind] = d;
            bestUp[kind] = dy * scale; // positive = above the player
            bestSub[kind] = (int)ent->subType;
            // Whether BFBB would collide this with anything at all. A trigger or a
            // pickup has no collide by design; a PLAT with 0 here is a platform the
            // engine drives around manually, which is also why it is invisible to TF2.
            bestColl[kind] = (ent->collType != 0) ? 1 : 0;
        }
    }

    char line[512];
    int used = snprintf(line, sizeof(line), "[bfbb] audit pos=(%.0f %.0f %.0f) scene=%08x ents=%d:",
        (double)(pos.x * scale), (double)(pos.y * scale), (double)(pos.z * scale),
        sceneId, (int)sc->num_ents);

    for (int k = 0; k < kAuditKindCount; ++k)
    {
        if (count[k] == 0)
            continue;

        const char* name = TF2Bridge_BaseTypeName(sAuditKinds[k]);
        if (used < 0 || used >= (int)sizeof(line))
            break;
        used += snprintf(line + used, sizeof(line) - used, " %s=%d@%.0f%s(%d,%s)",
            name != NULL ? name : "?", count[k], (double)best[k], bestUp[k] >= 0.0f ? "+" : "",
            bestSub[k], bestColl[k] ? "col" : "-col");
    }

    if (used >= 0 && used < (int)sizeof(line) && other > 0)
        snprintf(line + used, sizeof(line) - used, " other=%d", other);

    printf("%s\n", line);
}

// A hit that BFBB refused. On a real level this is how a hazard that is not in the
// bridge's collision list at all reads: nothing happens, and without this line the
// only symptom is that the spike trap did not hurt. One line per source per two
// seconds, because a flame in a refused state calls this every frame.
void zTF2Bridge_AuditRefusedHit(xBase* src, const char* reason)
{
    // Also the session log's line, so the throttle has to run when either has a
    // reader: a refusal is exactly the evidence that a hazard reached the damage
    // funnel and was gated, as opposed to never touching the player at all.
    if (!TF2Bridge_AuditOn() && !TF2Bridge_LogWanted())
        return;

    struct AuditRefusal
    {
        const xBase* src;
        F32 when;
    };
    static AuditRefusal sRefused[8];

    for (int i = 0; i < 8; ++i)
    {
        if (sRefused[i].src == src)
        {
            if (sBridgeClock - sRefused[i].when < 2.0f)
                return;
            sRefused[i].when = sBridgeClock;
            goto report;
        }
    }
    for (int i = 0; i < 8; ++i)
    {
        if (sRefused[i].src == NULL || sBridgeClock - sRefused[i].when > 10.0f)
        {
            sRefused[i].src = src;
            sRefused[i].when = sBridgeClock;
            break;
        }
    }

report:
    const char* name = src != NULL ? TF2Bridge_BaseTypeName((int)src->baseType) : NULL;
    if (TF2Bridge_AuditOn())
    {
        printf("[bfbb] audit hit refused (%s) src=%s id=%08x type=%08x\n",
            reason != NULL ? reason : "?", name != NULL ? name : "none",
            src != NULL ? src->id : 0u, src != NULL ? (unsigned)src->baseType : 0u);
    }

    TF2Bridge_Logf("REFUSED", "reason=%s src=%s id=%08x type=%08x",
        reason != NULL ? reason : "?", name != NULL ? name : "none",
        src != NULL ? src->id : 0u, src != NULL ? (unsigned)src->baseType : 0u);
}

zNPCCommon* zTF2Bridge_ParticleDamageSource(const xVec3* pos)
{
    if (!iTF2BridgeActive() || pos == NULL)
    {
        return NULL;
    }

    st_XORDEREDARRAY* npclist = zNPCMgr_GetNPCList();
    if (npclist == NULL)
    {
        return NULL;
    }

    zNPCCommon* closest = NULL;
    F32 closestSqr = 0.0f;
    for (S32 i = 0; i < npclist->cnt; ++i)
    {
        zNPCCommon* npc = (zNPCCommon*)npclist->list[i];
        if (npc == NULL || npc->frame == NULL || !npc->IsAlive())
            continue;

        switch (npc->SelfType())
        {
        case NPC_TYPE_TUBELET:
        case NPC_TYPE_TUBESLAVE:
        case NPC_TYPE_CHOMPER:
            break;
        default:
            continue;
        }

        const F32 dx = npc->frame->mat.pos.x - pos->x;
        const F32 dy = npc->frame->mat.pos.y - pos->y;
        const F32 dz = npc->frame->mat.pos.z - pos->z;
        const F32 distSqr = dx * dx + dy * dy + dz * dz;
        if (distSqr > kParticleSourceRange * kParticleSourceRange)
            continue;

        if (closest == NULL || distSqr < closestSqr)
        {
            closest = npc;
            closestSqr = distSqr;
        }
    }

    return closest;
}

// 0 when the robot is not listed, which leaves the hit on the chunk path.
static U32 BridgeRobotHitHealth(int selfType)
{
    const int count = (int)(sizeof(sRobotDamage) / sizeof(sRobotDamage[0]));
    for (int i = 0; i < count; ++i)
    {
        if (sRobotDamage[i].selfType == selfType)
        {
            return sRobotDamage[i].health;
        }
    }

    return 0;
}

void zTF2Bridge_ReportPlayerDamage(xBase* src, U32 damage)
{
    // Consumed whatever happens next: a refused hit must not leave the flag behind to
    // label the next robot hit as fire.
    const int hitKind = sHitKind;
    const bool bHitKindHasPos = sHitKindHasPos;
    const xVec3 hitKindPos = sHitKindPos;
    sHitKind = BRIDGE_HITKIND_NONE;
    sHitKindHasPos = false;

    // Afterburn exposure, as measured by NotePlayerHit (the only place that knows how
    // long it has been since the last tick of the same attack). Consumed here too, for
    // the same reason: a hit that goes no further must not set the next one on fire.
    const F32 burn = sPendingBurn;
    sPendingBurn = 0.0f;

    if (!iTF2BridgeActive() || damage == 0)
    {
        return;
    }

    BridgePlayerHitPacket hit;
    memset(&hit, 0, sizeof(hit));
    hit.sceneId = globals.sceneCur != NULL ? globals.sceneCur->sceneID : 0;
    hit.damage = (F32)damage;

    // BFBB has no afterburn of its own -- the flame only spends health while the
    // player stands in it -- so the burn is TF2's, and the hit carries the exposure
    // that feeds it. A blast is not fire, so it carries none.
    if (burn > 0.0f)
    {
        hit.burn = burn;
        hit.flags |= BRIDGE_PLAYER_HIT_BURN;
    }
    if (hitKind == BRIDGE_HITKIND_FIRE)
    {
        // Fire and gas never throw, even when the lookup below finds the robot. The
        // flag is sent by the particle, not inferred from the robot, so this holds
        // whether or not the hit could be attributed.
        hit.flags |= BRIDGE_PLAYER_HIT_NO_SHOVE;
    }
    else if (hitKind == BRIDGE_HITKIND_EXPLOSION)
    {
        // A blast always throws, whatever robot owns it and whether or not the owner
        // could be named at all.
        hit.flags |= BRIDGE_PLAYER_HIT_EXPLOSION;
    }

    // Where the hit came from. An NPC gives TF2 a real direction; anything else
    // (a surface, goo, a script) reports the player's own position.
    //
    // Robots are zNPCCommon, and zNPCCommon::Init leaves baseType as the level
    // asset's own: eBaseTypeNPC. eBaseTypeVillain is the older name for the same
    // family and shows up on some levels' entities, so both count -- testing for
    // Villain alone classified every robot hit as a hazard, which is why TF2
    // played no pain sound, drew no damage arrow and applied no shove.
    xVec3 from;
    from.x = from.y = from.z = 0.0f;
    if (globals.player.ent.frame != NULL)
    {
        from = *xEntGetPos(&globals.player.ent);
    }
    if (bHitKindHasPos)
    {
        // The particle or the blast centre itself is the source. It gives the damage
        // arrow a direction even when no robot was named, and for a blast it is the
        // point the shove has to point away from.
        from = hitKindPos;
    }
    if (src != NULL && (src->baseType == eBaseTypeNPC || src->baseType == eBaseTypeVillain))
    {
        hit.npcId = src->id;
        zNPCCommon* npc = (zNPCCommon*)src;
        hit.npcType = (U32)npc->SelfType();

        // A listed robot brings its own number, already in TF2 health, so TF2 must
        // not scale it again.
        const U32 robotHealth = BridgeRobotHitHealth((int)npc->SelfType());
        if (robotHealth != 0)
        {
            hit.damage = (F32)robotHealth;
            hit.flags |= BRIDGE_PLAYER_HIT_ABSOLUTE;
        }

        // Tubelet's two attacks share a robot but not a hit: the spiral is a tick and
        // the resurrection blast is one burst, so the blast keeps the bigger number
        // even though the table now carries the tick's.
        if (hitKind == BRIDGE_HITKIND_EXPLOSION &&
            (npc->SelfType() == NPC_TYPE_TUBELET || npc->SelfType() == NPC_TYPE_TUBESLAVE))
        {
            hit.damage = (F32)kTubeletBlastHealth;
            hit.flags |= BRIDGE_PLAYER_HIT_ABSOLUTE;
        }

        if (npc->frame != NULL)
        {
            from = npc->frame->mat.pos;

            // The attack's own direction, for the hits that land inside the player
            // (a hammer swing, a bomb bot bursting in their face). "Away from the
            // robot" is undefined at that range, and reversing the player's movement
            // throws anyone running away from a chaser back into it. The robot is
            // attacking, so it is facing the player: this is the missing direction.
            hit.dirX = npc->frame->mat.at.x;
            hit.dirY = npc->frame->mat.at.y;
            hit.dirZ = npc->frame->mat.at.z;
        }
    }
    hit.x = from.x;
    hit.y = from.y;
    hit.z = from.z;

    // The chunk path keeps BFBB's own meaning: a hit worth all the health left is a
    // pit or goo and kills outright. A robot's number is TF2 health, and TF2 decides
    // whether that kills, so the flag is not set for one.
    if (!(hit.flags & BRIDGE_PLAYER_HIT_ABSOLUTE) && damage >= globals.player.Health)
    {
        hit.flags |= BRIDGE_PLAYER_HIT_LETHAL;
    }

    // Unconditional, one line per accepted hit: the TF2-side log says what TF2 did
    // with a hit, and this says what BFBB actually sent. Without both, "the flame is
    // still silent" cannot be told apart from "the flame never reached the bridge".
    {
        char kind = '?';
        if (hit.flags & BRIDGE_PLAYER_HIT_NO_SHOVE)
            kind = 'F'; // fire/gas
        else if (hit.flags & BRIDGE_PLAYER_HIT_EXPLOSION)
            kind = 'X'; // explosion

        printf("[bfbb] hit id=%08x type=%08x flags=%u dmg=%.0f burn=%.2f kind=%c src=%.0f,%.0f,%.0f\n",
               hit.npcId, hit.npcType, hit.flags, hit.damage, hit.burn, kind, hit.x, hit.y, hit.z);

        // The same fact in the session log, next to the level's own line for the same
        // second: "the goo killed me" is answered by a HIT with kind '?' and the NEAR
        // line that says no robot was anywhere near.
        const F32 logScale = TF2Bridge_LiveScale();
        char pstr[64];
        TF2Bridge_LogPos(pstr, sizeof(pstr), TF2Bridge_PlayerPos(), logScale);
        TF2Bridge_Logf("HIT", "id=%08x type=%08x flags=%u dmg=%.0f burn=%.2f kind=%c src=(%.0f %.0f %.0f) pos=%s",
            hit.npcId, hit.npcType, hit.flags, (double)hit.damage, (double)hit.burn, kind,
            (double)(hit.z * logScale), (double)(hit.x * logScale), (double)(hit.y * logScale), pstr);
    }

    iTF2BridgeSendPlayerHit(&hit);
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

// The critters: jellyfish, the only non-robot NPCs the bridge hands to TF2 so
// far. They are zNPCAmbient, not zNPCRobot, which is the whole reason they were
// invisible to every path below -- a shot went through one and it never appeared on
// TF2's side. Nothing else about them is special: they carry hitpoints from their
// ini, `Damage` runs the ordinary NPC damage path (which makes a bumped jelly
// angry), and their model is exported by the same DumpNpcModel, so admitting them
// here is all a hull and a proxy take.
//
// They are deliberately not robots: the robot-only blocks (the knockback override,
// the per-type damage table's robot half) are about goals and hovers that ambients
// do not have.
static bool IsTF2BridgeCritter(const zNPCCommon* npc)
{
    if (npc == NULL)
        return false;

    switch (npc->SelfType())
    {
    case NPC_TYPE_JELLYPINK:
    case NPC_TYPE_JELLYBLUE:
        return true;
    default:
        return false;
    }
}

// Everything the bridge mirrors into TF2 as a hittable target. The robot check stays
// separate above because several places need "is this one of the robots" rather
// than "can this be shot".
static bool IsTF2BridgeTarget(const zNPCCommon* npc)
{
    return IsTF2BridgeRobot(npc) || IsTF2BridgeCritter(npc);
}

static void TF2Bridge_ProcessNpcHits()
{
    BridgeNpcHitPacket hit;
    while (iTF2BridgePopNpcHit(&hit))
    {
        // BFBB's existing rocket sweep already owns blast damage/falloff. Keep
        // its result for rockets while proxy-reported direct hits cover other
        // TF2 weapon damage events.
        if (hit.npcId == 0 ||
            hit.sceneId != (globals.sceneCur != NULL ? globals.sceneCur->sceneID : 0) ||
            !(hit.damage > 0.0f) || globals.sceneCur == NULL)
            continue;

        st_XORDEREDARRAY* npclist = zNPCMgr_GetNPCList();
        if (npclist == NULL)
            continue;

        zNPCCommon* target = NULL;
        for (S32 i = 0; i < npclist->cnt; ++i)
        {
            zNPCCommon* npc = (zNPCCommon*)npclist->list[i];
            if (npc != NULL && npc->id == hit.npcId && IsTF2BridgeTarget(npc))
            {
                target = npc;
                break;
            }
        }
        if (target == NULL || !target->IsAlive() || target->frame == NULL)
            continue;

        xVec3 dir = { hit.dirX, hit.dirY, hit.dirZ };
        if (xVec3Length2(&dir) < 0.0001f)
        {
            dir.x = 0.0f; dir.y = 0.0f; dir.z = 1.0f;
        }
        xVec3Normalize(&dir, &dir);

        float requested = hit.knockbackMultiplier;
        if (!(requested >= 0.1f && requested <= 5.0f))
            requested = 1.5f;
        const float referenceDamage = hit.baseDamage > 0.0f ? hit.baseDamage : hit.damage;
        const bool critical = (hit.hitFlags & BRIDGE_HIT_CRITICAL) != 0;
        const float nonCritDamage = critical ? hit.damage / 3.0f : hit.damage;
        const float falloffScale = MIN(2.0f, MAX(1.0f, referenceDamage / nonCritDamage));
        const float critScale = critical ? 3.0f : 1.0f;
        const float chuckScale = TF2Bridge_ImpactKnockbackMultiplier(target, 1.0f, nonCritDamage);
        sPendingKnockbackDirX = dir.x;
        sPendingKnockbackDirY = dir.y;
        sPendingKnockbackDirZ = dir.z;
        sPendingKnockbackMultiplier = MIN(5.0f, requested * falloffScale * critScale * chuckScale);
        sPendingKnockbackTarget = target;
        sPendingKnockbackOverride = true;

        target->Damage(DMGTYP_SIDE, NULL, &dir);
        if (target->SelfType() == NPC_TYPE_CHUCK && target->IsAlive() && target->frame != NULL &&
            sPendingKnockbackOverride && sPendingKnockbackTarget == target)
        {
            target->frame->vel.x += dir.x * 2.5f * sPendingKnockbackMultiplier;
            target->frame->vel.z += dir.z * 2.5f * sPendingKnockbackMultiplier;
            sPendingKnockbackTarget = NULL;
            sPendingKnockbackOverride = false;
        }
        printf("bfbb: tf2bridge -- proxy hit id=%u type=%d damage=%.2f base=%.2f typebits=0x%08x kb=%.2f\n",
            (unsigned)hit.npcId, (int)target->SelfType(), (double)hit.damage,
            (double)referenceDamage, (unsigned)hit.damageType, (double)sPendingKnockbackMultiplier);

        // In the session log as well: whether shots land is the one thing a play
        // session cannot show from memory, and a robot that never died is either
        // "the shot missed the proxy" (no line) or "the shot landed for nothing".
        TF2Bridge_Logf("HITNPC", "id=%u type=%d dmg=%.2f base=%.2f kb=%.2f alive=%d",
            (unsigned)hit.npcId, (int)target->SelfType(), (double)hit.damage,
            (double)referenceDamage, (double)sPendingKnockbackMultiplier,
            target->IsAlive() ? 1 : 0);
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
        if (!IsTF2BridgeTarget(npc) || !npc->IsAlive())
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

    sPendingKnockbackOverride = false;
    sPendingKnockbackTarget = NULL;
    if (IsTF2BridgeRobot(best))
    {
        const float pushLen = sqrtf(dir.x * dir.x + dir.z * dir.z);
        sPendingKnockbackDirX = pushLen > 0.001f ? dir.x / pushLen : 0.0f;
        sPendingKnockbackDirZ = pushLen > 0.001f ? dir.z / pushLen : 1.0f;
        float requested = in->knockbackMultiplier;
        if (!(requested >= 0.1f && requested <= 5.0f)) requested = 1.5f;
        sPendingKnockbackMultiplier = requested;
        if (best->SelfType() == NPC_TYPE_CHUCK && in->weaponDamage > 0.0f)
        {
            const float overflowDamage = MAX(0.0f, in->weaponDamage / 35.0f - (float)MAX(1, ((zNPCRobot*)best)->hitpoints));
            sPendingKnockbackMultiplier = MIN(5.0f, requested * (1.0f + MIN(2.0f, overflowDamage * 0.5f)));
        }
        sPendingKnockbackTarget = best;
        sPendingKnockbackOverride = true;
    }
    best->Damage(DMGTYP_SIDE, NULL, &dir);
    if (best->SelfType() == NPC_TYPE_CHUCK && best->IsAlive() && best->frame != NULL &&
        sPendingKnockbackOverride && sPendingKnockbackTarget == best)
    {
        best->frame->vel.x += dir.x * 2.5f * sPendingKnockbackMultiplier;
        best->frame->vel.z += dir.z * 2.5f * sPendingKnockbackMultiplier;
        sPendingKnockbackTarget = NULL;
        sPendingKnockbackOverride = false;
    }
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
        if (!IsTF2BridgeTarget(npc) || !npc->IsAlive())
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
    if (IsTF2BridgeRobot(best))
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

        // Use the item's resolved attribute, then apply Chuck's capped
        // overflow bonus using the estimated damage and his pre-hit HP.
        sPendingKnockbackMultiplier =
            TF2Bridge_ImpactKnockbackMultiplier(best, in->knockbackMultiplier, in->weaponDamage);

        sPendingKnockbackTarget = best;
        sPendingKnockbackOverride = true;
    }

    // BFBB's normal damage path still owns HP, damage goal, death animation,
    // rewards, and native collision/bounce handling.
    best->Damage(DMGTYP_SIDE, NULL, &dir);
    // Chuck's hover logic doesn't use the normal gravity launch. Apply a direct
    // horizontal impulse scaled by the active weapon and any estimated overflow.
    if (best->SelfType() == NPC_TYPE_CHUCK && best->IsAlive() && best->frame != NULL &&
        sPendingKnockbackOverride && sPendingKnockbackTarget == best)
    {
        // Only take this path if Chuck's custom goal did not consume the normal
        // override during Damage(); avoid applying the same impulse twice.
        best->frame->vel.x += dir.x * 2.5f * sPendingKnockbackMultiplier;
        best->frame->vel.z += dir.z * 2.5f * sPendingKnockbackMultiplier;
        sPendingKnockbackTarget = NULL;
        sPendingKnockbackOverride = false;
    }
    if (IsTF2BridgeRobot(best))
        printf("bfbb: tf2bridge -- knockback offered type=%d target=%p mult=%.2f\n",
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

    // The player's bound has to stay a SPHERE, and it has to be rebuilt here rather
    // than left to the game, because the game rebuilt it from the position of the
    // frame's own (zeroed) movement and this is the position that actually happened.
    //
    // This used to be TF2's hull as a BOX, and that one change was two visible bugs.
    // xBound is a union, so `.sph.r` and `.box.box.lower.x` are the same four bytes:
    //
    //   - The pickup magnet and its collect radius are `plent->bound.sph.r` and
    //     `4 * plent->bound.sph.r` (zEntPickup), so the radius was reading the box's
    //     lower x -- a world coordinate, ~8 BFBB units here. Shinies were seized
    //     from 1280 Source units away, which is why socks and shiny objects were
    //     collected instantly instead of drifting toward the player.
    //   - PlayerCollCheckEnv sets `.sph.r` to 0.65 for the world sweep and back to
    //     0.5 every frame, which on a box writes `lower.x`. xBoundUpdate then took
    //     the box centre as (lower + upper) / 2 -- halfway between the origin and
    //     the player -- and that is the position every bound-based trigger and
    //     hazard test was using. Triggers fired in rooms the player was not in.
    //
    // The shape is now the one the game made (PlayerBoundUpdate): radius 0.5, centre
    // 0.5 above the feet, which is a merc's worth of clearance -- 20 Source units
    // against TF2's 16. Crouching no longer buys a tighter fit against a robot; that
    // refinement needs a per-shape bound, not a union field the game still writes.
    e.bound.type = XBOUND_TYPE_SPHERE;
    e.bound.sph.center = p;
    e.bound.sph.center.x += globals.player.RootUp.x * 0.3f;
    e.bound.sph.center.y += globals.player.RootUp.y * 0.3f;
    e.bound.sph.center.z += globals.player.RootUp.z * 0.3f;
    e.bound.sph.center.y += 0.2f;
    e.bound.sph.r = 0.5f;
    xBoundUpdate(&e.bound);

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

// Whether BFBB should draw SpongeBob while TF2 owns the camera. That camera sits
// inside him, so in first person his model only fills the view with its own
// geometry; bfbb_player_visible 1 on the TF2 console brings it back for debugging.
// Visibility only: the collision handover in zTF2Bridge_AfterPlayerUpdate happens
// either way, so showing the model cannot change what the player can walk into.
// Whether TF2 is moving and aiming the player this frame. Game code that has to
// wait on a BFBB animation asks this, because an animation TF2 cannot finish is
// exactly what a control-off looks like from the player's side.
bool zTF2Bridge_DrivesPlayer()
{
    return PuppetIntent() != NULL;
}

bool zTF2Bridge_HidesPlayerModel()
{
    const BridgeIntentPacket* in = PuppetIntent();
    if (in == NULL)
    {
        return false;
    }
    return (in->flags & BRIDGE_INTENT_SHOW_PLAYER) == 0;
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
