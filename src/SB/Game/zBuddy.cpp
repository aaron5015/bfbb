#include "zBuddy.h"

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

#include <rwcore.h>
#include <string.h>

namespace
{
enum buddy_type
{
    BUDDY_NONE,
    BUDDY_CHERRY_COLA
};

enum buddy_state
{
    BUDDY_STATE_FOLLOW,
    BUDDY_STATE_CHASE,
    BUDDY_STATE_STRIKE,
    BUDDY_STATE_RECOVER,
    BUDDY_STATE_DEAD
};

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
const F32 buddy_sneak_speed = 0.35f;
const F32 buddy_sleepy_escape_margin = 1.0f;
S32 buddy_sleepy_count = 0;
bool buddy_sneaking_sleepy = false;

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


struct buddy_frame
{
    U16 x;
    U16 y;
    U16 width;
    U16 height;
};

static const buddy_frame idle_frames[] = {
    { 785, 120, 143, 129 },
    { 0, 221, 139, 129 },
    { 261, 238, 139, 132 },
};

static const buddy_frame run_frames[] = {
    { 209, 111, 118, 124 },
    { 327, 112, 120, 126 },
    { 663, 114, 122, 126 },
    { 139, 235, 122, 131 },
};

static const buddy_frame approach_frame = { 499, 0, 140, 114 };
static const buddy_frame death_frame = { 232, 0, 124, 111 };

static const buddy_frame attack_frames[] = {
    // skill0_01 and skill1_00 are the two strike frames.
    { 841, 0, 164, 120 },
    { 520, 240, 145, 159 },
};

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
    robot_hit_cooldown = 0.0f;
    damage_cooldown = 0.0f;
    buddy_sneaking_sleepy = false;
}
}

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
    max_health = MAX(1, xIniGetInt(ini, "Buddy.MaxHealth", 3));
    respawn_time = MAX(0.1f, xIniGetFloat(ini, "Buddy.RespawnTime", 5.0f));
}

void zBuddy_SceneInit()
{
    reset_position();
    if (globals.player.ent.frame != NULL)
    {
        position = globals.player.ent.frame->mat.pos;
        position.x -= 0.8f;
        position.z -= 0.8f;
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
    death_timer = respawn_time;
    death_alpha = 1.0f;
    death_velocity = 5.0f;
}

void zBuddy_ForgetTarget(zNPCCommon* target)
{
    if (attack_target == target)
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

S32 zBuddy_IsAvailable()
{
    return enabled && selected != BUDDY_NONE && state != BUDDY_STATE_DEAD;
}

S32 zBuddy_IsSleepyAlerting()
{
    if (!zBuddy_IsAvailable())
    {
        return 0;
    }

    // These are the Buddy equivalents of the player's alerting movement:
    // actively attacking something, or moving at normal/run speed instead
    // of sneaking around a sleeping Sleepy.
    return state == BUDDY_STATE_STRIKE || follow_running;
}

const xVec3* zBuddy_GetPosition()
{
    return zBuddy_IsAvailable() ? &position : NULL;
}

const xVec3* zBuddy_GetTargetPosition()
{
    static xVec3 target;
    if (!zBuddy_IsAvailable())
    {
        return NULL;
    }

    target = position;
    target.y += 0.5f * buddy_height;
    return &target;
}

S32 zBuddy_IsCloserTarget(const xVec3* source_position)
{
    if (!zBuddy_IsAvailable() || source_position == NULL || globals.player.Health < 1)
    {
        return 0;
    }

    F32 buddy_distance = xVec3Dist2(source_position, &position);
    F32 player_distance = xVec3Dist2(source_position, xEntGetPos(&globals.player.ent));
    return buddy_distance < player_distance;
}

S32 zBuddy_GetPreferredTarget(const xVec3* source_position, xVec3* target_position)
{
    if (source_position == NULL || target_position == NULL)
    {
        return 0;
    }

    if (zBuddy_IsCloserTarget(source_position))
    {
        *target_position = position;
        return 1;
    }

    *target_position = *xEntGetPos(&globals.player.ent);
    return 0;
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

    if (state == BUDDY_STATE_FOLLOW)
    {
        st_XORDEREDARRAY* npclist = zNPCMgr_GetNPCList();
        zNPCCommon* nearest = NULL;
        F32 nearest_distance = attack_radius * attack_radius;

        if (npclist != NULL)
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

        if (nearest != NULL)
        {
            state = BUDDY_STATE_CHASE;
            attack_target = nearest;
        }
    }

    if (state == BUDDY_STATE_CHASE)
    {
        if (attack_target == NULL || !attack_target->IsAlive() || !attack_target->IsHealthy())
        {
            state = BUDDY_STATE_FOLLOW;
            attack_target = NULL;
        }
        else
        {
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
            if (xVec3Length2(&delta) <= 1.0f)
            {
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
                bool target_is_sleepy = attack_target->SelfType() == NPC_TYPE_SLEEPY;
                bool low_health = health <= 2;
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
            if (attack_target != NULL && attack_target->IsAlive() && attack_target->IsHealthy())
            {
                if (attack_target->SelfType() == NPC_TYPE_SLEEPY)
                {
                    zNPCSleepy_BuddyAttack(attack_target);
                }
                attack_target->Damage(DMGTYP_SIDE, NULL, &position);
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

    xVec3 target = player;
    target.x -= 0.8f;
    target.z -= 0.8f;
    bool next_running = globals.player.Speed != 0;
    if (next_running != follow_running)
    {
        follow_running = next_running;
        frame_index = 0;
        frame_timer = 0.0f;
    }
    bool in_sleepy_range = buddy_has_sleepy_hazard(&position, &buddy_sleepy_count);
    buddy_sneaking_sleepy = in_sleepy_range;
    F32 follow_speed = in_sleepy_range ? buddy_sneak_speed : 1.0f;
    F32 follow = 1.0f - expf(-8.0f * follow_speed * dt);
    position.x += (target.x - position.x) * follow;
    position.y += (target.y - position.y) * follow;
    position.z += (target.z - position.z) * follow;
    frame_timer += dt;
    F32 frame_duration = in_sleepy_range ? 0.14f : follow_running ? 0.10f : 0.18f;
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
            : state == BUDDY_STATE_STRIKE || state == BUDDY_STATE_RECOVER
            ? &attack_frames[frame_index]
            : state == BUDDY_STATE_CHASE
                ? buddy_sneaking_sleepy
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