#pragma once

#include "zNMECommon.h"
#include "xLaserBolt.h"
#include "zNPCTypeRobot.h"

struct zNMEGoalMiniMervZap : zNMEGoalCommon
{
    zNMEGoalMiniMervZap(S32 goalID) : zNMEGoalCommon(goalID) {}
    S32 Process(en_trantype* trantype, F32 dt, void* ctxt, xScene* scene) override;
};

struct zNMEMiniMerv : zNMEStandard
{
    enum
    {
        GOAL_ZAP = 'MMZP'
    };

    F32 focus_radius;
    F32 danger_radius;
    F32 close_attack_radius;
    F32 warning_time;
    F32 cooldown_time;
    F32 zap_timer;
    F32 cooldown_timer;
    U8 warning_active;
    U8 zap_fired;
    U8 zap_visible;
    U8 pad[1];
    F32 zap_visual_timer;
    static const S32 MUZZLE_FLASH_COUNT = 3;
    F32 muzzle_flash_spawn_timer;
    F32 muzzle_flash_timer[MUZZLE_FLASH_COUNT];
    F32 muzzle_flash_angle[MUZZLE_FLASH_COUNT];
    F32 muzzle_flash_scale[MUZZLE_FLASH_COUNT];
    S32 muzzle_flash_frame[MUZZLE_FLASH_COUNT];
    RwRaster* muzzle_flash_raster;
    RwRaster* muzzle_flash_inverted_raster;
    xLaserBoltEmitter warning_beam;
    NPCLaser zap_beam;
    NPCLaser muzzle_debug_beam;

    zNMEMiniMerv(S32 myType);

    void Setup() override;
    void Reset() override;
    void Process(xScene* xscn, F32 dt) override;
    void RenderExtra() override;
    void SelfDestroy() override;

    void UpdateZap(F32 dt);
    void FireWarningBeam();
    void FireZap();
    void UpdateZapBeam();
    void UpdateMuzzleFlash(F32 dt);
    void RenderMuzzleFlash();
    void GetMuzzlePos(xVec3* pos) const;
    bool TargetInFocusRange() const;
    bool TargetInDangerRange() const;
    bool TargetInCloseAttackRange() const;
};

void zNME_Register_MiniMervGoal();

xFactoryInst* ZNME_Create_MiniMerv(S32 who, RyzMemGrow* grow, void* userdata);
void ZNME_Destroy_MiniMerv(xFactoryInst* inst);
