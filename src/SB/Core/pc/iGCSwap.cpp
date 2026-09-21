#include "iGCSwap.h"

#include <string.h>

// Only <types.h> and the C library, so tools/gcswap/check.py can compile this
// file on its own and run it over a whole disc.

static inline void gcsSwap16(U8* p)
{
    U8 t = p[0];
    p[0] = p[1];
    p[1] = t;
}

static inline void gcsSwap32(U8* p)
{
    U8 t = p[0];
    p[0] = p[3];
    p[3] = t;
    t = p[1];
    p[1] = p[2];
    p[2] = t;
}

static inline void gcsSwap64(U8* p)
{
    for (S32 i = 0; i < 4; i++)
    {
        U8 t = p[i];
        p[i] = p[7 - i];
        p[7 - i] = t;
    }
}

// Little-endian reads, for count fields a step has already swapped.
static inline U32 gcsRead8(const U8* p)
{
    return p[0];
}

static inline U32 gcsRead16(const U8* p)
{
    return (U32)p[0] | ((U32)p[1] << 8);
}

static inline U32 gcsRead32(const U8* p)
{
    return (U32)p[0] | ((U32)p[1] << 8) | ((U32)p[2] << 16) | ((U32)p[3] << 24);
}

// A big-endian read, for a field not swapped yet.
static inline U32 gcsReadBE32(const U8* p)
{
    return ((U32)p[0] << 24) | ((U32)p[1] << 16) | ((U32)p[2] << 8) | (U32)p[3];
}

// Fields the GameCube packer swapped in some assets and not in others; see
// EITHER in tools/gcswap/spec.py. Each keeps whichever byte order reads as a
// plausible value, and swaps when both do. Plausible for a float is zero or a
// magnitude between 2^-20 and 2^20: the fields are timer ranges and heights.
static bool gcsPlausibleFloat(U32 bits)
{
    U32 exponent = (bits >> 23) & 0xFF;
    return (bits & 0x7FFFFFFF) == 0 || (exponent >= 127 - 20 && exponent <= 127 + 20);
}

static inline void gcsSwap32EitherFloat(U8* p)
{
    U32 asIs = gcsRead32(p);
    if (!gcsPlausibleFloat(gcsReadBE32(p)) && gcsPlausibleFloat(asIs))
    {
        return;
    }
    gcsSwap32(p);
}

static inline void gcsSwap32EitherInt(U8* p)
{
    if (gcsReadBE32(p) > 0xFFFF && gcsRead32(p) <= 0xFFFF)
    {
        return;
    }
    gcsSwap32(p);
}

// An xBaseAsset's links: `linkCount` xLinkAssets at the very end of the
// asset. linkCount is a byte, so it reads the same either way round. Sets *end
// to where the links begin -- the end of the type's own data.
static void gcs_xLinkAsset(U8* p);

static bool gcsSwapLinks(U8* p, U32 size, U32* end)
{
    const U32 kLinkSize = 0x20;

    if (size < 8)
    {
        return false;
    }

    U32 links = p[5] * kLinkSize;
    if (links > size)
    {
        return false;
    }

    *end = size - links;
    for (U32 at = *end; at < size; at += kLinkSize)
    {
        gcs_xLinkAsset(p + at);
    }
    return true;
}

// The hand-written rules the spec names; defined below the generated table,
// whose struct swaps they use.
static S32 iGCSwapAnim(U8* p, U32 size);
static S32 iGCSwapDisco(U8* p, U32 size);
static S32 iGCSwapModelInfo(U8* p, U32 size);

#include "iGCSwapTable.inc"

// ANIM, an SKB1 skeletal animation, laid out as iAnimEvalSKB reads it: the
// iAnimSKBHeader -- Magic, Flags, BoneCount and TimeCount (16-bit), KeyCount,
// Scale[3] -- then KeyCount iAnimSKBKeys of eight 16-bit fields each, then
// TimeCount float times, then 16-bit key offsets to the end.
static S32 iGCSwapAnim(U8* p, U32 size)
{
    const U32 kHeader = 0x1C;
    const U32 kKey = 0x10;

    if (size < kHeader)
    {
        return 0;
    }
    gcsSwap32(p + 0x0);
    gcsSwap32(p + 0x4);
    gcsSwap16(p + 0x8);
    gcsSwap16(p + 0xA);
    gcsSwap32(p + 0xC);
    gcsSwap32(p + 0x10);
    gcsSwap32(p + 0x14);
    gcsSwap32(p + 0x18);

    U32 keys = gcsRead32(p + 0xC);
    U32 times = gcsRead16(p + 0xA);
    if (keys > (size - kHeader) / kKey || times > (size - kHeader - keys * kKey) / 4)
    {
        return 0;
    }

    U32 c = kHeader;
    for (U32 end = c + keys * kKey; c < end; c += 2)
    {
        gcsSwap16(p + c);
    }
    for (U32 end = c + times * 4; c < end; c += 4)
    {
        gcsSwap32(p + c);
    }
    for (; c + 2 <= size; c += 2)
    {
        gcsSwap16(p + c);
    }
    return 1;
}

// DSCO, z_disco_floor. Unlike every other base asset its links come straight
// after the header -- zDiscoFloor.cpp takes `&asset + 1` -- and the data after
// them is addressed by offsets from `asset.flags`, byte 8: the three prefix
// strings, a table of `states_size` u32 offsets at `states_offset`, and the
// state masks, which are bytes.
static S32 iGCSwapDisco(U8* p, U32 size)
{
    const U32 kHeader = 0x2C;
    const U32 kDataBase = 0x8;

    if (size < kHeader)
    {
        return 0;
    }
    gcs_z_disco_floor_asset(p);

    U32 links = p[5] * 0x20;
    if (kHeader + links > size)
    {
        return 0;
    }
    for (U32 at = kHeader; at < kHeader + links; at += 0x20)
    {
        gcs_xLinkAsset(p + at);
    }

    U32 table = kDataBase + gcsRead32(p + 0x24);
    U32 count = gcsRead32(p + 0x28);
    if (table > size || count > (size - table) / 4)
    {
        return 0;
    }
    for (U32 i = 0; i < count; i++)
    {
        gcsSwap32(p + table + i * 4);
    }
    return 1;
}

// MINF, xModelAssetInfo: the header, NumModelInst xModelAssetInsts, then the
// parameters zParamGetString walks -- each a u32 hash, a WordLength byte, and
// text filling WordLength * 4 + 4 more bytes.
static S32 iGCSwapModelInfo(U8* p, U32 size)
{
    const U32 kInfo = 0x14;
    const U32 kInst = 0x38;

    if (size < kInfo)
    {
        return 0;
    }
    gcs_xModelAssetInfo(p);

    U32 n = gcsRead32(p + 0x4);
    if (n > (size - kInfo) / kInst)
    {
        return 0;
    }
    U32 c = kInfo;
    for (U32 i = 0; i < n; i++, c += kInst)
    {
        gcs_xModelAssetInst(p + c);
    }

    while (c + 8 <= size)
    {
        gcsSwap32(p + c);
        c += p[c + 4] * 4 + 8;
    }
    return 1;
}

iGCSwapResult iGCSwapAsset(U32 type, void* data, U32 size)
{
    S32 r = iGCSwapGenerated(type, (U8*)data, size);
    if (r < 0)
    {
        return IGCSWAP_UNKNOWN;
    }
    return r ? IGCSWAP_DONE : IGCSWAP_MALFORMED;
}

bool iGCSwapKnowsType(U32 type)
{
    return iGCSwapGeneratedKnows(type);
}
