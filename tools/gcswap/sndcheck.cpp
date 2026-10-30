// Decodes every GameCube sound with iSndDsp.cpp, through the SNDI swap in
// iGCSwap.cpp, and compares it with its Xbox twin. Built and run by
// sndcheck.py, which feeds it the records on stdin.
//
// Record, every field a little-endian u32:
//     'TBL ' path_len path gc_sndi_len gc_sndi xb_sndi_len xb_sndi
//     'SND ' asset_id gc_len gc_data xb_len xb_data
//     'END '
//
// Per sound: whether the rate, the length and the loop flag agree, and the
// correlation of the two decoded waveforms.

#include "iGCSwap.h"
#include "iSndDsp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static bool readAll(void* p, size_t n)
{
    return fread(p, 1, n, stdin) == n;
}

static U32 readU32()
{
    U32 v = 0;
    if (!readAll(&v, 4))
    {
        fprintf(stderr, "sndcheck: short read\n");
        exit(1);
    }
    return v;
}

static std::vector<U8> readBlob()
{
    std::vector<U8> b(readU32());
    if (!b.empty() && !readAll(b.data(), b.size()))
    {
        fprintf(stderr, "sndcheck: short read\n");
        exit(1);
    }
    return b;
}

struct XboxEntry
{
    U16 format_tag;
    U16 channels;
    U32 samples_per_sec;
    U32 avg_bytes_per_sec;
    U16 block_align;
    U16 bits_per_sample;
    U16 cb_size;
    U16 pad12;
    U32 data_size;
    U32 assetID;
    U32 loop_flag;
    U32 runtime[3];
};

struct GcEntry
{
    iSndDspHeader dsp;
    U32 assetID;
    int kind; // 0 sfx, 1 stream, 2 cutscene
};

static const int kImaStep[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,   21,    23,
    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,   73,    80,
    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,  253,   279,
    307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,  876,   963,
    1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749, 3024,  3327,
    3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};
static const int kImaIndex[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

// Mono Xbox ADPCM: 36-byte blocks, a predictor and step index, then 64 samples.
static std::vector<S16> decodeXboxAdpcm(const std::vector<U8>& d)
{
    std::vector<S16> out;
    for (size_t b = 0; b + 36 <= d.size(); b += 36)
    {
        int pred = (S16)(d[b] | (d[b + 1] << 8));
        int index = std::min<int>(d[b + 2], 88);
        for (int i = 4; i < 36; i++)
        {
            for (int half = 0; half < 2; half++)
            {
                int nib = half == 0 ? (d[b + i] & 0xF) : (d[b + i] >> 4);
                int step = kImaStep[index];
                int diff = step >> 3;
                if (nib & 1)
                    diff += step >> 2;
                if (nib & 2)
                    diff += step >> 1;
                if (nib & 4)
                    diff += step;
                pred = (nib & 8) ? pred - diff : pred + diff;
                pred = std::max(-32768, std::min(32767, pred));
                out.push_back((S16)pred);
                index = std::max(0, std::min(88, index + kImaIndex[nib]));
            }
        }
    }
    return out;
}

static std::vector<S16> decodeXbox(const XboxEntry& e, const std::vector<U8>& d)
{
    if (e.format_tag == 0x69)
    {
        return decodeXboxAdpcm(d);
    }
    std::vector<S16> out(d.size() / 2);
    memcpy(out.data(), d.data(), out.size() * 2);
    return out;
}

static U32 xboxFrames(const XboxEntry& e)
{
    if (e.block_align == 0)
        return 0;
    if (e.format_tag == 0x69)
        return e.data_size / e.block_align * 64;
    return e.data_size / e.block_align;
}

static double correlate(const std::vector<S16>& a, const std::vector<S16>& b, int lag)
{
    size_t n = std::min(a.size(), b.size());
    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
    size_t count = 0;
    for (size_t i = 0; i < n; i++)
    {
        long j = (long)i + lag;
        if (j < 0 || (size_t)j >= b.size())
            continue;
        double x = a[i], y = b[j];
        sa += x;
        sb += y;
        saa += x * x;
        sbb += y * y;
        sab += x * y;
        count++;
    }
    if (count == 0)
        return 0;
    double va = saa - sa * sa / count, vb = sbb - sb * sb / count;
    if (va <= 0 || vb <= 0)
        return (va <= 0 && vb <= 0) ? 1.0 : 0.0;
    return (sab - sa * sb / count) / sqrt(va * vb);
}

static std::vector<S16> halve(const std::vector<S16>& a)
{
    std::vector<S16> out(a.size() / 2);
    for (size_t i = 0; i < out.size(); i++)
        out[i] = (S16)(((int)a[2 * i] + a[2 * i + 1]) / 2);
    return out;
}

// Linear interpolation from one rate down to a lower one.
static std::vector<S16> resample(const std::vector<S16>& a, U32 from, U32 to)
{
    if (from == 2 * to)
        return halve(a);
    std::vector<S16> out((size_t)((double)a.size() * to / from));
    for (size_t i = 0; i < out.size(); i++)
    {
        double t = (double)i * from / to;
        size_t j = (size_t)t;
        double f = t - j;
        double v = j + 1 < a.size() ? a[j] * (1 - f) + a[j + 1] * f : a[j];
        out[i] = (S16)v;
    }
    return out;
}

static double rms(const std::vector<S16>& a)
{
    double s = 0;
    for (S16 v : a)
        s += (double)v * v;
    return a.empty() ? 0 : sqrt(s / a.size());
}

struct Row
{
    U32 aid;
    std::string table;
    int kind;
    double corr;
    int lag;
    U32 gcRate, xbRate, gcLen, xbLen;
    bool gcLoop, xbLoop;
    double gain;
    U32 xbTag;
};

int main()
{
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif

    std::map<U32, GcEntry> gc;
    std::map<U32, XboxEntry> xb;
    std::string table;
    std::vector<Row> rows;

    int tables = 0, tablesSwapped = 0, tablesSized = 0;
    int noTwin = 0, noXboxData = 0, noGcData = 0, shortData = 0, shortFrame = 0, headed = 0,
        headedCut = 0;
    int loopStartNonZero = 0, loopEntries = 0;

    for (;;)
    {
        U32 tag = readU32();
        if (tag == 'END ')
        {
            break;
        }

        if (tag == 'TBL ')
        {
            std::vector<U8> path = readBlob();
            table.assign(path.begin(), path.end());
            std::vector<U8> g = readBlob();
            std::vector<U8> x = readBlob();
            tables++;

            gc.clear();
            xb.clear();

            if (iGCSwapAsset('SNDI', g.data(), (U32)g.size()) != IGCSWAP_DONE)
            {
                printf("%s: the GameCube SNDI did not swap\n", table.c_str());
                continue;
            }
            tablesSwapped++;

            U32 h[4];
            memcpy(h, g.data(), 16);
            U32 n = h[0] + h[2] + h[3];
            if (h[1] == IGCSWAP_SNDI_GAMECUBE && g.size() == 16 + 0x64 * (size_t)n)
            {
                tablesSized++;
            }
            for (U32 i = 0; i < n; i++)
            {
                GcEntry e;
                memcpy(&e.dsp, g.data() + 16 + i * 0x64, ISNDDSP_HEADER_SIZE);
                memcpy(&e.assetID, g.data() + 16 + i * 0x64 + 0x60, 4);
                e.kind = i < h[0] ? 0 : (i < h[0] + h[2] ? 1 : 2);
                gc[e.assetID] = e;
            }

            if (x.size() >= 12)
            {
                U32 xh[3];
                memcpy(xh, x.data(), 12);
                U32 xn = xh[0] + xh[1] + xh[2];
                for (U32 i = 0; i < xn && 12 + (i + 1) * 44 <= x.size(); i++)
                {
                    XboxEntry e;
                    memcpy(&e, x.data() + 12 + i * 44, 44);
                    xb[e.assetID] = e;
                }
            }
            continue;
        }

        if (tag != 'SND ')
        {
            fprintf(stderr, "sndcheck: bad record\n");
            return 1;
        }

        U32 aid = readU32();
        std::vector<U8> gd = readBlob();
        std::vector<U8> xd = readBlob();

        auto gi = gc.find(aid);
        if (gi == gc.end())
        {
            continue;
        }
        const GcEntry& g = gi->second;

        if (g.dsp.loop_flag)
        {
            loopEntries++;
            if (iSndDspNibbleToSample(g.dsp.loop_start) != 0)
            {
                loopStartNonZero++;
            }
        }

        if (gd.empty())
        {
            noGcData++;
            continue;
        }

        U32 offset = iSndDspDataOffset(gd.data(), (U32)gd.size(), &g.dsp);
        if (offset != 0)
        {
            headed++;
            headedCut += g.kind == 2;
        }
        size_t need = iSndDspBytesFor(g.dsp.num_samples);
        if (gd.size() - offset + ISNDDSP_FRAME_BYTES <= need)
        {
            shortData++;
        }
        else if (gd.size() - offset < need)
        {
            shortFrame++;
        }

        std::vector<S16> pcm(g.dsp.num_samples);
        iSndDspDecode(gd.data() + offset, (U32)(gd.size() - offset), &g.dsp, pcm.data());

        auto xi = xb.find(aid);
        if (xi == xb.end())
        {
            noTwin++;
            continue;
        }
        if (xd.empty())
        {
            noXboxData++;
            continue;
        }
        const XboxEntry& x = xi->second;
        std::vector<S16> xpcm = decodeXbox(x, xd);

        // Some sounds were authored at a different rate in the two builds.
        // Those are compared at the lower rate.
        std::vector<S16> gcmp = pcm;
        if (x.samples_per_sec > g.dsp.sample_rate && g.dsp.sample_rate != 0)
        {
            xpcm = resample(xpcm, x.samples_per_sec, g.dsp.sample_rate);
        }
        else if (g.dsp.sample_rate > x.samples_per_sec && x.samples_per_sec != 0)
        {
            gcmp = resample(gcmp, g.dsp.sample_rate, x.samples_per_sec);
        }

        Row r;
        r.aid = aid;
        r.table = table;
        r.kind = g.kind;
        r.gcRate = g.dsp.sample_rate;
        r.xbRate = x.samples_per_sec;
        r.gcLen = g.dsp.num_samples;
        r.xbLen = xboxFrames(x);
        r.gcLoop = g.dsp.loop_flag != 0;
        r.xbLoop = x.loop_flag != 0;
        r.xbTag = x.format_tag;
        r.lag = 0;
        r.corr = correlate(gcmp, xpcm, 0);
        if (r.corr < 0.99)
        {
            for (int lag = -64; lag <= 64; lag++)
            {
                double c = correlate(gcmp, xpcm, lag);
                if (c > r.corr)
                {
                    r.corr = c;
                    r.lag = lag;
                }
            }
        }
        double xr = rms(xpcm);
        r.gain = xr > 0 ? rms(gcmp) / xr : 0;
        rows.push_back(r);
    }

    int rate = 0, len = 0, lenExact = 0, loop = 0, lagged = 0, halfRate = 0, halfLen = 0;
    int bands[5] = {};
    int kinds[3] = {};
    for (const Row& r : rows)
    {
        rate += r.gcRate == r.xbRate;
        lenExact += r.gcLen == r.xbLen;
        // Xbox ADPCM ends on a whole 64-sample block.
        len +=
            r.gcLen == r.xbLen || (r.xbTag == 0x69 && r.xbLen >= r.gcLen && r.xbLen - r.gcLen < 64);
        loop += r.gcLoop == r.xbLoop;
        lagged += r.lag != 0;
        if (r.gcRate * 2 == r.xbRate || r.xbRate * 2 == r.gcRate)
        {
            halfRate++;
            // The same duration: within a block of samples at the higher rate.
            double t = (double)r.gcLen / r.gcRate - (double)r.xbLen / r.xbRate;
            halfLen += fabs(t) * std::max(r.gcRate, r.xbRate) < 64;
        }
        kinds[r.kind]++;
        int b = r.corr >= 0.999 ? 0 :
                r.corr >= 0.99  ? 1 :
                r.corr >= 0.95  ? 2 :
                r.corr >= 0.9   ? 3 :
                                  4;
        bands[b]++;
    }

    printf("tables %d, swapped %d, tagged and sized %d\n", tables, tablesSwapped, tablesSized);
    printf("sounds compared %zu (sfx %d, streams %d, cutscene %d)\n", rows.size(), kinds[0],
           kinds[1], kinds[2]);
    printf("  no Xbox entry %d, no Xbox data %d, no GameCube data %d\n", noTwin, noXboxData,
           noGcData);
    printf("  GameCube data short by a whole frame %d, by part of the last one %d\n", shortData,
           shortFrame);
    printf("  carrying their own header %d (cutscene %d)\n", headed, headedCut);
    printf("  looping %d, loop start past sample 0: %d\n", loopEntries, loopStartNonZero);
    printf("rate matches %d/%zu; half or double the Xbox rate %d\n", rate, rows.size(), halfRate);
    printf("length matches %d/%zu (exactly %d); of the half-rate ones, same duration %d\n", len,
           rows.size(), lenExact, halfLen);
    printf("loop flag matches %d/%zu\n", loop, rows.size());
    printf("correlation >= 0.999: %d, 0.99-0.999: %d, 0.95-0.99: %d, 0.9-0.95: %d, < 0.9: %d\n",
           bands[0], bands[1], bands[2], bands[3], bands[4]);
    printf("best at a nonzero lag: %d\n", lagged);

    std::vector<double> c;
    for (const Row& r : rows)
        c.push_back(r.corr);
    std::sort(c.begin(), c.end());
    if (!c.empty())
    {
        printf("correlation median %.5f, min %.5f\n", c[c.size() / 2], c.front());
    }

    std::vector<double> same;
    for (const Row& r : rows)
        if (r.gcRate == r.xbRate)
            same.push_back(r.corr);
    std::sort(same.begin(), same.end());
    if (!same.empty())
    {
        printf("at the same rate: %zu sounds, median %.5f, >= 0.999: %zd, >= 0.99: %zd\n",
               same.size(), same[same.size() / 2],
               same.end() - std::lower_bound(same.begin(), same.end(), 0.999),
               same.end() - std::lower_bound(same.begin(), same.end(), 0.99));
    }

    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.corr < b.corr; });
    printf("\nlowest correlation:\n");
    for (size_t i = 0; i < rows.size() && i < 20; i++)
    {
        const Row& r = rows[i];
        printf(
            "  %08x %-18s %s corr %.4f lag %d gain %.3f rate %u/%u len %u/%u loop %d/%d xb tag %x\n",
            r.aid, r.table.c_str(),
            r.kind == 0 ? "sfx" :
            r.kind == 1 ? "str" :
                          "cut",
            r.corr, r.lag, r.gain, r.gcRate, r.xbRate, r.gcLen, r.xbLen, r.gcLoop, r.xbLoop,
            r.xbTag);
    }

    printf("\nmismatched length or loop flag, at the same rate:\n");
    for (const Row& r : rows)
    {
        bool lenOk =
            r.gcLen == r.xbLen || (r.xbTag == 0x69 && r.xbLen >= r.gcLen && r.xbLen - r.gcLen < 64);
        if (r.gcRate == r.xbRate && (!lenOk || r.gcLoop != r.xbLoop))
        {
            printf("  %08x %-18s rate %u/%u len %u/%u loop %d/%d corr %.4f\n", r.aid,
                   r.table.c_str(), r.gcRate, r.xbRate, r.gcLen, r.xbLen, r.gcLoop, r.xbLoop,
                   r.corr);
        }
    }
    return 0;
}
