// Save file paths and contents for code outside the game. See iSaveFile.h.

#include "iSaveFile.h"

#include "iHost.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{
    const size_t kHeaderSize = 16;
    const size_t kChunkHeaderSize = 12;

    // Tags are written as native U32s of a multi-character constant, so 'GDAT'
    // is the bytes "TADG" in the file.
    const U32 kTagSave = ((U32)'G' << 24) | ((U32)'D' << 16) | ((U32)'A' << 8) | (U32)'T';
    const U32 kTagLeader = ((U32)'L' << 24) | ((U32)'E' << 16) | ((U32)'D' << 8) | (U32)'R';

    const char* const kGameBase = "SpongeBob";

    U32 readU32(const U8* p)
    {
        return (U32)p[0] | ((U32)p[1] << 8) | ((U32)p[2] << 16) | ((U32)p[3] << 24);
    }
} // namespace

void iSaveFileRoot(const char* configured, const char* modName, char* out, size_t outSize)
{
    const char* explicitDir = getenv("BFBB_SAVE_DIR");
    if (explicitDir != NULL && explicitDir[0] != '\0')
    {
        snprintf(out, outSize, "%s", explicitDir);
        return;
    }

    if (configured != NULL && configured[0] != '\0')
    {
        snprintf(out, outSize, "%s", configured);
        return;
    }

    char base[512];
    if (!iHostUserDataDir(base, sizeof(base)))
    {
        base[0] = '\0';
    }
    const char* sep = base[0] != '\0' ? "/bfbb/" : "";

    if (modName != NULL && modName[0] != '\0')
    {
        snprintf(out, outSize, "%s%smods/%s/saves", base, sep, modName);
        return;
    }

    snprintf(out, outSize, "%s%ssaves", base, sep);
}

void iSaveFileLeaf(const char* path, char* out, size_t outSize)
{
    size_t n = strlen(path);
    while (n > 0 && (path[n - 1] == '/' || path[n - 1] == '\\'))
    {
        n--;
    }

    size_t start = n;
    while (start > 0 && path[start - 1] != '/' && path[start - 1] != '\\' && path[start - 1] != ':')
    {
        start--;
    }

    snprintf(out, outSize, "%.*s", (int)(n - start), path + start);
}

void iSaveFileName(S32 game, char* out, size_t outSize)
{
    snprintf(out, outSize, "%s%02d", kGameBase, (int)game);
}

S32 iSaveFileGameOf(const char* name)
{
    for (S32 game = 0; game < kSaveFileGames; game++)
    {
        char want[32];
        iSaveFileName(game, want, sizeof(want));
        if (iHostStrCaseCmp(name, want) == 0)
        {
            return game;
        }
    }
    return -1;
}

U32 iSaveFileCrc(const U8* data, size_t size)
{
    U32 crc = 0xFFFFFFFF;
    for (size_t i = 0; i < size; i++)
    {
        crc ^= (U32)data[i] << 24;
        for (S32 bit = 0; bit < 8; bit++)
        {
            crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04C11DB7 : crc << 1;
        }
    }
    return crc;
}

bool iSaveFileCheck(const U8* data, size_t size, size_t* total, char* why, size_t whySize)
{
    if (size < kHeaderSize || readU32(data) != kTagSave)
    {
        snprintf(why, whySize, "This is not a Battle for Bikini Bottom save.");
        return false;
    }

    if (readU32(data + 4) != 1)
    {
        snprintf(why, whySize, "The save is version %u; only version 1 is known.",
                 (unsigned)readU32(data + 4));
        return false;
    }

    const size_t want = readU32(data + 8);
    if (want < kHeaderSize || want > size)
    {
        snprintf(why, whySize, "The save says it is %u bytes, but the file has %u.", (unsigned)want,
                 (unsigned)size);
        return false;
    }

    if (iSaveFileCrc(data + kHeaderSize, want - kHeaderSize) != readU32(data + 12))
    {
        snprintf(why, whySize, "The save is damaged: its checksum does not match.");
        return false;
    }

    *total = want;
    return true;
}

bool iSaveFileReadLeader(const U8* data, size_t size, iSaveFileLeader* out)
{
    memset(out, 0, sizeof(*out));

    size_t at = kHeaderSize;
    while (at + kChunkHeaderSize <= size)
    {
        const U32 tag = readU32(data + at);
        const size_t maxamt = readU32(data + at + 4);
        const U8* body = data + at + kChunkHeaderSize;

        if (tag == kTagLeader)
        {
            // st_XSAVEGAME_LEADER: gameLabel[64], then S32 progress.
            if (at + kChunkHeaderSize + 68 > size)
            {
                return false;
            }
            memcpy(out->label, body, sizeof(out->label));
            out->label[sizeof(out->label) - 1] = '\0';
            out->progress = (S32)readU32(body + 64);
            return true;
        }

        if (maxamt == 0)
        {
            break;
        }
        at += kChunkHeaderSize + maxamt;
    }

    return false;
}

bool iSaveFileXboxMetaName(const U8* data, size_t size, char* out, size_t outSize)
{
    // Narrowed to ASCII; a save name is "SpongeBob00" and nothing wider.
    char text[512];
    size_t n = 0;
    size_t at = (size >= 2 && data[0] == 0xFF && data[1] == 0xFE) ? 2 : 0;
    for (; at + 1 < size && n + 1 < sizeof(text); at += 2)
    {
        const U32 c = (U32)data[at] | ((U32)data[at + 1] << 8);
        text[n++] = c < 0x80 ? (char)c : '?';
    }
    text[n] = '\0';

    const char* line = text;
    while (line != NULL && *line != '\0')
    {
        while (*line == '\r' || *line == '\n' || *line == ' ')
        {
            line++;
        }

        if (strncmp(line, "Name=", 5) == 0)
        {
            const char* value = line + 5;
            size_t len = strcspn(value, "\r\n");
            while (len > 0 && value[len - 1] == ' ')
            {
                len--;
            }
            snprintf(out, outSize, "%.*s", (int)len, value);
            return len > 0;
        }

        line = strchr(line, '\n');
    }

    return false;
}
