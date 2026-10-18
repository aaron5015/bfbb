// The mods page's model. See mods_model.h.

#include "mods_model.h"

#include "config_model.h"
#include "iHost.h"
#include "iSaveFile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{
    const S32 kMaxMods = 128;
    const size_t kMaxName = 256;

    ModsModelEntry gMods[kMaxMods];
    S32 gModCount;
    char gLibrary[kModsModelMaxPath];

    // The prefix a library entry's setting carries, relative to the exe dir.
    const char* const kLibraryPrefix = "mods/";

    // Names in a folder, copied out: iHostDirNext's string lasts only until
    // the next call, and a walk that recurses opens handles of its own.
    struct NameList
    {
        char (*names)[kMaxName];
        S32 count;
    };

    bool listDir(const char* dir, NameList* out)
    {
        out->names = NULL;
        out->count = 0;

        iHostDir* d = iHostDirOpen(dir);
        if (d == NULL)
        {
            return false;
        }

        S32 room = 0;
        const char* name;
        while ((name = iHostDirNext(d)) != NULL)
        {
            if (out->count == room)
            {
                room = room == 0 ? 32 : room * 2;
                void* grown = realloc(out->names, (size_t)room * kMaxName);
                if (grown == NULL)
                {
                    break;
                }
                out->names = (char(*)[kMaxName])grown;
            }
            snprintf(out->names[out->count++], kMaxName, "%s", name);
        }

        iHostDirClose(d);
        return true;
    }

    void freeList(NameList* list)
    {
        free(list->names);
        list->names = NULL;
        list->count = 0;
    }

    bool isDir(const char* path)
    {
        iHostFileInfo info;
        return iHostStat(path, &info) && info.is_dir;
    }

    bool samePath(const char* a, const char* b)
    {
        char x[kModsModelMaxPath];
        char y[kModsModelMaxPath];
        if (!iHostAbsolutePath(a, x, sizeof(x)))
        {
            snprintf(x, sizeof(x), "%s", a);
        }
        if (!iHostAbsolutePath(b, y, sizeof(y)))
        {
            snprintf(y, sizeof(y), "%s", b);
        }
        return iHostStrCaseCmp(x, y) == 0;
    }

    S32 modSetting()
    {
        return ConfigModelFind("assets", "mod");
    }

    void addEntry(const char* folder, const char* setting, bool inLibrary)
    {
        if (gModCount == kMaxMods)
        {
            return;
        }

        ModsModelEntry* e = &gMods[gModCount++];
        memset(e, 0, sizeof(*e));
        snprintf(e->folder, sizeof(e->folder), "%s", folder);
        snprintf(e->setting, sizeof(e->setting), "%s", setting);
        e->inLibrary = inLibrary;
        iModFolderInfo(folder, &e->info);
    }

    bool copyFile(const char* from, const char* to)
    {
        FILE* in = fopen(from, "rb");
        if (in == NULL)
        {
            return false;
        }
        FILE* out = fopen(to, "wb");
        if (out == NULL)
        {
            fclose(in);
            return false;
        }

        bool ok = true;
        char buffer[65536];
        size_t n;
        while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0)
        {
            if (fwrite(buffer, 1, n, out) != n)
            {
                ok = false;
                break;
            }
        }

        ok = ok && !ferror(in);
        fclose(in);
        ok = (fclose(out) == 0) && ok;
        return ok;
    }

    bool copyTree(const char* from, const char* to)
    {
        if (!iHostMakeDir(to))
        {
            return false;
        }

        NameList list;
        if (!listDir(from, &list))
        {
            return false;
        }

        bool ok = true;
        for (S32 i = 0; i < list.count && ok; i++)
        {
            char src[1024];
            char dst[1024];
            snprintf(src, sizeof(src), "%s/%s", from, list.names[i]);
            snprintf(dst, sizeof(dst), "%s/%s", to, list.names[i]);
            ok = isDir(src) ? copyTree(src, dst) : copyFile(src, dst);
        }

        freeList(&list);
        return ok;
    }

    S32 findFolder(const char* folder)
    {
        for (S32 i = 0; i < gModCount; i++)
        {
            if (samePath(gMods[i].folder, folder))
            {
                return i;
            }
        }
        return -1;
    }
} // namespace

const char* ModsModelLibrary()
{
    if (gLibrary[0] == '\0' && !iModFolderLibrary(gLibrary, sizeof(gLibrary)))
    {
        gLibrary[0] = '\0';
    }
    return gLibrary;
}

S32 ModsModelScan()
{
    gModCount = 0;

    const char* library = ModsModelLibrary();
    NameList list;
    if (library[0] != '\0' && listDir(library, &list))
    {
        // Sorted by name, so the list does not follow the order the
        // filesystem happens to return.
        qsort(list.names, (size_t)list.count, kMaxName, [](const void* a, const void* b) {
            return iHostStrCaseCmp((const char*)a, (const char*)b);
        });

        for (S32 i = 0; i < list.count; i++)
        {
            // Dot folders are the install's staging folders, and anything
            // else a tool hides.
            if (list.names[i][0] == '.')
            {
                continue;
            }

            char folder[kModsModelMaxPath];
            char setting[kModsModelMaxPath];
            snprintf(folder, sizeof(folder), "%s/%s", library, list.names[i]);
            snprintf(setting, sizeof(setting), "%s%s", kLibraryPrefix, list.names[i]);
            if (isDir(folder))
            {
                addEntry(folder, setting, true);
            }
        }
        freeList(&list);
    }

    // The setting may name a folder somewhere else; it is listed too, so the
    // page shows what the game will run.
    const S32 s = modSetting();
    char folder[kModsModelMaxPath];
    if (s >= 0 && iModFolderResolve(ConfigModelText(s), folder, sizeof(folder)) &&
        findFolder(folder) < 0)
    {
        addEntry(folder, ConfigModelText(s), false);
    }

    return gModCount;
}

S32 ModsModelCount()
{
    return gModCount;
}

const ModsModelEntry* ModsModelAt(S32 i)
{
    return &gMods[i];
}

S32 ModsModelActive()
{
    const S32 s = modSetting();
    if (s < 0 || ConfigModelText(s)[0] == '\0')
    {
        return -1;
    }

    char folder[kModsModelMaxPath];
    if (!iModFolderResolve(ConfigModelText(s), folder, sizeof(folder)))
    {
        return -2;
    }

    const S32 at = findFolder(folder);
    return at >= 0 ? at : -2;
}

void ModsModelUse(S32 i)
{
    const S32 s = modSetting();
    if (s >= 0)
    {
        ConfigModelSetText(s, i < 0 ? "" : gMods[i].setting);
    }
}

void ModsModelUseFolder(const char* folder)
{
    const S32 s = modSetting();
    if (s >= 0)
    {
        ConfigModelSetText(s, folder);
    }
    ModsModelScan();
}

void ModsModelSuggestName(const char* staged, const char* archive, char* out, size_t outSize)
{
    char root[kModsModelMaxPath];
    if (iModFolderFindAssets(staged, root, sizeof(root)) && !samePath(root, staged))
    {
        iSaveFileLeaf(root, out, outSize);
        return;
    }

    char leaf[kModsModelMaxPath];
    iSaveFileLeaf(archive, leaf, sizeof(leaf));
    char* dot = strrchr(leaf, '.');
    if (dot != NULL && dot != leaf)
    {
        *dot = '\0';
    }
    snprintf(out, outSize, "%s", leaf);
}

bool ModsModelNameOk(const char* name, char* why, size_t whySize)
{
    if (name[0] == '\0' || name[0] == '.' || name[0] == ' ')
    {
        snprintf(why, whySize, "Give the mod a folder name. It cannot start with '.' or a space.");
        return false;
    }

    if (strpbrk(name, "/\\:*?\"<>|") != NULL)
    {
        snprintf(why, whySize, "A folder name cannot contain / \\ : * ? \" < > or |.");
        return false;
    }

    char folder[kModsModelMaxPath];
    snprintf(folder, sizeof(folder), "%s/%s", ModsModelLibrary(), name);
    if (iHostPathExists(folder))
    {
        snprintf(why, whySize, "There is already a mod called %s.", name);
        return false;
    }

    return true;
}

bool ModsModelStage(char* out, size_t outSize, char* why, size_t whySize)
{
    const char* library = ModsModelLibrary();
    if (library[0] == '\0' || !iHostMakeDir(library))
    {
        snprintf(why, whySize, "Could not make the mods folder %s.", library);
        return false;
    }

    for (S32 n = 0; n < 1000; n++)
    {
        snprintf(out, outSize, "%s/.install-%d", library, (int)n);
        if (!iHostPathExists(out) && iHostMakeDir(out))
        {
            return true;
        }
    }

    snprintf(why, whySize, "Could not make a folder in %s to unpack into.", library);
    return false;
}

bool ModsModelInstall(const char* staged, const char* name, S32* index, char* why, size_t whySize)
{
    char root[kModsModelMaxPath];
    if (!iModFolderFindAssets(staged, root, sizeof(root)))
    {
        snprintf(why, whySize,
                 "This does not look like a mod: there are no .HIP or .HOP files in it.");
        return false;
    }

    if (!ModsModelNameOk(name, why, whySize))
    {
        return false;
    }

    char dest[kModsModelMaxPath];
    snprintf(dest, sizeof(dest), "%s/%s", ModsModelLibrary(), name);

    if (!iHostRenameReplace(root, dest))
    {
        snprintf(why, whySize, "Could not move the mod into %s.", dest);
        return false;
    }

    if (iHostPathExists(staged))
    {
        ModsModelRemoveTree(staged);
    }

    ModsModelScan();
    *index = findFolder(dest);
    return true;
}

bool ModsModelCopyIn(const char* source, const char* name, S32* index, char* why, size_t whySize)
{
    char root[kModsModelMaxPath];
    if (!iModFolderFindAssets(source, root, sizeof(root)))
    {
        snprintf(why, whySize,
                 "This does not look like a mod: there are no .HIP or .HOP files in it.");
        return false;
    }

    if (!ModsModelNameOk(name, why, whySize))
    {
        return false;
    }

    const char* library = ModsModelLibrary();
    char dest[kModsModelMaxPath];
    snprintf(dest, sizeof(dest), "%s/%s", library, name);

    if (!iHostMakeDir(library) || !copyTree(root, dest))
    {
        ModsModelRemoveTree(dest);
        snprintf(why, whySize, "Could not copy the mod into %s.", dest);
        return false;
    }

    ModsModelScan();
    *index = findFolder(dest);
    return true;
}

bool ModsModelRemove(S32 i, char* why, size_t whySize)
{
    if (i < 0 || i >= gModCount || !gMods[i].inLibrary)
    {
        snprintf(why, whySize, "Only a mod in %s can be removed here.", ModsModelLibrary());
        return false;
    }

    char folder[kModsModelMaxPath];
    snprintf(folder, sizeof(folder), "%s", gMods[i].folder);

    if (ModsModelActive() == i)
    {
        ModsModelUse(-1);
    }

    if (!ModsModelRemoveTree(folder))
    {
        snprintf(why, whySize, "Could not delete everything in %s.", folder);
        ModsModelScan();
        return false;
    }

    ModsModelScan();
    return true;
}

bool ModsModelRemoveTree(const char* folder)
{
    NameList list;
    if (!listDir(folder, &list))
    {
        return !iHostPathExists(folder);
    }

    bool ok = true;
    for (S32 i = 0; i < list.count; i++)
    {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", folder, list.names[i]);
        ok = (isDir(path) ? ModsModelRemoveTree(path) : iHostRemoveFile(path)) && ok;
    }
    freeList(&list);

    return iHostRemoveDir(folder) && ok;
}
