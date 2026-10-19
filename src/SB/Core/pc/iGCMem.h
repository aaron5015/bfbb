#ifndef IGCMEM_H
#define IGCMEM_H

#include <types.h>

// PC-only: GameCube mods that write to memory through eEventGivePowerUp.
//
// The event sets globals.player.g.PowerUp[index] = 1 without checking the
// index, so a link with a large or negative index writes a 1 to any byte of the
// GameCube's RAM. Mods use it to set flags the event system cannot reach.
// globals is laid out differently on the PC, so the same index lands somewhere
// else there.
//
// iGCMemLink rewrites such a link as it is read from a GameCube package: the
// GameCube address it writes becomes the index of the same byte of the same
// field in the PC's globals, or of the PC's stand-in for a byte a mod's DOL
// patch reads (iModPatch.h). A link whose address is neither (a pointer, the
// heap, code) is dropped. tools/gcmem/gen.py writes the table of fields.

// The PC byte that holds the GameCube byte at `address`, or NULL.
U8* iGCMemAddress(U32 address);

// Called with each link of a GameCube asset, after it is swapped.
void iGCMemLink(void* link);

#endif
