#ifndef ZBUDDY_H
#define ZBUDDY_H

#include <types.h>

struct xIniFile;
struct zNPCCommon;
struct xVec3;

void zBuddy_ParseINI(xIniFile* ini);
void zBuddy_SceneInit();
void zBuddy_SceneReset();
void zBuddy_SceneExit();
void zBuddy_SceneUpdate(F32 dt);
void zBuddy_Render();
void zBuddy_Damage(S32 amount);
void zBuddy_PlayerDeath();
void zBuddy_ForgetTarget(zNPCCommon* target);
void zBuddy_HitByRobot(const xVec3* robot_position, F32 radius);
void zBuddy_HitBySphere(const xVec3* sphere_center, F32 radius);
S32 zBuddy_IsAvailable();
S32 zBuddy_IsSleepyAlerting();
const xVec3* zBuddy_GetPosition();
const xVec3* zBuddy_GetTargetPosition();
S32 zBuddy_IsCloserTarget(const xVec3* source_position);
S32 zBuddy_GetPreferredTarget(const xVec3* source_position, xVec3* target_position);

#endif