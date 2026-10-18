#ifndef MODS_MODEL_H
#define MODS_MODEL_H

#include "iModFolder.h"

#include <types.h>

#include <stddef.h>

// The mods page without a window: which mods there are, which one
// `[assets] mod` picks, and installing and removing them. What a mod folder
// and its mod.ini are is in iModFolder.h.
//
// Choosing a mod edits the setting through config_model.h, so it is saved,
// applied and discarded with every other setting. Installing and removing
// change the disk at once.

const S32 kModsModelMaxPath = 512;

struct ModsModelEntry
{
    // Where it is, and what `[assets] mod` says to pick it: "mods/<folder>"
    // for one in the library, the folder itself for one outside it.
    char folder[kModsModelMaxPath];
    char setting[kModsModelMaxPath];

    iModInfo info;
    bool inLibrary;
};

// Look again: every folder in the library, then the folder the setting names
// if it is somewhere else. Returns how many.
S32 ModsModelScan();
S32 ModsModelCount();
const ModsModelEntry* ModsModelAt(S32 i);

// The library, `<exe dir>/mods`, or "" if the host cannot say.
const char* ModsModelLibrary();

// The entry the setting names now, -1 for none, or -2 for a folder that is
// not there -- which the game ignores, so the original game is what runs.
S32 ModsModelActive();

// Pick entry `i`, or the original game for -1.
void ModsModelUse(S32 i);

// Pick a folder that is not in the library, where it is.
void ModsModelUseFolder(const char* folder);

// The folder name to suggest for something unpacked into `staged`: the asset
// root's own name when the archive had one, `archive`'s file name without its
// extension otherwise.
void ModsModelSuggestName(const char* staged, const char* archive, char* out, size_t outSize);

// Whether `name` can be a new folder in the library. False fills in `why`.
bool ModsModelNameOk(const char* name, char* why, size_t whySize);

// Make a new, empty folder in the library to unpack an archive into.
bool ModsModelStage(char* out, size_t outSize, char* why, size_t whySize);

// Move the asset root found in `staged` into the library as `name`, then
// delete what is left of `staged`. `*index` is the new entry.
bool ModsModelInstall(const char* staged, const char* name, S32* index, char* why, size_t whySize);

// Copy a folder that holds a mod into the library as `name`.
bool ModsModelCopyIn(const char* source, const char* name, S32* index, char* why, size_t whySize);

// Delete a library entry's folder and everything in it. One outside the
// library is never deleted. If it was the mod in use, the original game is
// picked.
bool ModsModelRemove(S32 i, char* why, size_t whySize);

// Delete a folder and everything in it.
bool ModsModelRemoveTree(const char* folder);

#endif
