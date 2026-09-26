#pragma once

#include "zNMECommon.h"
#include "xLaserBolt.h"

struct zNMEGoalMiniMervZap : zNMEGoalCommon
{
    zNMEGoalMiniMervZap(S32 goalID) : zNMEGoalCommon(goalID) {}
    S32 Process(en_trantype* trantype, F32 dt, void* ctxt) override;
};

struct zNMEMiniMerv : zNMEStandard
{
    enum
    {
        GOAL_ZAP = 'MMZP'
    };

    F32 detect_radius;
    F32 danger_radius;
    F32 warning_time;
    F32 cooldown_time;
    F32 zap_timer;
    F32 cooldown_timer;
    U8 warning_active;
    U8 zap_fired;
    U8 pad[2];
    xLaserBoltEmitter warning_beam;

    zNMEMiniMerv(S32 myType);

    void Setup() override;
    void Reset() override;
    void Process(xScene* xscn, F32 dt) override;
    void RenderExtra() override;
    void SelfDestroy() override;

    void UpdateZap(F32 dt);
    void FireWarningBeam();
    void FireZap();
    bool TargetInDetectRange() const;
    bool TargetInDangerRange() const;
};

void zNME_Register_MiniMervGoal();

xFactoryInst* ZNME_Create_MiniMerv(S32 who, RyzMemGrow* grow, void* userdata);
void ZNME_Destroy_MiniMerv(xFactoryInst* inst);
