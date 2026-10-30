#ifndef ISAVEFILE_H
#define ISAVEFILE_H

#include <types.h>

#include <stddef.h>

// PC-only: where save files live and what is in one, for code that is not the
// game. isavegame.cpp writes and reads them; bfbb_config lists them and imports
// Xbox saves into them, and links this file without linking the game.
//
// A save is `GDAT` followed by one chunk per save client, written by
// xsavegame.cpp with native-endian copies:
//
//   header  [tag 'GDAT'][version 1][totalsize][crc]
//   chunk   [tag][maxamt][realamt][maxamt bytes]
//
// The crc is CRC-32 (polynomial 0x04C11DB7, MSB first, unreflected, initial
// value 0xFFFFFFFF, no final xor) over bytes 16..totalsize. The first chunk is
// `LEDR`, the leader the save screen lists.
//
// An Xbox `GameData.xsv` is the same bytes as a PC save followed by a 20-byte
// HMAC-SHA1 signature. Both are little-endian and both builds load the Xbox
// assets, so the scene chunks agree and nothing past the signature changes.

// The folder the three game files of the first save target live in. Follows
// isavegame.cpp's policy: BFBB_SAVE_DIR, then `configured` (game.save_folder),
// then `<user data>/bfbb/saves`, or `<user data>/bfbb/mods/<modName>/saves`
// while a mod is in use. `modName` may be NULL or empty.
void iSaveFileRoot(const char* configured, const char* modName, char* out, size_t outSize);

// The last component of a folder path, like "BFBBMix" for "D:/mods/BFBBMix/".
// The name a mod's saves are kept under.
void iSaveFileLeaf(const char* path, char* out, size_t outSize);

// "SpongeBob00".."SpongeBob02": the game file for one of the three games.
const S32 kSaveFileGames = 3;
void iSaveFileName(S32 game, char* out, size_t outSize);

// The game index a file name stands for, or -1.
S32 iSaveFileGameOf(const char* name);

U32 iSaveFileCrc(const U8* data, size_t size);

// Whether `data` holds a save this build can load: the tag, version 1, a size
// that fits, and a crc that matches. `*total` is the save's own size, which is
// less than `size` for an Xbox file. False fills in `why`.
bool iSaveFileCheck(const U8* data, size_t size, size_t* total, char* why, size_t whySize);

// What the save screen shows for a save. False when there is no leader.
struct iSaveFileLeader
{
    char label[64];
    S32 progress;
};
bool iSaveFileReadLeader(const U8* data, size_t size, iSaveFileLeader* out);

// The `Name=` line of an Xbox SaveMeta.xbx, which is UTF-16LE text. The value
// is the file name the save had on the Xbox, "SpongeBob00" for the first game.
bool iSaveFileXboxMetaName(const U8* data, size_t size, char* out, size_t outSize);

#endif
