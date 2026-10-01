#include "zNMETypeMiniMerv.h"

#include "zEntPlayer.h"
#include "zGlobals.h"
#include "zMovePoint.h"
#include "zNPCTypeRobot.h"
#include <math.h>

static const S32 MINI_MERV_MUZZLE_BONE = 28;

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
    zap_visible = 0;
    pad[0] = 0;
    zap_visual_timer = 0.0f;

    zap_beam.Prepare();
    zap_beam.TextureSet(NPCC_FindRWRaster("fx_solid"));
    zap_beam.RadiusSet(0.16f, 0.16f);
    RwRGBA zap_color = { 80, 220, 255, 255 };
    zap_beam.ColorSet(&zap_color, &zap_color);

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
    zap_visible = 0;
    zap_visual_timer = 0.0f;
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

void zNMEMiniMerv::GetMuzzlePos(xVec3* pos) const
{
    *pos = *xEntGetPos((xEnt*)this);

    // Mat[28] is the evaluated animated slot identified by the Mini Merv
    // animation sweep as the lowest, swaying laser assembly/muzzle.
    // Mat[] positions are model-local, so transform it through the entity frame.
    if (frame != NULL && model != NULL && model->Mat != NULL && model->BoneCount >= MINI_MERV_MUZZLE_BONE)
    {
        const xVec3& local = *(const xVec3*)&model->Mat[MINI_MERV_MUZZLE_BONE].pos;
        *pos = frame->mat.pos;
        pos->x += frame->mat.right.x * local.x + frame->mat.up.x * local.y + frame->mat.at.x * local.z;
        pos->y += frame->mat.right.y * local.x + frame->mat.up.y * local.y + frame->mat.at.y * local.z;
        pos->z += frame->mat.right.z * local.x + frame->mat.up.z * local.y + frame->mat.at.z * local.z;
    }
}

void zNMEMiniMerv::FireWarningBeam()
{
    xVec3 start;
    GetMuzzlePos(&start);

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
    zap_visible = 1;
    zap_visual_timer = 0.20f;
    cooldown_timer = cooldown_time;
}

void zNMEMiniMerv::UpdateZapBeam()
{
    if (!zap_visible)
    {
        return;
    }

    xVec3 start;
    GetMuzzlePos(&start);

    xVec3 target = *xEntGetPos(&globals.player.ent);
    zap_beam.Render(&start, &target);
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

    if (zap_visual_timer > 0.0f)
    {
        zap_visual_timer = MAX(0.0f, zap_visual_timer - dt);
        if (zap_visual_timer <= 0.0f)
        {
            zap_visible = 0;
        }
    }

    if (warning_beam.visible() || zap_visible)
    {
        flg_xtrarend |= 0x1;
    }
}

void zNMEMiniMerv::RenderExtra()
{
    warning_beam.render();
    UpdateZapBeam();
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
