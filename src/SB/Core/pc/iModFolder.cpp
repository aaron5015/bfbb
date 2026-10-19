// Mod folders. See iModFolder.h.

#include "iModFolder.h"

#include "iConfigEdit.h"
#include "iHost.h"
#include "iSaveFile.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

namespace
{
    const char* const kInfoSection = "mod";
    const S32 kMaxDepth = 4;

    bool isDir(const char* path)
    {
        iHostFileInfo info;
        return iHostStat(path, &info) && info.is_dir;
    }

    bool isAbsolute(const char* path)
    {
        return path[0] == '/' || (isalpha((unsigned char)path[0]) && path[1] == ':');
    }

    // Backslashes to slashes, and no trailing slash unless it is the whole
    // root ("/" or "C:/").
    void tidy(char* path)
    {
        for (char* c = path; *c != '\0'; c++)
        {
            if (*c == '\\')
            {
                *c = '/';
            }
        }

        size_t n = strlen(path);
        while (n > 1 && path[n - 1] == '/' && !(n == 3 && path[1] == ':'))
        {
            path[--n] = '\0';
        }
    }

    bool isPackage(const char* name)
    {
        size_t n = strlen(name);
        return n > 4 && (iHostStrCaseCmp(name + n - 4, ".hip") == 0 ||
                         iHostStrCaseCmp(name + n - 4, ".hop") == 0);
    }

    // Whether `name` starts with the two letters of the folder `leaf`, as
    // `hb01.HIP` does in `hb`.
    bool inItsLevelFolder(const char* name, const char* leaf)
    {
        return strlen(leaf) == 2 &&
               tolower((unsigned char)name[0]) == tolower((unsigned char)leaf[0]) &&
               tolower((unsigned char)name[1]) == tolower((unsigned char)leaf[1]);
    }

    S32 depthOf(const char* path)
    {
        S32 depth = 0;
        for (; *path != '\0'; path++)
        {
            depth += (*path == '/');
        }
        return depth;
    }

    void findAssets(const char* dir, S32 depth, char* best, size_t bestSize, S32* bestDepth)
    {
        iHostDir* d = iHostDirOpen(dir);
        if (d == NULL)
        {
            return;
        }

        char leaf[256];
        iSaveFileLeaf(dir, leaf, sizeof(leaf));

        // Collected first and walked after: iHostDirNext's string lasts only
        // until the next call, and the recursion opens handles of its own.
        char subdirs[64][256];
        S32 subdirCount = 0;

        const char* name;
        while ((name = iHostDirNext(d)) != NULL)
        {
            char path[1024];
            snprintf(path, sizeof(path), "%s/%s", dir, name);

            if (isPackage(name))
            {
                char root[1024];
                snprintf(root, sizeof(root), "%s", dir);

                if (inItsLevelFolder(name, leaf))
                {
                    char* slash = strrchr(root, '/');
                    if (slash != NULL)
                    {
                        *slash = '\0';
                    }
                }

                const S32 rootDepth = depthOf(root);
                if (*bestDepth < 0 || rootDepth < *bestDepth)
                {
                    snprintf(best, bestSize, "%s", root);
                    *bestDepth = rootDepth;
                }
            }
            else if (depth < kMaxDepth && subdirCount < 64 && isDir(path))
            {
                snprintf(subdirs[subdirCount++], sizeof(subdirs[0]), "%s", name);
            }
        }

        iHostDirClose(d);

        for (S32 i = 0; i < subdirCount; i++)
        {
            char path[1024];
            snprintf(path, sizeof(path), "%s/%s", dir, subdirs[i]);
            findAssets(path, depth + 1, best, bestSize, bestDepth);
        }
    }
} // namespace

S32 iModFolderSplit(const char* setting, iModFolderPath* out, S32 max)
{
    S32 count = 0;
    const char* p = setting != NULL ? setting : "";

    while (*p != '\0' && count < max)
    {
        const char* bar = strchr(p, '|');
        const char* end = bar != NULL ? bar : p + strlen(p);

        const char* a = p;
        const char* b = end;
        while (a < b && (*a == ' ' || *a == '\t'))
        {
            a++;
        }
        while (b > a && (b[-1] == ' ' || b[-1] == '\t'))
        {
            b--;
        }

        if (b > a)
        {
            snprintf(out[count++], kModFolderMaxPath, "%.*s", (int)(b - a), a);
        }

        if (bar == NULL)
        {
            break;
        }
        p = bar + 1;
    }

    return count;
}

void iModFolderSaveName(const iModFolderPath* folders, S32 count, char* out, size_t outSize)
{
    out[0] = '\0';
    size_t have = 0;

    for (S32 i = 0; i < count; i++)
    {
        char leaf[256];
        iSaveFileLeaf(folders[i], leaf, sizeof(leaf));

        const int n = snprintf(out + have, outSize - have, "%s%s", i == 0 ? "" : "+", leaf);
        if (n < 0 || (size_t)n >= outSize - have)
        {
            break;
        }
        have += (size_t)n;
    }
}

bool iModFolderResolve(const char* setting, char* out, size_t outSize)
{
    if (setting == NULL || setting[0] == '\0')
    {
        return false;
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s", setting);
    tidy(path);

    if (isDir(path))
    {
        snprintf(out, outSize, "%s", path);
        return true;
    }

    char dir[512];
    if (isAbsolute(path) || !iHostExeDir(dir, sizeof(dir)))
    {
        return false;
    }

    char beside[1024];
    snprintf(beside, sizeof(beside), "%s/%s", dir, path);
    tidy(beside);
    if (!isDir(beside))
    {
        return false;
    }

    snprintf(out, outSize, "%s", beside);
    return true;
}

bool iModFolderLibrary(char* out, size_t outSize)
{
    char dir[512];
    if (!iHostExeDir(dir, sizeof(dir)))
    {
        return false;
    }

    snprintf(out, outSize, "%s/mods", dir);
    return true;
}

void iModFolderInfo(const char* folder, iModInfo* out)
{
    memset(out, 0, sizeof(*out));

    char path[1024];
    snprintf(path, sizeof(path), "%s/mod.ini", folder);

    iConfigEditFile* file = iConfigEditOpen(path);
    if (file != NULL)
    {
        out->hasIni = true;

        struct
        {
            const char* key;
            char* to;
            size_t size;
        } fields[] = { { "name", out->name, sizeof(out->name) },
                       { "author", out->author, sizeof(out->author) },
                       { "version", out->version, sizeof(out->version) },
                       { "description", out->description, sizeof(out->description) } };

        for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
        {
            const char* value = iConfigEditGet(file, kInfoSection, fields[i].key);
            snprintf(fields[i].to, fields[i].size, "%s", value != NULL ? value : "");
        }

        iConfigEditClose(file);
    }

    if (out->name[0] == '\0')
    {
        iSaveFileLeaf(folder, out->name, sizeof(out->name));
    }
}

S32 iModFolderSettings(const char* folder, iModFolderSettingFn fn, void* context)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/mod.ini", folder);

    iConfigEditFile* file = iConfigEditOpen(path);
    if (file == NULL)
    {
        return 0;
    }

    S32 count = 0;
    const char* section;
    const char* name;
    for (S32 i = 0; iConfigEditKeyAt(file, i, &section, &name); i++)
    {
        if (iHostStrCaseCmp(section, kInfoSection) == 0 || section[0] == '\0')
        {
            continue;
        }

        char key[256];
        snprintf(key, sizeof(key), "%s.%s", section, name);
        for (char* c = key; *c != '\0'; c++)
        {
            *c = (char)tolower((unsigned char)*c);
        }

        const char* value = iConfigEditGet(file, section, name);
        fn(key, value != NULL ? value : "", context);
        count++;
    }

    iConfigEditClose(file);
    return count;
}

bool iModFolderFindAssets(const char* top, char* out, size_t outSize)
{
    char start[1024];
    snprintf(start, sizeof(start), "%s", top);
    tidy(start);

    S32 bestDepth = -1;
    findAssets(start, 0, out, outSize, &bestDepth);
    return bestDepth >= 0;
}
