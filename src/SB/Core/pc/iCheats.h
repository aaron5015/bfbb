#ifndef ICHEATS_H
#define ICHEATS_H

#include <types.h>

// PC-only: the `[cheats]` section of config.ini. Each one is off by default.
//
// iCheatsUpdate runs once a frame from the game loop and writes the player's
// state, so an unlock or a full inventory holds through a save load, a new
// game and whatever the level takes away. The power-ups, shinies and spatulas
// are saved like any other progress, so turning the cheat off keeps them.
//
// all_warps sets zGlobalSettings::CheatAlwaysPortal, the developers' own
// switch. zUI's pause-menu taxi already honours it; retail's SB.INI loader
// clears it. It is not saved.
//
// unlimited_health clears zGlobalSettings::TakeDamage, another developers'
// switch: zEntPlayer_Damage still knocks the player back but leaves Health
// alone. Goo and falling to death zero Health directly and still kill.
void iCheatSetAllWarps(S32 on);
void iCheatSetBubbleBowl(S32 on);
void iCheatSetCruiseBubble(S32 on);
void iCheatSetUnlimitedShinies(S32 on);
void iCheatSetUnlimitedSpatulas(S32 on);
void iCheatSetUnlimitedHealth(S32 on);

// The cheats from the retail button codes whose callbacks in zGameExtras.cpp
// only play a sound. The effect is the bit cheatList sets in
// zGameExtras_CheatFlags, which the robot AI, the villager AI, the player and
// the Cruise Bubble read. These are those bits, Medics' stray 0x100 included.
// zGameExtras_Save keeps the low 0xFFFF, so expert mode is saved and the rest
// are not.
enum iCheatGameFlag
{
    ICHEAT_EXPERT = 0x800,
    ICHEAT_BIG_PLANK = 0x10000,
    ICHEAT_MEDICS = 0x20100,
    ICHEAT_SMALL_VILLAGERS = 0x40000,
    ICHEAT_SMALL_COSTARS = 0x80000,
    ICHEAT_RICH_VILLAGERS = 0x100000,
    ICHEAT_PANHANDLE = 0x200000,
    ICHEAT_DOG_TRICKS = 0x400000,
    ICHEAT_SHRAPBOB = 0x2000000,
    ICHEAT_NO_PANTS = 0x10000000,
    ICHEAT_CRUISE_CONTROL = 0x20000000
};

void iCheatSetGameFlag(iCheatGameFlag flag, S32 on);

void iCheatsUpdate();

#endif
