// The config-file cheats. What each one does is in iCheats.h.

#include "iCheats.h"

#include "zEntPlayer.h"
#include "zGameExtras.h"
#include "zGlobals.h"

namespace
{
    // The number of golden spatulas in the game.
    const U32 kSpatulaMax = 100;

    S32 sAllWarps = FALSE;
    S32 sBubbleBowl = FALSE;
    S32 sCruiseBubble = FALSE;
    S32 sUnlimitedShinies = FALSE;
    S32 sUnlimitedSpatulas = FALSE;
    S32 sUnlimitedHealth = FALSE;
    S32 sGameFlags = 0;
}

void iCheatSetGameFlag(iCheatGameFlag flag, S32 on)
{
    if (on)
    {
        sGameFlags |= flag;
    }
    else
    {
        sGameFlags &= ~flag;
    }
}

void iCheatSetAllWarps(S32 on)
{
    sAllWarps = on;
}

void iCheatSetBubbleBowl(S32 on)
{
    sBubbleBowl = on;
}

void iCheatSetCruiseBubble(S32 on)
{
    sCruiseBubble = on;
}

void iCheatSetUnlimitedShinies(S32 on)
{
    sUnlimitedShinies = on;
}

void iCheatSetUnlimitedSpatulas(S32 on)
{
    sUnlimitedSpatulas = on;
}

void iCheatSetUnlimitedHealth(S32 on)
{
    sUnlimitedHealth = on;
}

void iCheatsUpdate()
{
    if (sAllWarps)
    {
        globals.player.g.CheatAlwaysPortal = TRUE;
    }
    if (sBubbleBowl)
    {
        globals.player.g.PowerUp[0] = TRUE;
    }
    if (sCruiseBubble)
    {
        globals.player.g.PowerUp[1] = TRUE;
    }
    if (sUnlimitedShinies)
    {
        globals.player.Inv_Shiny = SHINY_MAX;
    }
    if (sUnlimitedSpatulas)
    {
        globals.player.Inv_Spatula = kSpatulaMax;
    }
    if (sUnlimitedHealth)
    {
        globals.player.g.TakeDamage = FALSE;
    }
    if (sGameFlags != 0)
    {
        GEC_CheatFlagAdd(sGameFlags);
    }
}
