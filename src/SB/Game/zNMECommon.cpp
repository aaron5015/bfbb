#include "zNMECommon.h"

#include "xFactory.h"
#include "xMath.h"
#include "zNPCTypes.h"
#include "zNMETypeMiniMerv.h"

static xFactoryInst* NMEGoalCreate(S32 who, RyzMemGrow* grow, void*)
{
    if (who == NME_GOAL_IDLE)
    {
        return new (who, grow) zNMEGoalIdle(who);
    }

    return NULL;
}

static void NMEGoalDestroy(xFactoryInst* inst)
{
    delete inst;
}

void zNME_RegisterGoals()
{
    xFactory* factory = xBehaveMgr_GetSelf()->GetFactory();
    if (factory != NULL)
    {
        factory->RegItemType(NME_GOAL_IDLE, NMEGoalCreate, NMEGoalDestroy);
        zNME_Register_MiniMervGoal();
    }
}

void zNME_UnregisterGoals()
{
}

zNMENavNet::zNMENavNet()
{
    Reset();
}

void zNMENavNet::Reset()
{
    nav_past = NULL;
    nav_curr = NULL;
    nav_dest = NULL;
    nav_lead = NULL;
    spl_mvptspline = NULL;
    len_mvptspline = 0.0f;
    dst_curspline = 0.0f;
    nme_owner = NULL;
}

NMECfgCommon::NMECfgCommon()
{
    tym_invuln = 0.0f;
    acc_grav = 0.0f;
    spd_maxFall = 100.0f;
    spd_move = 2.0f;
    acc_move = 4.0f;
    dst_deviant = 0.0f;
    spd_turnrate = 2.0f;
}

zNMEGoalCommon::zNMEGoalCommon(S32 goalID) : xGoal(goalID)
{
    anid_played = 0;
    flg_npcgauto = ~(1 << 3);
    flg_npcgable = 0;
    flg_info = 0;
    flg_user = 0;
}

void zNMEGoalCommon::Clear()
{
    flg_info = 0;
    flg_user = 0;
    xGoal::Clear();
}

S32 zNMEGoalCommon::Enter(F32 dt, void* updCtxt)
{
    return xGoal::Enter(dt, updCtxt);
}

S32 zNMEGoalCommon::Resume(F32 dt, void* updCtxt)
{
    return xGoal::Resume(dt, updCtxt);
}

S32 zNMEGoalCommon::PreCalc(F32 dt, void* updCtxt)
{
    return xGoal::PreCalc(dt, updCtxt);
}

U32 zNMEGoalCommon::DoAutoAnim(S32 gspot, S32 forceRestart)
{
    zNMECommon* npc = (zNMECommon*)psyche->clt_owner;
    if (npc == NULL)
    {
        return 0;
    }

    U32 anid = npc->AnimPick(GetID(), (en_NPC_GOAL_SPOT)gspot, this);
    if (anid != 0)
    {
        DoExplicitAnim(anid, forceRestart);
    }

    return anid_played;
}

U32 zNMEGoalCommon::DoExplicitAnim(U32 anid, S32 forceRestart)
{
    zNMECommon* npc = (zNMECommon*)psyche->clt_owner;
    if (npc != NULL && npc->AnimStart(anid, forceRestart))
    {
        anid_played = anid;
    }
    else
    {
        anid_played = 0;
    }

    if (flg_npcgauto & 0x8)
    {
        flg_info &= ~0x8;
    }

    return anid_played;
}

zNMECommon::zNMECommon(S32 myType) : zNPCCommon(myType)
{
    psy_self = NULL;
    cfg_common = NULL;
    flg_nmeVuln = 0;
    flg_nmeMove = 0;
    flg_nmeMisc = 0;
    flg_nmeAble = 0;
    spd_throttle = 0.0f;
    runtimeData.flags = 0;
    runtimeData.overrideDetect = 1;
    runtimeData.overrideAttack = 1;
    navnet.nme_owner = this;
    tmr_common[0] = tmr_common[1] = tmr_common[2] = 0.0f;
    tmr_scary = 0.0f;
    tmr_lastAlert = 0.0f;
    npc_duplodude = NULL;
}

void zNMECommon::Init(xEntAsset* asset)
{
    zNPCCommon::Init(asset);

    if (cfg_common == NULL)
    {
        cfg_common = new NMECfgCommon();
    }

    navnet.Reset();
    navnet.nme_owner = this;
    psy_self = NULL;
    spd_throttle = 0.0f;
    tmr_common[0] = tmr_common[1] = tmr_common[2] = 0.0f;
}

void zNMECommon::Reset()
{
    zNPCCommon::Reset();

    if (psy_self != NULL)
    {
        psy_self->Amnesia(0);
    }

    navnet.Reset();
    navnet.nme_owner = this;
    runtimeData.flags = 0;
    spd_throttle = 0.0f;
    tmr_common[0] = tmr_common[1] = tmr_common[2] = 0.0f;
}

void zNMECommon::Setup()
{
    zNPCCommon::Setup();

    if (psy_self == NULL)
    {
        psy_self = xBehaveMgr_GetSelf()->Subscribe(this, 0);
        psy_self->BrainBegin();
        psy_self->AddGoal(NME_GOAL_IDLE, NULL);
        psy_self->BrainEnd();
        psy_self->SetSafety(NME_GOAL_IDLE);
        psy_self->GoalSet(NME_GOAL_IDLE, 0);
    }

    SelfSetup();
}

void zNMECommon::SelfSetup()
{
}

void zNMECommon::SelfDestroy()
{
}

void zNMECommon::Process(xScene* xscn, F32 dt)
{
    xNPCBasic::Process(xscn, dt);

    if (psy_self != NULL)
    {
        psy_self->Timestep(dt, this);
    }

    if (cfg_common != NULL && cfg_common->tym_invuln > 0.0f)
    {
        cfg_common->tym_invuln = MAX(0.0f, cfg_common->tym_invuln - dt);
    }

    tmr_scary = MAX(0.0f, tmr_scary - dt);
    tmr_lastAlert = MAX(0.0f, tmr_lastAlert - dt);
}

void zNMECommon::BUpdate(xVec3* pos)
{
    xNPCBasic::BUpdate(pos);
}

void zNMECommon::NewTime(xScene* xscn, F32 dt)
{
    xNPCBasic::NewTime(xscn, dt);
}

void zNMECommon::Destroy()
{
    SelfDestroy();

    if (psy_self != NULL)
    {
        xBehaveMgr_GetSelf()->UnSubscribe(psy_self);
        psy_self = NULL;
    }

    if (cfg_common != NULL)
    {
        delete cfg_common;
        cfg_common = NULL;
    }

}

S32 zNMECommon::IsHealthy()
{
    return 1;
}

S32 zNMECommon::IsAlive()
{
    return 1;
}

U32 zNMECommon::AnimPick(S32, en_NPC_GOAL_SPOT, xGoal*)
{
    return 0;
}

void zNMECommon::RenderExtra()
{
}

zNMENavNet* zNMECommon::Gimme_NavNet()
{
    return &navnet;
}

F32 zNMECommon::ThrottleAdjust(F32 dt, F32 spd_want, F32 accel)
{
    F32 delta = spd_want - spd_throttle;
    F32 step = accel * dt;

    if (delta > step)
    {
        delta = step;
    }
    else if (delta < -step)
    {
        delta = -step;
    }

    spd_throttle += delta;
    return spd_throttle;
}

zNMEStandard::zNMEStandard(S32 myType) : zNMECommon(myType)
{
    tmr_stunned = 0.0f;
    pts_health = 1;
    pts_healthMax = 1;
}

void zNMEStandard::Init(xEntAsset* asset)
{
    zNMECommon::Init(asset);

    pts_healthMax = 1;
    pts_health = pts_healthMax;
    tmr_stunned = 0.0f;
}

void zNMEStandard::Reset()
{
    zNMECommon::Reset();
    pts_health = pts_healthMax;
    tmr_stunned = 0.0f;
}

void zNMEStandard::Setup()
{
    zNMECommon::Setup();
}

void zNMEStandard::Process(xScene* xscn, F32 dt)
{
    zNMECommon::Process(xscn, dt);

    if (tmr_stunned > 0.0f)
    {
        tmr_stunned = MAX(0.0f, tmr_stunned - dt);
    }
}

void zNMEStandard::SelfSetup()
{
}

S32 zNMEStandard::IsHealthy()
{
    return pts_health > 0;
}

S32 zNMEStandard::IsAlive()
{
    return pts_health > 0;
}

F32 zNMEStandard::HealthRatio() const
{
    if (pts_healthMax <= 0)
    {
        return 0.0f;
    }

    return (F32)pts_health / (F32)pts_healthMax;
}

U8 zNMEStandard::ColChkFlags() const
{
    return 0x18;
}

U8 zNMEStandard::ColPenFlags() const
{
    return 0x18;
}

U8 zNMEStandard::ColChkByFlags() const
{
    return 0x18;
}

U8 zNMEStandard::ColPenByFlags() const
{
    return 0x18;
}

U8 zNMEStandard::PhysicsFlags() const
{
    return 0x07;
}

xFactoryInst* ZNME_Create_Test(S32 who, RyzMemGrow* grow, void*)
{
    if (who == NPC_TYPE_NME_TEST)
    {
        return new (who, grow) zNMEStandard(who);
    }

    return NULL;
}

void ZNME_Destroy_Test(xFactoryInst* inst)
{
    delete (zNMEStandard*)inst;
}
