#ifndef SAVES_MODEL_H
#define SAVES_MODEL_H

#include "iSaveFile.h"

#include <types.h>

#include <stddef.h>

// The saves page without a window: the three games in the save folder the
// game will use, and importing Xbox saves into them. What a save file is, and
// why an Xbox one needs only its signature cut off, is in iSaveFile.h.
//
// The folder follows the settings as config_model.h holds them, unsaved edits
// included.

const S32 kSavesModelMaxPath = 512;

// The folder the game's first save target is in.
void SavesModelFolder(char* out, size_t outSize);

// What is in game `game` of that folder. False for an empty slot or a file
// that is not a save.
bool SavesModelRead(S32 game, iSaveFileLeader* out);

// An Xbox save, checked and ready to import.
struct SavesModelXbox
{
    U8* data;
    size_t size;

    // The game it was on the Xbox, from SaveMeta.xbx, or -1 when there was no
    // SaveMeta.xbx or it named something else.
    S32 game;

    iSaveFileLeader leader;
    char source[kSavesModelMaxPath];
};

// Check a GameData.xsv and keep a copy of its save. `meta` is the
// SaveMeta.xbx beside it and may be NULL. False fills in `why`.
bool SavesModelReadXbox(const U8* xsv, size_t xsvSize, const U8* meta, size_t metaSize,
                        const char* source, SavesModelXbox* out, char* why, size_t whySize);
void SavesModelFreeXbox(SavesModelXbox* save);

// Write `save` as game `game`. A save already there is kept beside it as
// SpongeBob0N.bak, and its thumbnail is deleted, since it shows the old game.
bool SavesModelImport(const SavesModelXbox* save, S32 game, char* why, size_t whySize);

#endif
