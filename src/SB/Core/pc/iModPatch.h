#ifndef IMODPATCH_H
#define IMODPATCH_H

#include <types.h>

// PC-only: what GameCube mods change in the game's code, done in C++.
//
// A GameCube mod can ship a DOL patch next to its assets. The port cannot run
// PowerPC code, so each patch it supports is written out here. The patches read
// bytes the GameCube leaves free in .sbss, which the mod's scenes set through
// eEventGivePowerUp (iGCMem.h); iModPatchByte gives those bytes PC storage.
//
// BFBB Deluxe 1.1.1 reads two of them:
// - 0x803CB8F4: when it is 1, dampen_velocity zeroes the vertical velocity, so
//   the bubble spin and the air melee hold the player in the air.
// - 0x803CB8F5: when it is not 0, PlayerAbsControl no longer cuts the bubble
//   bowl's turn rate to a tenth.
// Retail never writes either, so both are off in an unmodded game.

// The PC's stand-in for the free GameCube byte at `address`, or NULL.
U8* iModPatchByte(U32 address);

bool iModPatchSpinHover();
bool iModPatchBowlFullTurn();

// Script assets (SCRP). Retail has a _zScript object but no packer handler for
// its asset, so no package can make one. Mods built with Industrial Park add
// them, with the "Script Asset Support" code that every GameCube mod loader
// ships: one write puts 'SCRP' in assetTypeHandlers, the other turns
// zScriptUpdate's `state == WAITING` into `!=`, so a script that has run out
// of events sends eEventExpired. Retail packages hold no SCRP assets, so
// neither change reaches an unmodded game.
#define IMODPATCH_ASSET_TYPES { 'SCRP' },
#define IMODPATCH_ASSET_TYPE_COUNT 1

// Whether a script that ran out of events is done: `waiting` is retail's
// test, that the script is in ZSCRIPT_STATE_WAITING.
inline bool iModPatchScriptDone(bool waiting)
{
    return !waiting;
}

#endif
