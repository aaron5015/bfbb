#ifndef ZNMECOMMON_H
#define ZNMECOMMON_H

#include "zNPCTypeCommon.h"
#include "xBehaveMgr.h"
#include "zMovePoint.h"

enum en_nmegoal
{
    NME_GOAL_UNKNOWN = 0,
    NME_GOAL_IDLE = 'NG00',
    NME_GOAL_NOMORE,
};

struct NMERuntime
{
    U32 flags;
    S32 overrideDetect;
    S32 overrideAttack;
};

struct zNMENavNet
{
    zMovePoint* nav_past;
    zMovePoint* nav_curr;
    zMovePoint* nav_dest;
    zMovePoint* nav_lead;
    xSpline3* spl_mvptspline;
    F32 len_mvptspline;
    F32 dst_curspline;
    void* nme_owner;

    zNMENavNet();
    void Reset();
};

struct NMECfgCommon
{
    F32 tym_invuln;
    F32 acc_grav;
    F32 spd_maxFall;
    F32 spd_move;
    F32 acc_move;
    F32 dst_deviant;
    F32 spd_turnrate;

    NMECfgCommon();
};

struct zNMEGoalCommon : xGoal
{
    U32 anid_played;
    S32 flg_npcgauto;
    S32 flg_npcgable;
    S32 flg_info;
    S32 flg_user;

    zNMEGoalCommon(S32 goalID);

    void Clear() override;
    S32 Enter(F32 dt, void* updCtxt) override;
    S32 Resume(F32 dt, void* updCtxt) override;
    S32 PreCalc(F32 dt, void* updCtxt) override;

    U32 DoAutoAnim(S32 gspot, S32 forceRestart);
    U32 DoExplicitAnim(U32 anid, S32 forceRestart);
};

struct zNMECommon : zNPCCommon
{
    xPsyche* psy_self;
    NMECfgCommon* cfg_common;
    S32 flg_nmeVuln;
    S32 flg_nmeMove;
    S32 flg_nmeMisc;
    S32 flg_nmeAble;
    F32 spd_throttle;
    NMERuntime runtimeData;
    zNMENavNet navnet;
    F32 tmr_common[3];
    F32 tmr_scary;
    F32 tmr_lastAlert;
    zNMECommon* npc_duplodude;

    zNMECommon(S32 myType);

    void Init(xEntAsset* asset) override;
    void Reset() override;
    void Setup() override;
    void Process(xScene* xscn, F32 dt) override;
    void BUpdate(xVec3* pos) override;
    void NewTime(xScene* xscn, F32 dt) override;
    void Destroy() override;
    void SelfSetup() override;
    void SelfDestroy() override;
    S32 IsHealthy() override;
    S32 IsAlive() override;
    U32 AnimPick(S32 animID, en_NPC_GOAL_SPOT gspot, xGoal* goal) override;
    void RenderExtra() override;

    zNMENavNet* Gimme_NavNet();
    F32 ThrottleAdjust(F32 dt, F32 spd_want, F32 accel);
};

struct zNMEStandard : zNMECommon
{
    F32 tmr_stunned;
    S32 pts_health;
    S32 pts_healthMax;

    zNMEStandard(S32 myType);

    void Init(xEntAsset* asset) override;
    void Reset() override;
    void Setup() override;
    void Process(xScene* xscn, F32 dt) override;
    void SelfSetup() override;
    S32 IsHealthy() override;
    S32 IsAlive() override;
    F32 HealthRatio() const;

    U8 ColChkFlags() const override;
    U8 ColPenFlags() const override;
    U8 ColChkByFlags() const override;
    U8 ColPenByFlags() const override;
    U8 PhysicsFlags() const override;
};

struct zNMEGoalIdle : zNMEGoalCommon
{
    zNMEGoalIdle(S32 goalID) : zNMEGoalCommon(goalID) {}
};

void zNME_RegisterGoals();
void zNME_UnregisterGoals();

xFactoryInst* ZNME_Create_Test(S32 who, RyzMemGrow* grow, void* userdata);
void ZNME_Destroy_Test(xFactoryInst* inst);

#endif
