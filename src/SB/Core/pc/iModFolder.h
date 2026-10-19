#ifndef IMODFOLDER_H
#define IMODFOLDER_H

#include <types.h>

#include <stddef.h>

// PC-only: mod folders, for the game and for bfbb_config.
//
// A mod is a folder laid out like the asset folder; `[assets] mod` names the
// ones in use (iFile.h). Mods installed by bfbb_config go in `mods` beside the
// executable, and the setting names them relative to it, as "mods/BFBBMix".
//
// A mod may carry a `mod.ini` in its root:
//
//   [mod]
//   name = BFBBMix
//   author = ...
//   version = 5.06
//   description = ...
//
//   [video]
//   profile = vanilla
//
// [mod] describes it. Every other section holds config.ini settings that the
// mod's values replace while it is in use; with several mods, the first that
// sets a key wins. The player's config.ini is not changed.

// `[assets] mod` may list several mods, separated by '|', highest priority
// first: for each file, the first mod in the list that has it is read. '|'
// because ';' and '#' start a comment in config.ini and no Windows path can
// hold one.
const S32 kModFolderMax = 8;
const S32 kModFolderMaxPath = 512;
typedef char iModFolderPath[kModFolderMaxPath];

// The entries of `setting`, trimmed, empty ones dropped, at most `max`.
// Returns how many.
S32 iModFolderSplit(const char* setting, iModFolderPath* out, S32 max);

// The name a set of mods keeps its saves under: each folder's last component,
// joined with '+', so one mod's saves keep the folder name they always had.
void iModFolderSaveName(const iModFolderPath* folders, S32 count, char* out, size_t outSize);

// `setting` as a folder that exists, with '/' separators and no trailing
// slash. A relative path is tried against the working directory, then against
// the executable's directory. False for an empty setting or a folder that is
// not there.
bool iModFolderResolve(const char* setting, char* out, size_t outSize);

// `<exe dir>/mods`, where installed mods go. False if the host cannot say
// where the executable is.
bool iModFolderLibrary(char* out, size_t outSize);

struct iModInfo
{
    char name[128];
    char author[128];
    char version[64];
    char description[512];

    // Whether the folder has a mod.ini at all.
    bool hasIni;
};

// The [mod] section of `folder`/mod.ini. A field mod.ini does not give is
// empty, except `name`, which falls back to the folder's last component.
void iModFolderInfo(const char* folder, iModInfo* out);

// Every setting `folder`/mod.ini gives outside [mod], in file order, as
// "section.name" and value. Returns how many there were.
typedef void (*iModFolderSettingFn)(const char* key, const char* value, void* context);
S32 iModFolderSettings(const char* folder, iModFolderSettingFn fn, void* context);

// The folder under `top` (or `top` itself) that is laid out like the asset
// folder, for an archive unpacked into `top`. Found from the packages: a
// package in a folder named for its first two letters, like `hb/hb01.HIP`,
// puts the root one level above that folder; any other, like `boot.HIP`,
// puts it in the package's own folder. The shallowest answer wins. False
// when there are no packages within four levels.
bool iModFolderFindAssets(const char* top, char* out, size_t outSize);

#endif
