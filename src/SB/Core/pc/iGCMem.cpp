#include "iGCMem.h"
#include "iModPatch.h"

#include "xEvent.h"
#include "xLinkAsset.h"
#include "zGlobals.h"

#include <stdio.h>

// `count` scalars of `width` bytes from GameCube address `address`, and the
// first of them on the PC.
struct iGCMemField
{
    U32 address;
    U16 width;
    U16 count;
    void* pc;
};

#include "iGCMemTable.inc"

// An index past this either way is not exact as an F32.
static const S32 kMaxIndex = 1 << 24;

// The field of globals at `address`, or NULL.
static U8* fieldAddress(U32 address)
{
    U32 lo = 0;
    U32 hi = sizeof(sGCMemFields) / sizeof(sGCMemFields[0]);
    while (lo < hi)
    {
        U32 mid = (lo + hi) / 2;
        if (sGCMemFields[mid].address <= address)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid;
        }
    }
    if (lo == 0)
    {
        return NULL;
    }

    const iGCMemField& f = sGCMemFields[lo - 1];
    U32 at = address - f.address;
    if (at >= (U32)f.width * f.count)
    {
        return NULL;
    }

    // The GameCube is big-endian: its byte k of a scalar is the PC's byte
    // width - 1 - k.
    U32 element = at / f.width;
    U32 byte = at % f.width;
    return (U8*)f.pc + element * f.width + (f.width - 1 - byte);
}

U8* iGCMemAddress(U32 address)
{
    U8* pc = fieldAddress(address);
    return pc != NULL ? pc : iModPatchByte(address);
}

// Whether `address` has been reported, so that a link copied 254 times says so
// once.
static bool reported(U32 address)
{
    static U32 sReported[64];
    static U32 sCount;
    for (U32 i = 0; i < sCount; i++)
    {
        if (sReported[i] == address)
        {
            return true;
        }
    }
    if (sCount < sizeof(sReported) / sizeof(sReported[0]))
    {
        sReported[sCount++] = address;
    }
    return false;
}

void iGCMemLink(void* data)
{
    xLinkAsset* link = (xLinkAsset*)data;
    if (link->dstEvent != eEventGivePowerUp)
    {
        return;
    }

    S32 index = (S32)link->param[0];
    if (index >= 0 && index < (S32)sizeof(globals.player.g.PowerUp))
    {
        return;
    }

    U32 address = IGCMEM_POWERUP + (U32)index;
    U8* pc = iGCMemAddress(address);
    if (pc != NULL)
    {
        ptrdiff_t at = pc - globals.player.g.PowerUp;
        if (at > -kMaxIndex && at < kMaxIndex)
        {
            link->param[0] = (F32)at;
            return;
        }
    }

    if (!reported(address))
    {
        printf("bfbb: dropping GivePowerUp links that write GameCube address %08X, which the "
               "port does not map\n",
               address);
        fflush(stdout);
    }
    link->dstEvent = eEventUnknown;
}
