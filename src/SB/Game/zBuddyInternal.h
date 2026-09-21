#ifndef ZBUDDY_INTERNAL_H
#define ZBUDDY_INTERNAL_H

#include "zBuddy.h"

#include <rwcore.h>

namespace zBuddyInternal
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
    BUDDY_STATE_SKILL,
    BUDDY_STATE_SKILL_RECOVER,
    BUDDY_STATE_RECOVER,
    BUDDY_STATE_DEAD
};

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
    { 841, 0, 164, 120 },
    { 520, 240, 145, 159 },
};

static const buddy_frame skill_frames[] = {
    { 639, 0, 202, 114 },
    { 400, 238, 120, 133 },
    { 0, 103, 209, 118 },
    { 356, 0, 143, 112 },
    { 447, 114, 216, 124 },
    { 0, 0, 232, 103 },
};

extern S32 enabled;
extern buddy_type selected;
extern xVec3 position;
extern RwRaster* buddy_raster;
extern F32 buddy_width;
extern F32 buddy_height;
extern S32 frame_index;
extern bool follow_running;
extern buddy_state state;
extern bool buddy_sneaking_sleepy;
extern bool buddy_moving;
extern F32 death_alpha;
extern S32 health;
extern S32 max_health;
extern S32 skill_kills;
extern S32 skill_kill_cost;
extern F32 skill_sweep_timer;
}

#endif
