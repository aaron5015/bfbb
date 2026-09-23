#include "zBuddyInternal.h"

#include "zGlobals.h"
#include "zEntPlayer.h"

namespace zBuddyInternal
{
extern F32 buddy_width;
}

S32 zBuddy_IsAvailable()
{
    using namespace zBuddyInternal;
    return enabled && selected != BUDDY_NONE && state != BUDDY_STATE_DEAD;
}

S32 zBuddy_IsSleepyAlerting()
{
    using namespace zBuddyInternal;
    if (!zBuddy_IsAvailable())
    {
        return 0;
    }

    return state == BUDDY_STATE_STRIKE || (buddy_moving && !buddy_sneaking_sleepy);
}

const xVec3* zBuddy_GetPosition()
{
    using namespace zBuddyInternal;
    return zBuddy_IsAvailable() ? &position : NULL;
}

const xVec3* zBuddy_GetTargetPosition()
{
    using namespace zBuddyInternal;
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
    using namespace zBuddyInternal;
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
        *target_position = *zBuddy_GetPosition();
        return 1;
    }

    *target_position = *xEntGetPos(&globals.player.ent);
    return 0;
}
