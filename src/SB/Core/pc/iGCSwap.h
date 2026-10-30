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
// platform and are not touched here; neither are the samples in SND and SNDS,
// which are byte streams.

// The second word of a GameCube SNDI after the swap. On the Xbox that word is
// num_streams. iSnd.cpp tells the two table layouts apart by it.
#define IGCSWAP_SNDI_GAMECUBE 0x47435349u // "GCSI"

enum iGCSwapResult
{
    IGCSWAP_DONE, // swapped
    IGCSWAP_UNKNOWN, // a type with no rule; left as it was
    IGCSWAP_MALFORMED // shorter than its layout says; partly swapped
};

// Swaps one asset of packer type `type` ('SIMP' and so on) in place. onLink,
// when given, is called with each of the asset's xLinkAssets once it is swapped.
iGCSwapResult iGCSwapAsset(U32 type, void* data, U32 size, void (*onLink)(void* link) = NULL);

// A cutscene streams most of itself from its CSN asset rather than loading it
// through the packer, so iCutscene.cpp swaps the pieces as they arrive: each
// time chunk, and each JDELTAMODEL. The models are RenderWare streams and the
// sound is DSP ADPCM; neither is touched. False when the data is shorter than
// it says it is; what was swapped by then stays swapped.
bool iGCSwapCutsceneTime(void* chunk, U32 size);
bool iGCSwapCutsceneDeltaModel(void* data, U32 size);

// Whether iGCSwapAsset has a rule for `type`. Types without one are either
// the same on both platforms or not handled yet.
bool iGCSwapKnowsType(U32 type);

#endif
