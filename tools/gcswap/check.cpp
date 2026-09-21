// Runs iGCSwap.cpp over a corpus from corpus.py and compares every swapped
// GameCube asset with its Xbox twin. Built and run by check.py.
//
//     check corpus.bin [TYPE [ASSETID]]
//
// Per type (and per DYNA subtype): how many pairs, how many the same size, how
// many match exactly after the swap, and of the rest how many differ only in
// content. For each differing aligned word it asks which byte order of the raw
// GameCube word gives the Xbox one: left alone, a 32-bit swap, or two 16-bit
// swaps. If one does, the rule is wrong there. If none does, the two builds
// shipped different values, and no rule could match.
//
// With TYPE, only rows starting with it are printed, each with its words. With
// an ASSETID too, that asset is dumped word by word.

#include "iGCSwap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <vector>

struct Stats
{
    int pairs = 0, sameSize = 0, match = 0, differ = 0, contentOnly = 0, unknown = 0,
        malformed = 0;
    std::map<unsigned, std::array<int, 4>> words; // offset -> keep, swap32, swap16x2, content
    unsigned example = 0;
};

static const char* const kKind[4] = { "keep", "swap32", "swap16", "content" };

static int classify(const unsigned char* r, const unsigned char* x)
{
    if (memcmp(r, x, 4) == 0)
        return 0;
    if (r[0] == x[3] && r[1] == x[2] && r[2] == x[1] && r[3] == x[0])
        return 1;
    if (r[0] == x[1] && r[1] == x[0] && r[2] == x[3] && r[3] == x[2])
        return 2;
    return 3;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: check corpus.bin [TYPE [ASSETID]]\n");
        return 2;
    }

    FILE* f = fopen(argv[1], "rb");
    if (f == NULL)
    {
        perror(argv[1]);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> buf(n);
    fread(buf.data(), 1, n, f);
    fclose(f);

    const char* only = argc > 2 ? argv[2] : NULL;
    unsigned dumpId = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 16) : 0;

    std::map<std::string, Stats> stats;
    size_t p = 0;
    while (p + 16 <= buf.size())
    {
        char tag[5] = {};
        memcpy(tag, &buf[p], 4);
        unsigned aid, gs, xs;
        memcpy(&aid, &buf[p + 4], 4);
        memcpy(&gs, &buf[p + 8], 4);
        memcpy(&xs, &buf[p + 12], 4);
        p += 16;
        const std::vector<unsigned char> raw(buf.begin() + p, buf.begin() + p + gs);
        std::vector<unsigned char> gc = raw;
        const unsigned char* xb = &buf[p + gs];
        p += gs + xs;

        std::string key = tag;
        if (key == "DYNA" && xs >= 12)
        {
            char sub[16];
            unsigned h;
            memcpy(&h, xb + 8, 4);
            snprintf(sub, sizeof(sub), ":%08X", h);
            key += sub;
        }

        Stats& s = stats[key];
        s.pairs++;

        U32 type = ((U32)tag[0] << 24) | ((U32)tag[1] << 16) | ((U32)tag[2] << 8) | (U32)tag[3];
        iGCSwapResult r = iGCSwapAsset(type, gc.data(), gs);

        if (dumpId && aid == dumpId)
        {
            printf("%s %08X: GameCube %u bytes, Xbox %u, links %u, result %d\n", tag, aid, gs, xs,
                   xs > 5 ? xb[5] : 0, (int)r);
            unsigned lim = gs > xs ? gs : xs;
            for (unsigned i = 0; i + 4 <= lim; i += 4)
            {
                unsigned a = 0, b = 0, w = 0;
                if (i + 4 <= gs)
                {
                    memcpy(&a, &gc[i], 4);
                    memcpy(&w, &raw[i], 4);
                }
                if (i + 4 <= xs)
                {
                    memcpy(&b, xb + i, 4);
                }
                float fa, fb;
                memcpy(&fa, &a, 4);
                memcpy(&fb, &b, 4);
                printf("  %4X  raw %08X  swapped %08X  xbox %08X  %s  %g / %g\n", i, w, a, b,
                       a == b ? "  " : "<>", fa, fb);
            }
        }

        if (r == IGCSWAP_UNKNOWN)
        {
            s.unknown++;
            continue;
        }
        if (r == IGCSWAP_MALFORMED)
        {
            s.malformed++;
        }
        if (gs != xs)
        {
            continue;
        }
        s.sameSize++;
        if (memcmp(gc.data(), xb, gs) == 0)
        {
            s.match++;
            continue;
        }
        s.differ++;
        if (!s.example)
        {
            s.example = aid;
        }
        bool layout = false;
        for (unsigned w = 0; w + 4 <= gs; w += 4)
        {
            if (memcmp(&gc[w], xb + w, 4) == 0)
            {
                continue;
            }
            int kind = classify(&raw[w], xb + w);
            s.words[w][kind]++;
            layout = layout || kind != 3;
        }
        if (!layout)
        {
            s.contentOnly++;
        }
    }

    printf("%-15s %6s %6s %6s %6s %7s %6s %6s\n", "type", "pairs", "same", "match", "differ",
           "content", "unkn", "malf");
    int tp = 0, tsame = 0, tmatch = 0, tcontent = 0;
    for (auto& kv : stats)
    {
        const Stats& s = kv.second;
        if (only && kv.first.compare(0, strlen(only), only) != 0)
        {
            continue;
        }
        tp += s.pairs;
        tsame += s.sameSize;
        tmatch += s.match;
        tcontent += s.contentOnly;
        printf("%-15s %6d %6d %6d %6d %7d %6d %6d", kv.first.c_str(), s.pairs, s.sameSize, s.match,
               s.differ, s.contentOnly, s.unknown, s.malformed);
        if (s.differ > s.contentOnly)
        {
            // The words a different byte order would fix, commonest first.
            std::vector<std::pair<int, unsigned>> top;
            for (auto& w : s.words)
            {
                int fixable = w.second[0] + w.second[1] + w.second[2];
                if (fixable)
                {
                    top.push_back({-fixable, w.first});
                }
            }
            std::sort(top.begin(), top.end());
            printf("   e.g. %08X; fix:", s.example);
            for (size_t i = 0; i < top.size() && i < (only ? 32 : 5); i++)
            {
                const std::array<int, 4>& k = s.words.at(top[i].second);
                int best = 0;
                for (int j = 1; j < 3; j++)
                {
                    if (k[j] > k[best])
                    {
                        best = j;
                    }
                }
                printf(" %X=%s(%d)", top[i].second, kKind[best], k[best]);
            }
        }
        printf("\n");
    }
    printf("total: %d pairs, %d same size, %d match (%.1f%%), %d more differ only in content "
           "(%.1f%% accounted for)\n",
           tp, tsame, tmatch, tsame ? 100.0 * tmatch / tsame : 0.0, tcontent,
           tsame ? 100.0 * (tmatch + tcontent) / tsame : 0.0);
    return 0;
}
