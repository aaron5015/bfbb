#ifndef IGCSWAP_H
#define IGCSWAP_H

#include <types.h>

// PC-only: GameCube assets, byte-swapped into the layout the port reads.
//
// The GameCube and Xbox releases ship the same asset types with the same
// layouts; the GameCube's are big-endian. iGCSwapAsset turns one asset around in
// place, field by field, so that everything after the packer reads it exactly
// as it reads the Xbox copy.
//
// Most of it is generated: tools/gcswap/gen.py writes iGCSwapTable.inc from the
// struct layouts in dwarf/dwarf_data.cpp and a per-type list of those structs in
// tools/gcswap/spec.py. tools/gcswap/check.py runs this file over every asset on
// a GameCube disc and compares each result with the Xbox asset of the same id.
//
// RenderWare streams (models, textures, worlds) are little-endian on every
// platform and are not touched here; neither is audio.

enum iGCSwapResult
{
    IGCSWAP_DONE,      // swapped
    IGCSWAP_UNKNOWN,   // a type with no rule; left as it was
    IGCSWAP_MALFORMED  // shorter than its layout says; partly swapped
};

// Swaps one asset of packer type `type` ('SIMP' and so on) in place.
iGCSwapResult iGCSwapAsset(U32 type, void* data, U32 size);

// Whether iGCSwapAsset has a rule for `type`. Types without one are either
// the same on both platforms or not handled yet.
bool iGCSwapKnowsType(U32 type);

#endif
