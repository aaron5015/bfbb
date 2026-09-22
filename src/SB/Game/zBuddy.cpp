#include "zBuddy.h"
#include "zBuddyInternal.h"

#include "iCamera.h"
#include "xIni.h"
#include "xMath.h"
#include "xString.h"
#include "xstransvc.h"
#include "zEntPlayer.h"
#include "zGlobals.h"
#include "zNPCMgr.h"
#include "zNPCTypeCommon.h"
#include "zNPCTypeRobot.h"
#include "xCollide.h"
#include "xScene.h"

#include <rwcore.h>
#include <string.h>

extern U32 g_hash_dupoanim[5];

namespace zBuddyInternal
{
S32 enabled;
buddy_type selected = BUDDY_NONE;
xVec3 position;
char texture_name[64] = "buddy_cherry_cola";
RwRaster* buddy_raster;
F32 buddy_width = 0.65f;
F32 buddy_height = 1.0f;
F32 frame_timer;
S32 frame_index;
buddy_state state;
bool follow_running;
zNPCCommon* attack_target;
F32 attack_radius = 6.0f;
F32 attack_timer;
S32 attack_count;
xVec3 attack_position;
S32 health;
S32 max_health = 3;
F32 respawn_time = 5.0f;
F32 death_timer;
F32 death_alpha;
F32 death_velocity;
F32 robot_hit_cooldown;
F32 damage_cooldown;
S32 skill_kills;
zNPCCommon* skill_target;
zNPCCommon* pending_kill_target;
F32 pending_kill_timer;
xVec3 skill_start_position;
xVec3 skill_target_position;
bool skill_damage_applied;
S32 skill_kill_cost = 9;
F32 skill_sweep_timer;
F32 idle_follow_radius = 8.0f;
F32 combat_follow_radius = 16.0f;
F32 combat_return_radius = 20.0f;
F32 stuck_timeout = 8.0f;
F32 wander_radius = 3.0f;
F32 wander_pause_min = 0.8f;
F32 wander_pause_max = 2.5f;
F32 wander_idle_short_min = 3.0f;
F32 wander_idle_short_max = 5.0f;
F32 wander_idle_long_min = 10.0f;
F32 wander_idle_long_max = 15.0f;
F32 wander_idle_long_chance = 0.25f;
F32 wander_speed = 1.4f;
F32 wander_curve_radius = 0.8f;
F32 move_speed = 3.5f;
F32 run_speed = 6.0f;
F32 catch_up_speed = 9.0f;
F32 catch_up_release_radius = 4.0f;
F32 catch_up_transition_time = 0.75f;
F32 catch_up_momentum_decay_time = 0.18f;
F32 catch_up_timeout = 5.0f;
F32 catch_up_combat_grace = 5.0f;
F32 catch_up_point_radius = 0.9f;
F32 catch_up_acceleration = 28.0f;
F32 catch_up_deceleration = 5.0f;
F32 catch_up_min_speed = 1.0f;
F32 gravity = 24.0f;
F32 collision_radius = 0.35f;
F32 vertical_velocity;
F32 stuck_timer;
F32 wander_timer;
F32 wander_pause;
F32 wander_idle_timer;
F32 wander_progress;
F32 wander_path_length;
bool wander_active;
bool catch_up_active;
bool catch_up_approach;
F32 catch_up_blend;
F32 catch_up_momentum;
F32 catch_up_timer;
xVec3 catch_up_target;
xVec3 wander_target;
xVec3 wander_start;
xVec3 wander_control;
const F32 buddy_sneak_speed = 0.35f;
const F32 buddy_sleepy_escape_margin = 1.0f;
S32 buddy_sleepy_count = 0;
bool buddy_sneaking_sleepy = false;
bool buddy_moving = false;
xVec3 safe_ground_position;
bool safe_ground_valid;

static void buddy_cancel_wander()
{
    wander_timer = 0.0f;
    wander_pause = 0.0f;
    wander_idle_timer = 0.0f;
    wander_progress = 0.0f;
    wander_path_length = 0.0f;
    wander_active = false;
}

bool buddy_has_sleepy_hazard(const xVec3* test_position, S32* sleepy_count = NULL)
{
    S32 count = 0;
    st_XORDEREDARRAY* npclist = zNPCMgr_GetNPCList();

    if (test_position == NULL || npclist == NULL)
    {
        if (sleepy_count != NULL)
        {
            *sleepy_count = 0;
        }
        return false;
    }

    for (S32 i = 0; i < npclist->cnt; i++)
    {
        zNPCCommon* npc = (zNPCCommon*)npclist->list[i];
        if (npc == NULL || npc->SelfType() != NPC_TYPE_SLEEPY || !npc->frame ||
            !npc->IsAlive() || !npc->IsHealthy())
        {
            continue;
        }

        if (zNPCSleepy_IsAsleep(npc) && zNPCSleepy_IsInDetectionRange(npc, test_position))
        {
            count++;
        }
    }

    if (sleepy_count != NULL)
    {
        *sleepy_count = count;
    }

    return count > 0;
}

bool buddy_sleepy_destination_safe(const xVec3* destination)
{
    return !buddy_has_sleepy_hazard(destination);
}

bool buddy_escape_active_sleepy(F32 dt)
{
    if (health > 2)
    {
        return false;
    }

    st_XORDEREDARRAY* sleepy_list = zNPCMgr_GetNPCList();
    if (sleepy_list == NULL)
    {
        return false;
    }

    xVec3 away_sum = xVec3{ 0.0f, 0.0f, 0.0f };
    S32 threat_count = 0;

    for (S32 i = 0; i < sleepy_list->cnt; i++)
    {
        zNPCCommon* npc = (zNPCCommon*)sleepy_list->list[i];
        if (npc == NULL || npc->SelfType() != NPC_TYPE_SLEEPY || !npc->frame ||
            !npc->IsAlive() || !npc->IsHealthy() || zNPCSleepy_IsAsleep(npc) ||
            !zNPCSleepy_IsInDetectionRange(npc, zBuddy_GetTargetPosition()))
        {
            continue;
        }

        zNPCSleepy* sleepy = (zNPCSleepy*)npc;
        if (!sleepy->alert_buddy)
        {
            continue;
        }

        xVec3 away;
        xVec3Sub(&away, &position, npc->Pos());
        away.y = 0.0f;
        F32 len2 = xVec3Length2(&away);
        if (len2 > 0.001f)
        {
            F32 inv_len = 1.0f / sqrtf(len2);
            away_sum.x += away.x * inv_len;
            away_sum.z += away.z * inv_len;
            threat_count++;
        }
    }

    if (threat_count == 0)
    {
        return false;
    }

    state = BUDDY_STATE_FOLLOW;
    attack_target = NULL;
    attack_timer = 0.0f;
    attack_count = 0;
    frame_index = 0;
    frame_timer = 0.0f;
    buddy_sneaking_sleepy = false;

    F32 away_len2 = xVec3Length2(&away_sum);
    if (away_len2 > 0.001f)
    {
        F32 inv_len = 1.0f / sqrtf(away_len2);
        F32 escape_distance = 2.0f + buddy_sleepy_escape_margin;
        position.x += away_sum.x * inv_len * escape_distance *
                      (1.0f - expf(-8.0f * dt));
        position.z += away_sum.z * inv_len * escape_distance *
                      (1.0f - expf(-8.0f * dt));
    }

    return true;
}


void reset_position()
{
    position = xVec3{ 0.0f, 0.0f, 0.0f };
    frame_timer = 0.0f;
    frame_index = 0;
    state = BUDDY_STATE_FOLLOW;
    follow_running = false;
    attack_target = NULL;
    attack_timer = 0.0f;
    attack_count = 0;
    attack_position = xVec3{ 0.0f, 0.0f, 0.0f };
    health = max_health;
    death_timer = 0.0f;
    death_alpha = 1.0f;
    death_velocity = 0.0f;
    vertical_velocity = 0.0f;
    robot_hit_cooldown = 0.0f;
    damage_cooldown = 0.0f;
    skill_kills = 0;
    skill_target = NULL;
    pending_kill_target = NULL;
    pending_kill_timer = 0.0f;
    skill_start_position = xVec3{ 0.0f, 0.0f, 0.0f };
    skill_target_position = xVec3{ 0.0f, 0.0f, 0.0f };
    skill_damage_applied = false;
    skill_sweep_timer = 0.0f;
    buddy_sneaking_sleepy = false;
    buddy_moving = false;
    safe_ground_position = xVec3{ 0.0f, 0.0f, 0.0f };
    safe_ground_valid = false;
    stuck_timer = 0.0f;
    wander_timer = 0.0f;
    wander_pause = 0.0f;
    wander_idle_timer = 0.0f;
    wander_progress = 0.0f;
    wander_path_length = 0.0f;
    wander_active = false;
    catch_up_active = false;
    catch_up_blend = 0.0f;
    catch_up_momentum = 0.0f;
    catch_up_timer = 0.0f;
    catch_up_approach = false;
    catch_up_target = xVec3{ 0.0f, 0.0f, 0.0f };
    wander_target = xVec3{ 0.0f, 0.0f, 0.0f };
    wander_start = xVec3{ 0.0f, 0.0f, 0.0f };
    wander_control = xVec3{ 0.0f, 0.0f, 0.0f };
}
}

using namespace zBuddyInternal;

void zBuddy_ParseINI(xIniFile* ini)
{
    enabled = xIniGetInt(ini, "Buddy.Enabled", 0) != 0;
    selected = BUDDY_NONE;

    char* name = xIniGetString(ini, "Buddy.Selected", "NULL");
    if (enabled && xStricmp(name, "CherryCola") == 0)
    {
        selected = BUDDY_CHERRY_COLA;
    }

    strncpy(texture_name, xIniGetString(ini, "Buddy.Texture", "buddy_cherry_cola"),
            sizeof(texture_name) - 1);
    texture_name[sizeof(texture_name) - 1] = '\0';
    buddy_width = xIniGetFloat(ini, "Buddy.Width", 0.65f);
    buddy_height = xIniGetFloat(ini, "Buddy.Height", 1.0f);
    attack_radius = xIniGetFloat(ini, "Buddy.AttackRadius", 6.0f);
    idle_follow_radius = MAX(0.1f, xIniGetFloat(ini, "Buddy.IdleFollowRadius", 8.0f));
    combat_follow_radius = MAX(idle_follow_radius,
                               xIniGetFloat(ini, "Buddy.CombatFollowRadius", 16.0f));
    combat_return_radius = MAX(combat_follow_radius,
                               xIniGetFloat(ini, "Buddy.CombatReturnRadius", 20.0f));
    stuck_timeout = MAX(0.1f, xIniGetFloat(ini, "Buddy.StuckTimeout", 8.0f));
    wander_radius = MAX(0.0f, xIniGetFloat(ini, "Buddy.WanderRadius", 3.0f));
    wander_pause_min = MAX(0.0f, xIniGetFloat(ini, "Buddy.WanderPauseMin", 0.8f));
    wander_pause_max = MAX(wander_pause_min,
                           xIniGetFloat(ini, "Buddy.WanderPauseMax", 2.5f));
    wander_idle_short_min = MAX(0.0f, xIniGetFloat(ini, "Buddy.WanderIdleShortMin", 3.0f));
    wander_idle_short_max = MAX(wander_idle_short_min,
                                xIniGetFloat(ini, "Buddy.WanderIdleShortMax", 5.0f));
    wander_idle_long_min = MAX(wander_idle_short_max,
                               xIniGetFloat(ini, "Buddy.WanderIdleLongMin", 10.0f));
    wander_idle_long_max = MAX(wander_idle_long_min,
                               xIniGetFloat(ini, "Buddy.WanderIdleLongMax", 15.0f));
    wander_idle_long_chance = CLAMP(xIniGetFloat(ini, "Buddy.WanderIdleLongChance", 0.25f),
                                    0.0f, 1.0f);
    wander_speed = MAX(0.1f, xIniGetFloat(ini, "Buddy.WanderSpeed", 1.4f));
    wander_curve_radius = MAX(0.0f, xIniGetFloat(ini, "Buddy.WanderCurveRadius", 0.8f));
    move_speed = MAX(0.1f, xIniGetFloat(ini, "Buddy.MoveSpeed", 3.5f));
    run_speed = MAX(move_speed, xIniGetFloat(ini, "Buddy.RunSpeed", 6.0f));
    catch_up_speed = MAX(run_speed, xIniGetFloat(ini, "Buddy.CatchUpSpeed", 9.0f));
    catch_up_release_radius = MAX(0.1f, xIniGetFloat(ini, "Buddy.CatchUpReleaseRadius", 4.0f));
    catch_up_transition_time = MAX(0.1f,
                                   xIniGetFloat(ini, "Buddy.CatchUpTransitionTime", 0.75f));
    catch_up_momentum_decay_time = MAX(0.05f,
                                       xIniGetFloat(ini, "Buddy.CatchUpMomentumDecayTime", 0.18f));
    catch_up_timeout = MAX(0.1f, xIniGetFloat(ini, "Buddy.CatchUpTimeout", 5.0f));
    catch_up_combat_grace = MAX(0.1f,
                                xIniGetFloat(ini, "Buddy.CatchUpCombatGrace", 5.0f));
    catch_up_point_radius = MAX(0.1f,
                                xIniGetFloat(ini, "Buddy.CatchUpPointRadius", 0.9f));
    catch_up_acceleration = MAX(0.1f,
                               xIniGetFloat(ini, "Buddy.CatchUpAcceleration", 28.0f));
    catch_up_deceleration = MAX(0.1f,
                                xIniGetFloat(ini, "Buddy.CatchUpDeceleration", 5.0f));
    catch_up_min_speed = MAX(0.0f,
                             xIniGetFloat(ini, "Buddy.CatchUpMinSpeed", 1.0f));
    gravity = MAX(0.0f, xIniGetFloat(ini, "Buddy.Gravity", 24.0f));
    collision_radius = MAX(0.05f, xIniGetFloat(ini, "Buddy.CollisionRadius", 0.35f));
    max_health = MAX(1, xIniGetInt(ini, "Buddy.MaxHealth", 3));
    respawn_time = MAX(0.1f, xIniGetFloat(ini, "Buddy.RespawnTime", 5.0f));
    skill_kill_cost = MAX(1, xIniGetInt(ini, "Buddy.SkillKillCost", 9));
}

void zBuddy_SceneInit()
{
    reset_position();
    if (globals.player.ent.frame != NULL)
    {
        position = globals.player.ent.frame->mat.pos;
        position.x -= 0.8f;
        position.z -= 0.8f;
        safe_ground_position = position;
        safe_ground_valid = true;
    }
    buddy_raster = NULL;

    if (enabled && selected != BUDDY_NONE && texture_name[0] != '\0')
    {
        RwTexture* texture = (RwTexture*)xSTFindAsset(xStrHash(texture_name), NULL);
        if (texture != NULL)
        {
            buddy_raster = texture->raster;
        }
    }
}

void zBuddy_SceneReset()
{
    reset_position();
    if (globals.player.ent.frame != NULL)
    {
        position = globals.player.ent.frame->mat.pos;
        position.x -= 0.8f;
        position.z -= 0.8f;
        safe_ground_position = position;
        safe_ground_valid = true;
    }
}

void zBuddy_SceneExit()
{
    reset_position();
    buddy_raster = NULL;
}

void zBuddy_Damage(S32 amount)
{
    if (!enabled || selected == BUDDY_NONE || state == BUDDY_STATE_DEAD || amount <= 0 ||
        state == BUDDY_STATE_SKILL || state == BUDDY_STATE_SKILL_RECOVER ||
        damage_cooldown > 0.0f)
    {
        return;
    }

    health = MAX(0, health - amount);
    damage_cooldown = 0.5f;
    if (health == 0)
    {
        state = BUDDY_STATE_DEAD;
        attack_target = NULL;
        skill_target = NULL;
        pending_kill_target = NULL;
        pending_kill_timer = 0.0f;
        skill_kills = 0;
        death_timer = respawn_time;
        death_alpha = 1.0f;
        death_velocity = 5.0f;
    }
}

void zBuddy_PlayerDeath()
{
    if (!enabled || selected == BUDDY_NONE || state == BUDDY_STATE_DEAD)
    {
        return;
    }

    health = 0;
    state = BUDDY_STATE_DEAD;
    attack_target = NULL;
    skill_target = NULL;
    pending_kill_target = NULL;
    pending_kill_timer = 0.0f;
    skill_kills = 0;
    death_timer = respawn_time;
    death_alpha = 1.0f;
    death_velocity = 5.0f;
}

static S32 buddy_target_is_valid(zNPCCommon* target)
{
    if (target == NULL || !target->frame || !target->IsAlive())
    {
        return 0;
    }

    if (target->SelfType() == NPC_TYPE_DUPLOTRON &&
        target->AnimCurStateID() == g_hash_dupoanim[4])
    {
        return 0;
    }

    if (target->SelfType() == NPC_TYPE_ARFDOG)
    {
    }

    if (!target->IsHealthy())
    {
        return 0;
    }

    return 1;
}

static bool buddy_begin_skill(zNPCCommon* target)
{
    if (skill_kills < skill_kill_cost || !buddy_target_is_valid(target))
    {
        return false;
    }

    state = BUDDY_STATE_SKILL;
    buddy_cancel_wander();
    attack_target = NULL;
    skill_target = target;
    skill_start_position = position;
    skill_target_position = *xEntGetCenter(target);
    skill_target_position.y = target->frame->mat.pos.y;
    frame_index = 0;
    attack_timer = 0.0f;
    skill_damage_applied = false;
    skill_kills -= skill_kill_cost;
    return true;
}

void zBuddy_ForgetTarget(zNPCCommon* target)
{
    if (skill_target == target)
    {
        skill_target = NULL;
    }

    if (attack_target == NULL)
    {
        return;
    }

    if (attack_target == target ||
        (attack_target->SelfType() == NPC_TYPE_ARFDOG && target != NULL &&
         target->SelfType() == NPC_TYPE_ARFDOG))
    {
        attack_target = NULL;
        if (state == BUDDY_STATE_CHASE || state == BUDDY_STATE_STRIKE)
        {
            state = BUDDY_STATE_FOLLOW;
            attack_timer = 0.0f;
            attack_count = 0;
            frame_index = 0;
            frame_timer = 0.0f;
        }
    }
}

void zBuddy_HitBySphere(const xVec3* sphere_center, F32 radius)
{
    if (!enabled || selected == BUDDY_NONE || state == BUDDY_STATE_DEAD ||
        sphere_center == NULL || robot_hit_cooldown > 0.0f)
    {
        return;
    }

    F32 buddy_radius = 0.5f * buddy_width;
    F32 min_y = position.y;
    F32 max_y = position.y + buddy_height;
    F32 closest_y = MAX(min_y, MIN(max_y, sphere_center->y));

    F32 dx = sphere_center->x - position.x;
    F32 dy = sphere_center->y - closest_y;
    F32 dz = sphere_center->z - position.z;
    F32 hit_radius = buddy_radius + MAX(0.0f, radius);

    if (dx * dx + dy * dy + dz * dz <= hit_radius * hit_radius)
    {
        zBuddy_Damage(1);
        robot_hit_cooldown = 0.5f;
    }
}

void zBuddy_HitByRobot(const xVec3* robot_position, F32 radius)
{
    zBuddy_HitBySphere(robot_position, radius);
}

void zBuddy_HitByGlove(const xVec3* sphere_center, F32 radius)
{
    if (!enabled || selected == BUDDY_NONE || state == BUDDY_STATE_DEAD ||
        sphere_center == NULL || robot_hit_cooldown > 0.0f)
    {
        return;
    }

    F32 buddy_radius = 0.5f * buddy_width;
    F32 min_y = position.y;
    F32 max_y = position.y + buddy_height;
    F32 closest_y = MAX(min_y, MIN(max_y, sphere_center->y));

    F32 dx = sphere_center->x - position.x;
    F32 dy = sphere_center->y - closest_y;
    F32 dz = sphere_center->z - position.z;
    F32 hit_radius = buddy_radius + MAX(0.0f, radius) + 0.5f * buddy_width;

    if (dx * dx + dy * dy + dz * dz <= hit_radius * hit_radius)
    {
        zBuddy_Damage(1);
        robot_hit_cooldown = 0.5f;
    }
}

void zBuddy_SceneUpdate(F32 dt)
{
    if (!enabled || selected == BUDDY_NONE || !globals.player.ent.frame || dt <= 0.0f)
    {
        return;
    }

    const xVec3& player = globals.player.ent.frame->mat.pos;
    robot_hit_cooldown = MAX(0.0f, robot_hit_cooldown - dt);
    damage_cooldown = MAX(0.0f, damage_cooldown - dt);
    skill_sweep_timer += dt;

    if (pending_kill_target != NULL)
    {
        if (!pending_kill_target->IsAlive())
        {
            skill_kills = MIN(skill_kill_cost, skill_kills + 1);
            pending_kill_target = NULL;
            pending_kill_timer = 0.0f;
        }
        else if ((pending_kill_timer -= dt) <= 0.0f)
        {
            pending_kill_target = NULL;
        }
    }

    buddy_moving = false;

    if (state == BUDDY_STATE_DEAD)
    {
        death_timer -= dt;
        death_velocity -= 12.0f * dt;
        position.y += death_velocity * dt;
        death_alpha = MIN(1.0f, death_timer / respawn_time);
        if (death_timer <= 0.0f)
        {
            health = max_health;
            death_alpha = 1.0f;
            state = BUDDY_STATE_FOLLOW;
            position = player;
            position.x -= 0.8f;
            position.z -= 0.8f;
            frame_index = 0;
            frame_timer = 0.0f;
        }
        return;
    }

    if (buddy_escape_active_sleepy(dt))
    {
        return;
    }

    if (state == BUDDY_STATE_RECOVER)
    {
        attack_timer -= dt;
        if (attack_timer <= 0.0f)
        {
            state = BUDDY_STATE_FOLLOW;
            frame_index = 0;
            frame_timer = 0.0f;
        }
        return;
    }

    if (state == BUDDY_STATE_SKILL_RECOVER)
    {
        attack_timer -= dt;
        frame_timer += dt;
        if (frame_timer >= 0.18f)
        {
            frame_timer -= 0.18f;
            frame_index = (frame_index + 1) %
                          (S32)(sizeof(idle_frames) / sizeof(idle_frames[0]));
        }
        if (attack_timer <= 0.0f)
        {
            state = BUDDY_STATE_FOLLOW;
            frame_index = 0;
            frame_timer = 0.0f;
        }
        return;
    }

    if (state == BUDDY_STATE_FOLLOW)
    {
        st_XORDEREDARRAY* npclist = zNPCMgr_GetNPCList();
        zNPCCommon* nearest = NULL;
        F32 nearest_distance = attack_radius * attack_radius;

        if (npclist != NULL)
        {
            /*
             * Arf's kennel dogs are reusable NPC objects. Prefer a live,
             * healthy ARFDOG in range so a respawned kennel dog is reacquired
             * regardless of the level-specific asset/name used for it.
             */
            zNPCCommon* nearest_arf_dog = NULL;
            F32 nearest_arf_distance = nearest_distance;

            for (S32 i = 0; i < npclist->cnt; i++)
            {
                zNPCCommon* npc = (zNPCCommon*)npclist->list[i];
                if (!npc || !npc->frame || !npc->IsAlive() || npc->SelfType() != NPC_TYPE_ARFDOG)
                {
                    continue;
                }


                xVec3 delta = *xEntGetCenter(npc);
                delta.x -= position.x;
                delta.y = 0.0f;
                delta.z -= position.z;
                F32 distance = xVec3Length2(&delta);
                if (distance < nearest_arf_distance)
                {
                    nearest_arf_dog = npc;
                    nearest_arf_distance = distance;
                }
            }

            if (nearest_arf_dog != NULL)
            {
                nearest = nearest_arf_dog;
                nearest_distance = nearest_arf_distance;
            }
            else
            {
                for (S32 i = 0; i < npclist->cnt; i++)
                {
                    zNPCCommon* npc = (zNPCCommon*)npclist->list[i];
                    if (!npc || !npc->frame || !npc->IsAlive() || !npc->IsHealthy())
                    {
                        continue;
                    }

                    xVec3 delta = *xEntGetCenter(npc);
                    delta.x -= position.x;
                    delta.y = 0.0f;
                    delta.z -= position.z;
                    F32 distance = xVec3Length2(&delta);
                    if (distance < nearest_distance)
                    {
                        nearest = npc;
                        nearest_distance = distance;
                    }
                }
            }
        }

        if (nearest != NULL)
        {
            if (!buddy_begin_skill(nearest))
            {
                buddy_cancel_wander();
                state = BUDDY_STATE_CHASE;
                attack_target = nearest;
            }
        }
    }

    if (state == BUDDY_STATE_CHASE)
    {
        catch_up_active = false;
        catch_up_approach = false;
        catch_up_timer = 0.0f;
        catch_up_target = xVec3{ 0.0f, 0.0f, 0.0f };
        if (!buddy_target_is_valid(attack_target))
        {
            state = BUDDY_STATE_FOLLOW;
            attack_target = NULL;
        }
        else
        {
            if (buddy_begin_skill(attack_target))
            {
                return;
            }

            xVec3 target = *xEntGetCenter(attack_target);
            xVec3 from_target;
            xVec3Sub(&from_target, &player, &target);
            from_target.y = 0.0f;
            F32 from_target_length = xVec3Length(&from_target);
            if (from_target_length > 0.001f)
            {
                xVec3SMulBy(&from_target, 0.6f / from_target_length);
                xVec3AddTo(&target, &from_target);
            }
            target.y = attack_target->frame->mat.pos.y;

            xVec3 delta;
            xVec3Sub(&delta, &position, &target);
            delta.y = 0.0f;
            xVec3 old_position = position;

            bool target_is_sleepy = attack_target->SelfType() == NPC_TYPE_SLEEPY;
            bool low_health = health <= 2;

            /*
             * Health does not prevent a stealth attack. A sleeping Sleepy
             * can be approached and attacked at any health value. If that
             * attack wakes it while Buddy is low-health, the top-level
             * active-Sleepy escape check takes over on the next update.
             */
            if (xVec3Length2(&delta) <= 1.0f)
            {
                buddy_cancel_wander();
                state = BUDDY_STATE_STRIKE;
                attack_position = position;
                attack_position.y = target.y;
                attack_timer = 0.0f;
                attack_count = 0;
                frame_index = 0;
            }
            else
            {
                S32 sleepy_count = 0;
                bool in_sleepy_range = buddy_has_sleepy_hazard(&position, &sleepy_count);
                bool target_in_sleepy_range = buddy_has_sleepy_hazard(&target);
                // target_is_sleepy and low_health were evaluated above so the
                // low-health Sleepy case can bail out before STRIKE.
                bool active_sleepy_range = false;
                xVec3 active_sleepy_away = xVec3{ 0.0f, 0.0f, 0.0f };
                buddy_sneaking_sleepy = in_sleepy_range || target_in_sleepy_range;

                /*
                 * Once Sleepy is awake, use every active Sleepy in range as
                 * an escape source. This is deliberately accumulated rather
                 * than choosing one NPC, so two overlapping Sleepies cannot
                 * make the Buddy escape directly into the other one's range.
                 */
                st_XORDEREDARRAY* sleepy_list = zNPCMgr_GetNPCList();
                if (sleepy_list != NULL && low_health)
                {
                    for (S32 i = 0; i < sleepy_list->cnt; i++)
                    {
                        zNPCCommon* npc = (zNPCCommon*)sleepy_list->list[i];
                        if (npc == NULL || npc->SelfType() != NPC_TYPE_SLEEPY || !npc->frame ||
                            !npc->IsAlive() || !npc->IsHealthy() ||
                            zNPCSleepy_IsAsleep(npc) ||
                            !zNPCSleepy_IsInDetectionRange(npc, &position))
                        {
                            continue;
                        }

                        active_sleepy_range = true;
                        xVec3 away;
                        xVec3Sub(&away, &position, npc->Pos());
                        away.y = 0.0f;
                        F32 len2 = xVec3Length2(&away);
                        if (len2 > 0.001f)
                        {
                            F32 inv_len = 1.0f / sqrtf(len2);
                            active_sleepy_away.x += away.x * inv_len;
                            active_sleepy_away.z += away.z * inv_len;
                        }
                    }
                }

                /*
                 * A sleeping Sleepy is a movement hazard, not a target lock.
                 * We slow down while sneaking through its range, but still
                 * allow a direct attack on another enemy. If the target itself
                 * is Sleepy, approach it slowly; attacking it is what wakes it.
                 */
                if (in_sleepy_range && !target_is_sleepy && !low_health)
                {
                    F32 follow = 1.0f - expf(-8.0f * buddy_sneak_speed * dt);
                    position.x += (target.x - position.x) * follow;
                    position.y += (target.y - position.y) * follow;
                    position.z += (target.z - position.z) * follow;
                }
                else if (active_sleepy_range)
                {
                    buddy_sneaking_sleepy = false;
                    xVec3 escape = position;
                    F32 escape_len2 = xVec3Length2(&active_sleepy_away);

                    if (escape_len2 > 0.001f)
                    {
                        F32 inv_len = 1.0f / sqrtf(escape_len2);
                        escape.x += active_sleepy_away.x * inv_len *
                                     (2.0f + buddy_sleepy_escape_margin);
                        escape.z += active_sleepy_away.z * inv_len *
                                     (2.0f + buddy_sleepy_escape_margin);
                    }

                    if (buddy_sleepy_destination_safe(&escape))
                    {
                        F32 follow = 1.0f - expf(-8.0f * dt);
                        position.x += (escape.x - position.x) * follow;
                        position.y += (escape.y - position.y) * follow;
                        position.z += (escape.z - position.z) * follow;
                    }
                }
                else if (in_sleepy_range && low_health)
                {
                    /*
                     * If the Sleepy is still asleep, low-health stealth should
                     * not turn into an immediate sprint. Keep the Buddy moving
                     * cautiously unless an active Sleepy actually threatens it.
                     */
                    F32 follow = 1.0f - expf(-8.0f * buddy_sneak_speed * dt);
                    position.x += (target.x - position.x) * follow;
                    position.y += (target.y - position.y) * follow;
                    position.z += (target.z - position.z) * follow;
                }
                else if (target_in_sleepy_range || target_is_sleepy)
                {
                    F32 follow = 1.0f - expf(-8.0f * buddy_sneak_speed * dt);
                    position.x += (target.x - position.x) * follow;
                    position.y += (target.y - position.y) * follow;
                    position.z += (target.z - position.z) * follow;
                }
                else
                {
                    buddy_sneaking_sleepy = false;
                    F32 follow = 1.0f - expf(-8.0f * dt);
                    position.x += (target.x - position.x) * follow;
                    position.y += (target.y - position.y) * follow;
                    position.z += (target.z - position.z) * follow;
                }

                if (xVec3Dist2(&old_position, &position) >= 0.0001f)
                {
                    buddy_moving = true;
                }

                frame_timer += dt;
                if (frame_timer >= (in_sleepy_range ? 0.14f : 0.10f))
                {
                    frame_timer -= (in_sleepy_range ? 0.14f : 0.10f);
                    frame_index = (frame_index + 1) % (S32)(sizeof(run_frames) / sizeof(run_frames[0]));
                }
                return;
            }
        }
    }

    if (state == BUDDY_STATE_STRIKE)
    {
        position = attack_position;
        attack_timer -= dt;
        if (attack_timer <= 0.0f)
        {
            frame_index = xrand() & 1;
            if (buddy_target_is_valid(attack_target))
            {
                if (attack_target->SelfType() == NPC_TYPE_SLEEPY)
                {
                    zNPCSleepy_BuddyAttack(attack_target);
                }
                attack_target->Damage(DMGTYP_SIDE, NULL, &position);
                pending_kill_target = attack_target;
                pending_kill_timer = 1.0f;
            }
            attack_count++;
            attack_timer += 0.2f;
            if (attack_count >= 3)
            {
                state = BUDDY_STATE_RECOVER;
                attack_timer = 0.5f;
                attack_target = NULL;
            }
        }
        return;
    }

    if (state == BUDDY_STATE_SKILL)
    {
        attack_timer += dt;
        const F32 frame_one_time = 0.10f;
        const F32 frame_two_time = 0.25f;
        const F32 frame_three_time = 0.20f;
        const F32 frame_four_time = 0.10f;
        const F32 frame_five_time = 0.10f;
        const F32 frame_six_time = 0.15f;
        const F32 frame_two_start = frame_one_time;
        const F32 frame_three_start = frame_two_start + frame_two_time;
        const F32 frame_four_start = frame_three_start + frame_three_time;
        const F32 frame_five_start = frame_four_start + frame_four_time;
        const F32 frame_six_start = frame_five_start + frame_five_time;
        const F32 skill_total_time = frame_six_start + frame_six_time;

        if (attack_timer >= skill_total_time)
        {
            state = BUDDY_STATE_SKILL_RECOVER;
            attack_timer = 0.5f;
            frame_index = 0;
            frame_timer = 0.0f;
            skill_target = NULL;
            skill_damage_applied = false;
            return;
        }

        if (attack_timer < frame_two_start)
        {
            frame_index = 0;
        }
        else if (attack_timer < frame_three_start)
        {
            frame_index = 1;
        }
        else if (attack_timer < frame_four_start)
        {
            frame_index = 2;
        }
        else if (attack_timer < frame_five_start)
        {
            frame_index = 3;
        }
        else if (attack_timer < frame_six_start)
        {
            frame_index = 4;
        }
        else
        {
            frame_index = 5;
        }

        if (frame_index == 0)
        {
            position = skill_start_position;
        }
        else if (frame_index == 1)
        {
            F32 progress = (attack_timer - frame_two_start) / frame_two_time;
            progress = progress * progress * (3.0f - 2.0f * progress);
            position.x = skill_start_position.x +
                         (skill_target_position.x - skill_start_position.x) * progress;
            position.y = skill_start_position.y +
                         (skill_target_position.y + 3.0f - skill_start_position.y) * progress;
            position.z = skill_start_position.z +
                         (skill_target_position.z - skill_start_position.z) * progress;
        }
        else if (frame_index == 2)
        {
            position = skill_target_position;
            position.y += 3.0f;
        }
        else if (frame_index == 3)
        {
            F32 progress = (attack_timer - frame_four_start) / frame_four_time;
            progress = 1.0f - (1.0f - progress) * (1.0f - progress);
            position = skill_target_position;
            position.y += 3.0f * (1.0f - progress);
        }
        else
        {
            position = skill_target_position;
            if (!skill_damage_applied)
            {
                if (skill_target != NULL && buddy_target_is_valid(skill_target))
                {
                    if (skill_target->SelfType() == NPC_TYPE_SLICK &&
                        ((zNPCSlick*)skill_target)->IsShield())
                    {
                        skill_target->Damage(DMGTYP_SIDE, NULL, &position);
                    }
                    else
                    {
                        skill_target->Damage(DMGTYP_INSTAKILL, NULL, &position);
                    }
                }
                skill_damage_applied = true;
            }
        }
        return;
    }

    xVec3 target = player;
    bool in_combat = state == BUDDY_STATE_CHASE || state == BUDDY_STATE_STRIKE;
    F32 player_distance = xVec3Dist(&position, &player);
    F32 follow_radius = in_combat ? combat_follow_radius : idle_follow_radius;

    if (in_combat && player_distance > combat_return_radius)
    {
        buddy_cancel_wander();
        state = BUDDY_STATE_FOLLOW;
        attack_target = NULL;
        attack_timer = 0.0f;
        attack_count = 0;
        frame_index = 0;
        frame_timer = 0.0f;
        in_combat = false;
        follow_radius = idle_follow_radius;
    }

    if (!catch_up_active && player_distance > idle_follow_radius)
    {
        catch_up_active = true;
        catch_up_approach = true;
        catch_up_target = xVec3{ 0.0f, 0.0f, 0.0f };
        catch_up_timer += dt;
    }
    else if (catch_up_active)
    {
        catch_up_timer += dt;
    }
    else if (!catch_up_active)
    {
        catch_up_timer = 0.0f;
    }

    if (catch_up_approach && catch_up_target.x == 0.0f && catch_up_target.z == 0.0f)
    {
        xVec3 offset = position - player;
        offset.y = 0.0f;
        F32 offset_length = xVec3Length(&offset);
        if (offset_length > 0.001f)
        {
            xVec3SMulBy(&offset, catch_up_point_radius / offset_length);
        }
        else
        {
            offset = xVec3{ -catch_up_point_radius, 0.0f, 0.0f };
        }
        catch_up_target = player + offset;
    }

    F32 catch_up_limit = in_combat ? catch_up_combat_grace : catch_up_timeout;
    if (catch_up_active && catch_up_timer >= catch_up_limit)
    {
        buddy_cancel_wander();
        position = player;
        position.x -= 0.8f;
        position.z -= 0.8f;
        vertical_velocity = 0.0f;
        catch_up_active = false;
        catch_up_approach = false;
        catch_up_target = xVec3{ 0.0f, 0.0f, 0.0f };
        catch_up_timer = 0.0f;
        catch_up_blend = 0.0f;
        catch_up_momentum = 0.0f;
        state = BUDDY_STATE_FOLLOW;
        attack_target = NULL;
        attack_timer = 0.0f;
        attack_count = 0;
        follow_running = false;
        frame_index = 0;
        frame_timer = 0.0f;
        in_combat = false;
    }

    if (catch_up_active && catch_up_approach)
    {
        target = catch_up_target;
    }

    if (catch_up_active)
    {
        F32 catch_up_distance = xVec3Dist(&position, &catch_up_target);
        F32 target_speed = catch_up_speed;
        if (catch_up_approach)
        {
            F32 braking_speed = sqrtf(2.0f * catch_up_deceleration *
                                      MAX(0.0f, catch_up_distance));
            target_speed = MIN(catch_up_speed, MAX(catch_up_min_speed, braking_speed));
        }

        if (catch_up_momentum < target_speed)
        {
            catch_up_momentum = MIN(target_speed,
                                    catch_up_momentum + catch_up_acceleration * dt);
        }
        else
        {
            catch_up_momentum = MAX(target_speed,
                                    catch_up_momentum - catch_up_deceleration * dt);
        }
    }
    else
    {
        catch_up_momentum = MAX(0.0f, catch_up_momentum -
                                         catch_up_speed * dt / catch_up_momentum_decay_time);
    }

    F32 catch_up_blend_target = catch_up_active ? 1.0f : 0.0f;
    F32 catch_up_blend_step = dt / catch_up_transition_time;
    if (catch_up_blend < catch_up_blend_target)
    {
        catch_up_blend = MIN(catch_up_blend_target, catch_up_blend + catch_up_blend_step);
    }
    else
    {
        catch_up_blend = MAX(catch_up_blend_target, catch_up_blend - catch_up_blend_step);
    }

    bool must_catch_up = catch_up_active;
    bool in_sleepy_range = buddy_has_sleepy_hazard(&position, &buddy_sleepy_count);
    bool wander_finishing = false;
    if (!in_combat && !must_catch_up && player_distance <= follow_radius && wander_radius > 0.0f)
    {
        if (wander_idle_timer > 0.0f)
        {
            wander_idle_timer = MAX(0.0f, wander_idle_timer - dt);
            target = position;
        }
        else if (!wander_active)
        {
            F32 angle = xurand() * 6.28318530718f;
            F32 distance = wander_radius * (0.35f + 0.65f * xurand());
            F32 side = (xurand() * 2.0f) - 1.0f;
            wander_target = player;
            wander_target.x += cosf(angle) * distance;
            wander_target.z += sinf(angle) * distance;
            wander_start = position;
            wander_control = (wander_start + wander_target) * 0.5f;
            wander_control.x += -sinf(angle) * side * wander_curve_radius;
            wander_control.z += cosf(angle) * side * wander_curve_radius;
            wander_progress = 0.0f;
            wander_path_length = MAX(0.25f, xVec3Dist(&wander_start, &wander_target));
            wander_active = true;
        }

        if (wander_active)
        {
            F32 wander_step_speed = in_sleepy_range
                                        ? buddy_sneak_speed
                                        : MAX(wander_speed, catch_up_momentum);
            wander_progress = MIN(1.0f, wander_progress + wander_step_speed * dt /
                                                       wander_path_length);
            F32 eased = wander_progress * wander_progress *
                        (3.0f - 2.0f * wander_progress);
            F32 inverse = 1.0f - eased;
            target.x = inverse * inverse * wander_start.x +
                       2.0f * inverse * eased * wander_control.x + eased * eased * wander_target.x;
            target.y = position.y;
            target.z = inverse * inverse * wander_start.z +
                       2.0f * inverse * eased * wander_control.z + eased * eased * wander_target.z;

            if (wander_progress >= 1.0f)
            {
                wander_finishing = true;
                wander_active = false;
                wander_idle_timer = (xurand() < wander_idle_long_chance
                                         ? wander_idle_long_min +
                                               (wander_idle_long_max - wander_idle_long_min) * xurand()
                                         : wander_idle_short_min +
                                               (wander_idle_short_max - wander_idle_short_min) * xurand());
                    target = wander_target;
            }
        }
    }
    else
    {
        if (wander_active || wander_idle_timer > 0.0f)
        {
            buddy_cancel_wander();
        }
        target = player;
        if (!must_catch_up)
        {
            target.x -= 0.8f;
            target.z -= 0.8f;
        }
    }

    if (catch_up_active && catch_up_approach)
    {
        target = catch_up_target;
    }

    target.y = position.y;
    bool path_moving = wander_active && wander_progress < 1.0f;
    bool next_running = path_moving || xVec3Dist2(&position, &target) > 0.25f;
    if (next_running != follow_running)
    {
        follow_running = next_running;
        frame_index = 0;
        frame_timer = 0.0f;
    }
    buddy_sneaking_sleepy = in_sleepy_range;
    F32 normal_speed = in_sleepy_range
                           ? buddy_sneak_speed
                           : player_distance > follow_radius ? run_speed
                                                             : in_combat ? move_speed
                                                                         : ((wander_active || wander_finishing)
                                                                                ? wander_speed
                                                                                : 0.0f);
    F32 desired_speed = catch_up_active ? catch_up_momentum
                                       : normal_speed;
    F32 distance_to_target = xVec3Dist(&position, &target);
    F32 follow = distance_to_target > 0.001f
                     ? MIN(1.0f, desired_speed * dt / distance_to_target)
                     : 0.0f;
    xVec3 old_position = position;
    if (wander_active && !in_sleepy_range)
    {
        position.x = target.x;
        position.z = target.z;
    }
    else
    {
        position.x += (target.x - position.x) * follow;
        position.z += (target.z - position.z) * follow;
    }

    xVec3 player_delta = position - player;
    player_delta.y = 0.0f;
    F32 player_distance2 = xVec3Length2(&player_delta);
    F32 separation_radius = collision_radius + 0.5f;
    if (player_distance2 < separation_radius * separation_radius)
    {
        if (player_distance2 > 0.0001f)
        {
            F32 inv_distance = 1.0f / sqrtf(player_distance2);
            F32 separation = separation_radius - sqrtf(player_distance2);
            separation = MIN(separation, move_speed * dt);
            position.x += player_delta.x * inv_distance * separation;
            position.z += player_delta.z * inv_distance * separation;
        }
        else
        {
            position.x += MIN(separation_radius, move_speed * dt);
        }
    }

    xRay3 ground_ray;
    xCollis ground_coll;
    ground_ray.origin = position;
    ground_ray.origin.y += buddy_height + 0.5f;
    ground_ray.dir = xVec3{ 0.0f, -1.0f, 0.0f };
    ground_ray.min_t = 0.0f;
    ground_ray.max_t = 10.0f;
    ground_ray.flags = 0xc00;
    ground_coll.flags = 0;
    ground_coll.dist = 1e38f;
    if (globals.sceneCur != NULL)
    {
        xRayHitsScene(globals.sceneCur, &ground_ray, &ground_coll);
    }

    if ((ground_coll.flags & 1) && ground_coll.norm.y > 0.45f)
    {
        F32 ground_y = ground_ray.origin.y - ground_coll.dist;
        if (vertical_velocity <= 0.0f || position.y <= ground_y + 0.2f)
        {
            position.y = ground_y;
            vertical_velocity = 0.0f;
            safe_ground_position = position;
            safe_ground_valid = true;
        }
    }
    else
    {
        if (safe_ground_valid)
        {
            position.y = safe_ground_position.y;
            vertical_velocity = 0.0f;
        }
        else
        {
            vertical_velocity -= gravity * dt;
            position.y += vertical_velocity * dt;
        }
    }

    if (catch_up_active && catch_up_approach && (ground_coll.flags & 1) &&
        ground_coll.norm.y > 0.45f)
    {
        xVec3 point_delta = position - catch_up_target;
        point_delta.y = 0.0f;
        if (xVec3Length2(&point_delta) <= 0.35f * 0.35f)
        {
            catch_up_active = false;
            catch_up_approach = false;
            catch_up_timer = 0.0f;
            catch_up_blend = 0.0f;
            catch_up_momentum = 0.0f;
            catch_up_target = xVec3{ 0.0f, 0.0f, 0.0f };
            wander_idle_timer = wander_idle_short_min +
                                (wander_idle_short_max - wander_idle_short_min) * xurand();
            follow_running = false;
            buddy_moving = false;
            frame_index = 0;
            frame_timer = 0.0f;
        }
    }

    /*
     * Being inside Sleepy's detection range is not itself movement. If Buddy
     * is standing still, keep the idle animation rather than showing the
     * sneak/walk cycle indefinitely.
     */
    F32 moved2 = xVec3Dist2(&old_position, &position);
    buddy_moving = path_moving || moved2 >= 0.0001f;
    if (next_running && !buddy_moving)
    {
        stuck_timer += dt;
        if (stuck_timer >= stuck_timeout)
        {
            position = player;
            position.x -= 0.8f;
            position.z -= 0.8f;
            stuck_timer = 0.0f;
        }
    }
    else
    {
        stuck_timer = 0.0f;
    }
    if (in_sleepy_range && !buddy_moving)
    {
        follow_running = false;
        buddy_sneaking_sleepy = false;
        wander_pause = wander_pause_min + (wander_pause_max - wander_pause_min) * xurand();
    }

    frame_timer += dt;
    F32 frame_duration = follow_running ? 0.10f : 0.18f;
    if (frame_timer >= frame_duration)
    {
        frame_timer -= frame_duration;
        S32 frame_count = follow_running ? (S32)(sizeof(run_frames) / sizeof(run_frames[0]))
                                         : (S32)(sizeof(idle_frames) / sizeof(idle_frames[0]));
        frame_index = (frame_index + 1) % frame_count;
    }
}

void zBuddy_Render()
{
    if (!enabled || selected == BUDDY_NONE || !globals.camera.lo_cam)
    {
        return;
    }

    const buddy_frame* frame =
        state == BUDDY_STATE_DEAD
            ? &death_frame
            : state == BUDDY_STATE_SKILL
            ? &skill_frames[frame_index]
            : state == BUDDY_STATE_SKILL_RECOVER
            ? &idle_frames[frame_index]
            : state == BUDDY_STATE_STRIKE || state == BUDDY_STATE_RECOVER
            ? &attack_frames[frame_index]
            : state == BUDDY_STATE_CHASE
                ? (buddy_sneaking_sleepy && buddy_moving)
                    ? &idle_frames[frame_index % (S32)(sizeof(idle_frames) / sizeof(idle_frames[0]))]
                    : &approach_frame
                : buddy_sneaking_sleepy
                    ? &run_frames[frame_index % (S32)(sizeof(run_frames) / sizeof(run_frames[0]))]
                    : follow_running ? &run_frames[frame_index] : &idle_frames[frame_index];
    F32 frame_aspect = (F32)frame->width / (F32)frame->height;
    F32 half_width = buddy_width * frame_aspect * 0.5f;
    F32 half_height = buddy_height * 0.5f;
    F32 u0 = (F32)frame->x / 1024.0f;
    F32 v0 = (F32)frame->y / 512.0f;
    F32 u1 = (F32)(frame->x + frame->width) / 1024.0f;
    F32 v1 = (F32)(frame->y + frame->height) / 512.0f;

    RwMatrix* camera_matrix = RwFrameGetLTM(RwCameraGetFrame(globals.camera.lo_cam));
    RwIm3DVertex quad[4];
    F32 left_x = position.x - camera_matrix->right.x * half_width;
    F32 left_y = position.y - camera_matrix->right.y * half_width;
    F32 left_z = position.z - camera_matrix->right.z * half_width;
    F32 right_x = position.x + camera_matrix->right.x * half_width;
    F32 right_y = position.y + camera_matrix->right.y * half_width;
    F32 right_z = position.z + camera_matrix->right.z * half_width;
    F32 top_x = camera_matrix->up.x * buddy_height;
    F32 top_y = camera_matrix->up.y * buddy_height;
    F32 top_z = camera_matrix->up.z * buddy_height;
    U8 red = buddy_raster != NULL ? 255 : 255;
    U8 green = buddy_raster != NULL ? 255 : 80;
    U8 blue = buddy_raster != NULL ? 255 : 150;
    RwIm3DVertexSetPos(&quad[0], left_x, left_y, left_z);
    U8 alpha = (U8)(255.0f * death_alpha);
    RwIm3DVertexSetRGBA(&quad[0], red, green, blue, alpha);
    RwIm3DVertexSetUV(&quad[0], u0, v1);
    RwIm3DVertexSetPos(&quad[1], left_x + top_x, left_y + top_y, left_z + top_z);
    RwIm3DVertexSetRGBA(&quad[1], red, green, blue, alpha);
    RwIm3DVertexSetUV(&quad[1], u0, v0);
    RwIm3DVertexSetPos(&quad[2], right_x, right_y, right_z);
    RwIm3DVertexSetRGBA(&quad[2], red, green, blue, alpha);
    RwIm3DVertexSetUV(&quad[2], u1, v1);
    RwIm3DVertexSetPos(&quad[3], right_x + top_x, right_y + top_y, right_z + top_z);
    RwIm3DVertexSetRGBA(&quad[3], red, green, blue, alpha);
    RwIm3DVertexSetUV(&quad[3], u1, v0);

    RwRenderStateSet(rwRENDERSTATETEXTURERASTER, buddy_raster);
    RwRenderStateSet(rwRENDERSTATESRCBLEND, (void*)rwBLENDSRCALPHA);
    RwRenderStateSet(rwRENDERSTATEDESTBLEND, (void*)rwBLENDINVSRCALPHA);
    RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)TRUE);
    RwRenderStateSet(rwRENDERSTATEZTESTENABLE, (void*)TRUE);
    RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)FALSE);
    if (RwIm3DTransform(quad, 4, NULL, rwIM3D_VERTEXXYZ | rwIM3D_VERTEXUV |
                                         rwIM3D_VERTEXRGBA) != NULL)
    {
        RwIm3DRenderPrimitive(rwPRIMTYPETRISTRIP);
    }

    if (state != BUDDY_STATE_DEAD)
    {
        F32 skill_ratio = (F32)skill_kills / (F32)skill_kill_cost;
        F32 skill_bar_width = buddy_width * 0.9f;
        F32 skill_bar_height = 0.045f;
        F32 skill_bar_center_y = position.y + buddy_height + 0.20f;
        F32 skill_bar_left_x = position.x - camera_matrix->right.x * skill_bar_width * 0.5f;
        F32 skill_bar_left_y = skill_bar_center_y - camera_matrix->right.y * skill_bar_width * 0.5f;
        F32 skill_bar_left_z = position.z - camera_matrix->right.z * skill_bar_width * 0.5f;
        F32 skill_bar_right_x = position.x + camera_matrix->right.x * skill_bar_width * 0.5f;
        F32 skill_bar_right_y = skill_bar_center_y + camera_matrix->right.y * skill_bar_width * 0.5f;
        F32 skill_bar_right_z = position.z + camera_matrix->right.z * skill_bar_width * 0.5f;
        F32 skill_bar_top_x = camera_matrix->up.x * skill_bar_height * 0.5f;
        F32 skill_bar_top_y = camera_matrix->up.y * skill_bar_height * 0.5f;
        F32 skill_bar_top_z = camera_matrix->up.z * skill_bar_height * 0.5f;
        RwIm3DVertex skill_bar[4];

        RwRenderStateSet(rwRENDERSTATETEXTURERASTER, NULL);
        RwIm3DVertexSetPos(&skill_bar[0], skill_bar_left_x, skill_bar_left_y, skill_bar_left_z);
        RwIm3DVertexSetPos(&skill_bar[1], skill_bar_left_x + skill_bar_top_x,
                           skill_bar_left_y + skill_bar_top_y,
                           skill_bar_left_z + skill_bar_top_z);
        RwIm3DVertexSetPos(&skill_bar[2], skill_bar_right_x, skill_bar_right_y, skill_bar_right_z);
        RwIm3DVertexSetPos(&skill_bar[3], skill_bar_right_x + skill_bar_top_x,
                           skill_bar_right_y + skill_bar_top_y,
                           skill_bar_right_z + skill_bar_top_z);
        for (S32 i = 0; i < 4; i++)
        {
            RwIm3DVertexSetRGBA(&skill_bar[i], 35, 35, 40, 225);
        }
        if (RwIm3DTransform(skill_bar, 4, NULL, rwIM3D_VERTEXXYZ | rwIM3D_VERTEXRGBA) != NULL)
        {
            RwIm3DRenderPrimitive(rwPRIMTYPETRISTRIP);
        }

        F32 skill_fill_width = skill_bar_width * skill_ratio;
        skill_bar_right_x = skill_bar_left_x + camera_matrix->right.x * skill_fill_width;
        skill_bar_right_y = skill_bar_left_y + camera_matrix->right.y * skill_fill_width;
        skill_bar_right_z = skill_bar_left_z + camera_matrix->right.z * skill_fill_width;
        RwIm3DVertexSetPos(&skill_bar[2], skill_bar_right_x, skill_bar_right_y, skill_bar_right_z);
        RwIm3DVertexSetPos(&skill_bar[3], skill_bar_right_x + skill_bar_top_x,
                           skill_bar_right_y + skill_bar_top_y,
                           skill_bar_right_z + skill_bar_top_z);

        U8 skill_left_red = 255;
        U8 skill_left_green = 55;
        U8 skill_left_blue = 55;
        U8 skill_right_red = (U8)(255.0f * (1.0f - skill_ratio));
        U8 skill_right_green = (U8)(55.0f + 200.0f * skill_ratio);
        U8 skill_right_blue = (U8)(55.0f + 200.0f * skill_ratio);
        if (skill_kills >= skill_kill_cost)
        {
            F32 sweep = skill_sweep_timer * 3.0f;
            skill_left_red = (U8)(80.0f + 100.0f * (0.5f + 0.5f * sinf(sweep - 2.1f)));
            skill_left_green = (U8)(150.0f + 105.0f * (0.5f + 0.5f * sinf(sweep)));
            skill_left_blue = (U8)(150.0f + 105.0f * (0.5f + 0.5f * sinf(sweep + 2.1f)));
            skill_right_red = (U8)(80.0f + 100.0f * (0.5f + 0.5f * sinf(sweep + 1.0f)));
            skill_right_green = (U8)(150.0f + 105.0f * (0.5f + 0.5f * sinf(sweep + 3.1f)));
            skill_right_blue = (U8)(150.0f + 105.0f * (0.5f + 0.5f * sinf(sweep + 5.2f)));
        }
        RwIm3DVertexSetRGBA(&skill_bar[0], skill_left_red, skill_left_green, skill_left_blue, 255);
        RwIm3DVertexSetRGBA(&skill_bar[1], skill_left_red, skill_left_green, skill_left_blue, 255);
        RwIm3DVertexSetRGBA(&skill_bar[2], skill_right_red, skill_right_green, skill_right_blue,
                            255);
        RwIm3DVertexSetRGBA(&skill_bar[3], skill_right_red, skill_right_green, skill_right_blue,
                            255);
        if (RwIm3DTransform(skill_bar, 4, NULL, rwIM3D_VERTEXXYZ | rwIM3D_VERTEXRGBA) != NULL)
        {
            RwIm3DRenderPrimitive(rwPRIMTYPETRISTRIP);
        }

        F32 health_ratio = (F32)health / (F32)max_health;
        F32 bar_width = buddy_width * 0.9f;
        F32 bar_height = 0.06f;
        F32 bar_center_y = position.y + buddy_height + 0.12f;
        F32 bar_left_x = position.x - camera_matrix->right.x * bar_width * 0.5f;
        F32 bar_left_y = bar_center_y - camera_matrix->right.y * bar_width * 0.5f;
        F32 bar_left_z = position.z - camera_matrix->right.z * bar_width * 0.5f;
        F32 bar_right_x = position.x + camera_matrix->right.x * bar_width * 0.5f;
        F32 bar_right_y = bar_center_y + camera_matrix->right.y * bar_width * 0.5f;
        F32 bar_right_z = position.z + camera_matrix->right.z * bar_width * 0.5f;
        F32 bar_top_x = camera_matrix->up.x * bar_height * 0.5f;
        F32 bar_top_y = camera_matrix->up.y * bar_height * 0.5f;
        F32 bar_top_z = camera_matrix->up.z * bar_height * 0.5f;
        RwIm3DVertex bar[4];

        RwRenderStateSet(rwRENDERSTATETEXTURERASTER, NULL);
        RwIm3DVertexSetPos(&bar[0], bar_left_x, bar_left_y, bar_left_z);
        RwIm3DVertexSetPos(&bar[1], bar_left_x + bar_top_x, bar_left_y + bar_top_y,
                           bar_left_z + bar_top_z);
        RwIm3DVertexSetPos(&bar[2], bar_right_x, bar_right_y, bar_right_z);
        RwIm3DVertexSetPos(&bar[3], bar_right_x + bar_top_x, bar_right_y + bar_top_y,
                           bar_right_z + bar_top_z);
        for (S32 i = 0; i < 4; i++)
        {
            RwIm3DVertexSetRGBA(&bar[i], 20, 20, 20, 220);
        }
        if (RwIm3DTransform(bar, 4, NULL, rwIM3D_VERTEXXYZ | rwIM3D_VERTEXRGBA) != NULL)
        {
            RwIm3DRenderPrimitive(rwPRIMTYPETRISTRIP);
        }

        F32 fill_width = bar_width * health_ratio;
        bar_right_x = bar_left_x + camera_matrix->right.x * fill_width;
        bar_right_y = bar_left_y + camera_matrix->right.y * fill_width;
        bar_right_z = bar_left_z + camera_matrix->right.z * fill_width;
        RwIm3DVertexSetPos(&bar[2], bar_right_x, bar_right_y, bar_right_z);
        RwIm3DVertexSetPos(&bar[3], bar_right_x + bar_top_x, bar_right_y + bar_top_y,
                           bar_right_z + bar_top_z);
        for (S32 i = 0; i < 4; i++)
        {
            RwIm3DVertexSetRGBA(&bar[i], 60, 220, 80, 255);
        }
        if (RwIm3DTransform(bar, 4, NULL, rwIM3D_VERTEXXYZ | rwIM3D_VERTEXRGBA) != NULL)
        {
            RwIm3DRenderPrimitive(rwPRIMTYPETRISTRIP);
        }
    }
}