#include "zNMETypeMiniMerv.h"

#include "zEntPlayer.h"
#include "zGlobals.h"
#include <math.h>

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
    detect_radius = 18.0f;
    danger_radius = 10.0f;
    warning_time = 1.25f;
    cooldown_time = 0.75f;
    zap_timer = 0.0f;
    cooldown_timer = 0.0f;
    warning_active = 0;
    zap_fired = 0;
    pad[0] = pad[1] = 0;

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

bool zNMEMiniMerv::TargetInDetectRange() const
{
    const xVec3* npc_pos = xEntGetPos((xEnt*)this);
    const xVec3* plyr_pos = xEntGetPos(&globals.player.ent);
    return xVec3Dist2(npc_pos, plyr_pos) <= detect_radius * detect_radius;
}

bool zNMEMiniMerv::TargetInDangerRange() const
{
    const xVec3* npc_pos = xEntGetPos((xEnt*)this);
    const xVec3* plyr_pos = xEntGetPos(&globals.player.ent);
    return xVec3Dist2(npc_pos, plyr_pos) <= danger_radius * danger_radius;
}

void zNMEMiniMerv::FireWarningBeam()
{
    xVec3 start = *xEntGetPos((xEnt*)this);
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

    if (!TargetInDetectRange())
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

    if (globals.player.Health > 0)
    {
        xVec3 dir;
        xVec3Sub(&dir, xEntGetPos(&globals.player.ent), xEntGetPos((xEnt*)this));
        dir.y = 0.0f;
        if (xVec3Length2(&dir) > 0.0001f)
        {
            TurnToFace(dt, &dir, -1.0f);
        }
    }
    warning_beam.update(dt);

    if (warning_beam.visible())
    {
        flg_xtrarend |= 0x1;
    }
}

void zNMEMiniMerv::RenderExtra()
{
    warning_beam.render();
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
