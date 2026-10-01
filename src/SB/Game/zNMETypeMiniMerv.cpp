#include "zNMETypeMiniMerv.h"

#include "zEntPlayer.h"
#include "zGlobals.h"
#include "zMovePoint.h"
#include <math.h>
#include <stdio.h>
#include <rphanim.h>

static const U32 MINI_MERV_MUZZLE_BONE = 26;

static RwFrame* MiniMervFindHAnim(RwFrame* frame, void* data)
{
    if (frame == NULL)
    {
        return NULL;
    }

    RpHAnimHierarchy** found = (RpHAnimHierarchy**)data;
    RpHAnimHierarchy* hierarchy = RpHAnimFrameGetHierarchy(frame);

    if (hierarchy != NULL)
    {
        *found = hierarchy;
        return frame;
    }

    RwFrameForAllChildren(frame, MiniMervFindHAnim, data);
    return frame;
}

static void MiniMervDumpBones(xModelInstance* model)
{
    if (model == NULL || model->Data == NULL)
    {
        printf("[MiniMervBoneDump] no model data\\n");
        return;
    }

    RpAtomic* atomic = model->Data;
    RpHAnimHierarchy* hierarchy = NULL;
    RwFrame* root = (RwFrame*)atomic->object.object.parent;

    MiniMervFindHAnim(root, &hierarchy);

    if (hierarchy == NULL)
    {
        printf("[MiniMervBoneDump] no HAnim hierarchy\\n");
        return;
    }

    printf("[MiniMervBoneDump] numNodes=%d\\n", hierarchy->numNodes);

    for (S32 i = 0; i < hierarchy->numNodes; ++i)
    {
        const RpHAnimNodeInfo& node = hierarchy->pNodeInfo[i];
        xVec3 pos = xModelGetBoneLocation(*model, (U32)i);

        printf("[MiniMervBoneDump] slot=%d nodeID=%d nodeIndex=%d flags=0x%08X "
               "pos=(%.3f, %.3f, %.3f)",
               i, node.nodeID, node.nodeIndex, (U32)node.flags,
               pos.x, pos.y, pos.z);

        if (node.nodeIndex >= 0 && node.nodeIndex < hierarchy->numNodes &&
            node.nodeIndex != i)
        {
            xVec3 mapped = xModelGetBoneLocation(*model, (U32)node.nodeIndex);
            printf(" mappedPos=(%.3f, %.3f, %.3f)",
                   mapped.x, mapped.y, mapped.z);
        }

        printf("\\n");
    }
}



static void MiniMervRenderBoneAxis(xModelInstance* model, S32 bone)
{
    if (model == NULL || bone < 0 || bone >= 28)
        return;

    xMat4x3 mat;
    xModelGetBoneMat(mat, *model, (size_t)bone);

    const F32 len = 0.9f;
    RwIm3DVertex verts[6];

    const xVec3 points[6] =
    {
        mat.pos,
        { mat.pos.x + mat.right.x * len, mat.pos.y + mat.right.y * len, mat.pos.z + mat.right.z * len },
        mat.pos,
        { mat.pos.x + mat.up.x * len, mat.pos.y + mat.up.y * len, mat.pos.z + mat.up.z * len },
        mat.pos,
        { mat.pos.x + mat.at.x * len, mat.pos.y + mat.at.y * len, mat.pos.z + mat.at.z * len }
    };

    const U8 colors[3][3] =
    {
        {255, 60, 60},
        {60, 255, 60},
        {60, 120, 255}
    };

    for (S32 i = 0; i < 6; ++i)
    {
        RwIm3DVertexSetPos(&verts[i], points[i].x, points[i].y, points[i].z);
        S32 axis = i / 2;
        RwIm3DVertexSetRGBA(&verts[i], colors[axis][0], colors[axis][1], colors[axis][2], 255);
    }

    RwRenderStateSet(rwRENDERSTATETEXTURERASTER, NULL);
    RwRenderStateSet(rwRENDERSTATEZTESTENABLE, (void*)FALSE);
    RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)FALSE);
    RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)TRUE);

    if (RwIm3DTransform(verts, 6, NULL, rwIM3D_VERTEXXYZ | rwIM3D_VERTEXRGBA) != NULL)
        RwIm3DRenderPrimitive(rwPRIMTYPELINELIST);
}

static xFactoryInst* MiniMervGoalCreate(S32 who, RyzMemGrow* grow, void*)
{
    if (who == zNMEMiniMerv::GOAL_ZAP)
    {
        return new (who, grow) zNMEGoalMiniMervZap(who);
    }

    return NULL;
}

static void MiniMervGoalDestroy(xFactoryInst* inst)
{
    delete inst;
}

void zNME_Register_MiniMervGoal()
{
    static bool registered = false;
    if (registered)
    {
        return;
    }

    xFactory* factory = xBehaveMgr_GetSelf()->GetFactory();
    if (factory != NULL)
    {
        factory->RegItemType(zNMEMiniMerv::GOAL_ZAP, MiniMervGoalCreate, MiniMervGoalDestroy);
        registered = true;
    }
}

S32 zNMEGoalMiniMervZap::Process(en_trantype*, F32 dt, void* ctxt, xScene*)
{
    zNMEMiniMerv* npc = (zNMEMiniMerv*)ctxt;
    if (npc != NULL)
    {
        npc->UpdateZap(dt);
    }

    return 0;
}

zNMEMiniMerv::zNMEMiniMerv(S32 myType) : zNMEStandard(myType)
{
    focus_radius = 18.0f;
    danger_radius = 10.0f;
    warning_time = 1.25f;
    cooldown_time = 0.75f;
    zap_timer = 0.0f;
    cooldown_timer = 0.0f;
    warning_active = 0;
    zap_fired = 0;
    pad[0] = pad[1] = 0;

    bone_debug_timer = 0.0f;
    bone_debug_index = 0;

    warning_beam.init(8, "Mini Merv warning");
    warning_beam.set_texture("plankton_laser_bolt");
    warning_beam.cfg.radius = 0.08f;
    warning_beam.cfg.length = 12.0f;
    warning_beam.cfg.vel = 40.0f;
    warning_beam.cfg.fade_dist = 12.0f;
    warning_beam.cfg.kill_dist = 12.0f;
    warning_beam.cfg.safe_dist = 0.0f;
    warning_beam.cfg.hit_radius = 0.0f;
    warning_beam.cfg.rand_ang = 0.0f;
    warning_beam.cfg.scar_life = 0.0f;
    warning_beam.cfg.hit_interval = 0;
    warning_beam.cfg.damage = 0.0f;
    warning_beam.cfg.bolt_uv[0] = { 0.0f, 0.0f };
    warning_beam.cfg.bolt_uv[1] = { 1.0f, 1.0f };
    warning_beam.refresh_config();
}

void zNMEMiniMerv::Setup()
{
    zNMEStandard::Setup();

    if (nav_curr == NULL && npcass != NULL && npcass->movepoint != 0)
    {
        nav_curr = zMovePoint_From_xAssetID(npcass->movepoint);
    }

    if (psy_self != NULL)
    {
        psy_self->BrainBegin();
        psy_self->AddGoal(GOAL_ZAP, NULL);
        psy_self->BrainEnd();
        psy_self->GoalSet(GOAL_ZAP, 0);
    }
}

void zNMEMiniMerv::Reset()
{
    zNMEStandard::Reset();
    zap_timer = 0.0f;
    cooldown_timer = 0.0f;
    warning_active = 0;
    zap_fired = 0;
    warning_beam.reset();
}

bool zNMEMiniMerv::TargetInFocusRange() const
{
    const xVec3* npc_pos = xEntGetPos((xEnt*)this);
    const xVec3* plyr_pos = xEntGetPos(&globals.player.ent);

    F32 radius = focus_radius;
    if (nav_curr != NULL && nav_curr->asset != NULL && nav_curr->asset->arenaRadius > 0.0f)
    {
        const xVec3* mvpt_pos = zMovePointGetPos(nav_curr);
        radius = nav_curr->asset->arenaRadius;

        if (xVec3Dist2(mvpt_pos, plyr_pos) > radius * radius)
        {
            return false;
        }
    }
    else
    {
        if (xVec3Dist2(npc_pos, plyr_pos) > radius * radius)
        {
            return false;
        }
    }

    return true;
}

bool zNMEMiniMerv::TargetInDangerRange() const
{
    const xVec3* npc_pos = xEntGetPos((xEnt*)this);
    const xVec3* plyr_pos = xEntGetPos(&globals.player.ent);
    return xVec3Dist2(npc_pos, plyr_pos) <= danger_radius * danger_radius;
}

void zNMEMiniMerv::FireWarningBeam()
{
    static bool dumped_bones = false;
    if (!dumped_bones)
    {
        MiniMervDumpBones(model);
        dumped_bones = true;
    }

    xVec3 start = *xEntGetPos((xEnt*)this);

    if (model != NULL && MINI_MERV_MUZZLE_BONE < model->BoneCount)
    {
        start = xModelGetBoneLocation(*model, MINI_MERV_MUZZLE_BONE);
    }
    xVec3 target = *xEntGetPos(&globals.player.ent);
    xVec3 dir;
    xVec3Sub(&dir, &target, &start);

    F32 len2 = xVec3Length2(&dir);
    if (len2 <= 0.0001f)
    {
        return;
    }

    xVec3SMul(&dir, &dir, 1.0f / sqrtf(len2));
    warning_beam.emit(start, dir);
}

void zNMEMiniMerv::FireZap()
{
    zEntPlayer_Damage((xBase*)this, 1);
    zap_fired = 1;
    cooldown_timer = cooldown_time;
}

void zNMEMiniMerv::UpdateZap(F32 dt)
{
    if (globals.player.Health < 1)
    {
        warning_beam.reset();
        zap_timer = 0.0f;
        cooldown_timer = 0.0f;
        warning_active = 0;
        zap_fired = 0;
        return;
    }

    if (!TargetInFocusRange())
    {
        warning_beam.reset();
        zap_timer = 0.0f;
        warning_active = 0;
        zap_fired = 0;
        return;
    }

    if (cooldown_timer > 0.0f)
    {
        cooldown_timer = MAX(0.0f, cooldown_timer - dt);
    }

    if (!TargetInDangerRange())
    {
        zap_timer = 0.0f;
        warning_active = 0;
        zap_fired = 0;
        return;
    }

    if (cooldown_timer > 0.0f)
    {
        return;
    }

    if (!warning_active)
    {
        warning_active = 1;
        zap_fired = 0;
        zap_timer = warning_time;
    }

    FireWarningBeam();
    zap_timer -= dt;

    if (zap_timer <= 0.0f && !zap_fired)
    {
        FireZap();
        warning_active = 0;
    }
}

void zNMEMiniMerv::Process(xScene* xscn, F32 dt)
{
    zNMEStandard::Process(xscn, dt);

    if (globals.player.Health > 0 && TargetInFocusRange())
    {
        xVec3 dir;
        xVec3Sub(&dir, xEntGetPos(&globals.player.ent), xEntGetPos((xEnt*)this));
        dir.y = 0.0f;
        if (xVec3Length2(&dir) > 0.0001f)
        {
            xVec3Normalize(&dir, &dir);
            TurnToFace(dt, &dir, 4.0f);
            xEntMotionToMatrix((xEnt*)this, this->frame);
        }
    }
    warning_beam.update(dt);

    bone_debug_timer += dt;
    if (bone_debug_timer >= 1.0f)
    {
        bone_debug_timer -= 1.0f;
        bone_debug_index = (bone_debug_index + 1) % 28;
        printf("[MiniMervBoneAxis] bone=%d\n", bone_debug_index);
    }

    if (warning_beam.visible())
    {
        flg_xtrarend |= 0x1;
    }
}

void zNMEMiniMerv::RenderExtra()
{
    warning_beam.render();
    MiniMervRenderBoneAxis(model, bone_debug_index);
}

void zNMEMiniMerv::SelfDestroy()
{
    warning_beam.reset();
}

xFactoryInst* ZNME_Create_MiniMerv(S32 who, RyzMemGrow* grow, void*)
{
    if (who == NPC_TYPE_NME_TEST)
    {
        return new (who, grow) zNMEMiniMerv(who);
    }

    return NULL;
}

void ZNME_Destroy_MiniMerv(xFactoryInst* inst)
{
    delete (zNMEMiniMerv*)inst;
}
