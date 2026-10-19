// The saves page's model. See saves_model.h.

#include "saves_model.h"

#include "config_model.h"
#include "iHost.h"
#include "iModFolder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{
    // Larger than any save; a GDAT is about 50 KB.
    const size_t kMaxSave = 1024 * 1024;

    const char* textOf(const char* section, const char* name)
    {
        const S32 i = ConfigModelFind(section, name);
        return i >= 0 ? ConfigModelText(i) : "";
    }

    void gamePath(S32 game, const char* suffix, char* out, size_t outSize)
    {
        char folder[kSavesModelMaxPath];
        char name[32];
        SavesModelFolder(folder, sizeof(folder));
        iSaveFileName(game, name, sizeof(name));
        snprintf(out, outSize, "%s/%s%s", folder, name, suffix);
    }

    U8* readAll(const char* path, size_t* size)
    {
        FILE* f = fopen(path, "rb");
        if (f == NULL)
        {
            return NULL;
        }

        U8* data = (U8*)malloc(kMaxSave);
        size_t n = data != NULL ? fread(data, 1, kMaxSave, f) : 0;
        fclose(f);

        *size = n;
        return data;
    }

    bool makeDirs(const char* path)
    {
        char tmp[kSavesModelMaxPath];
        snprintf(tmp, sizeof(tmp), "%s", path);

        for (char* p = tmp + 1; *p != '\0'; p++)
        {
            if (*p == '/' || *p == '\\')
            {
                const char c = *p;
                *p = '\0';
                if (!(p == tmp + 2 && tmp[1] == ':'))
                {
                    iHostMakeDir(tmp);
                }
                *p = c;
            }
        }
        return iHostMakeDir(tmp);
    }

    bool writeFile(const char* path, const U8* data, size_t size)
    {
        FILE* f = fopen(path, "wb");
        if (f == NULL)
        {
            return false;
        }
        bool ok = fwrite(data, 1, size, f) == size;
        ok = (fclose(f) == 0) && ok;
        return ok;
    }
} // namespace

void SavesModelFolder(char* out, size_t outSize)
{
    // Named as iFileModName names them: the folders that are there, in order.
    iModFolderPath listed[kModFolderMax];
    iModFolderPath found[kModFolderMax];
    const S32 count = iModFolderSplit(textOf("assets", "mod"), listed, kModFolderMax);
    S32 have = 0;
    for (S32 i = 0; i < count; i++)
    {
        if (iModFolderResolve(listed[i], found[have], sizeof(found[have])))
        {
            have++;
        }
    }

    char mod[kSavesModelMaxPath];
    iModFolderSaveName(found, have, mod, sizeof(mod));

    iSaveFileRoot(textOf("game", "save_folder"), mod, out, outSize);
}

bool SavesModelRead(S32 game, iSaveFileLeader* out)
{
    char path[kSavesModelMaxPath + 32];
    gamePath(game, "", path, sizeof(path));

    size_t size = 0;
    U8* data = readAll(path, &size);
    if (data == NULL)
    {
        return false;
    }

    char why[256];
    size_t total;
    const bool ok = iSaveFileCheck(data, size, &total, why, sizeof(why)) &&
                    iSaveFileReadLeader(data, total, out);
    free(data);
    return ok;
}

bool SavesModelReadXbox(const U8* xsv, size_t xsvSize, const U8* meta, size_t metaSize,
                        const char* source, SavesModelXbox* out, char* why, size_t whySize)
{
    memset(out, 0, sizeof(*out));
    out->game = -1;
    snprintf(out->source, sizeof(out->source), "%s", source);

    size_t total;
    if (!iSaveFileCheck(xsv, xsvSize, &total, why, whySize))
    {
        return false;
    }

    out->data = (U8*)malloc(total);
    if (out->data == NULL)
    {
        snprintf(why, whySize, "Out of memory.");
        return false;
    }
    memcpy(out->data, xsv, total);
    out->size = total;

    iSaveFileReadLeader(out->data, out->size, &out->leader);

    char name[64];
    if (meta != NULL && iSaveFileXboxMetaName(meta, metaSize, name, sizeof(name)))
    {
        out->game = iSaveFileGameOf(name);
    }
    return true;
}

void SavesModelFreeXbox(SavesModelXbox* save)
{
    free(save->data);
    save->data = NULL;
    save->size = 0;
}

bool SavesModelImport(const SavesModelXbox* save, S32 game, char* why, size_t whySize)
{
    if (game < 0 || game >= kSaveFileGames)
    {
        snprintf(why, whySize, "There is no game %d.", (int)game + 1);
        return false;
    }

    char folder[kSavesModelMaxPath];
    SavesModelFolder(folder, sizeof(folder));
    if (!makeDirs(folder))
    {
        snprintf(why, whySize, "Could not make the save folder %s.", folder);
        return false;
    }

    char path[kSavesModelMaxPath + 32];
    char tmp[kSavesModelMaxPath + 32];
    char bak[kSavesModelMaxPath + 32];
    char thumb[kSavesModelMaxPath + 32];
    gamePath(game, "", path, sizeof(path));
    gamePath(game, ".tmp", tmp, sizeof(tmp));
    gamePath(game, ".bak", bak, sizeof(bak));
    gamePath(game, ".tga", thumb, sizeof(thumb));

    if (!writeFile(tmp, save->data, save->size))
    {
        iHostRemoveFile(tmp);
        snprintf(why, whySize, "Could not write %s.", tmp);
        return false;
    }

    if (iHostPathExists(path) && !iHostRenameReplace(path, bak))
    {
        iHostRemoveFile(tmp);
        snprintf(why, whySize, "Could not move the save already in game %d aside.", (int)game + 1);
        return false;
    }

    if (!iHostRenameReplace(tmp, path))
    {
        iHostRemoveFile(tmp);
        snprintf(why, whySize, "Could not write %s.", path);
        return false;
    }

    if (iHostPathExists(thumb))
    {
        iHostRemoveFile(thumb);
    }
    return true;
}
