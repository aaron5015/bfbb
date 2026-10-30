#include "iSndDsp.h"

#include <stddef.h>
#include <string.h>

static_assert(offsetof(iSndDspHeader, coef) == 0x1C, "coef sits at 0x1C in the DSPADPCM header");
static_assert(offsetof(iSndDspHeader, loop_pred_scale) == 0x44,
              "the loop context sits at 0x44 in the DSPADPCM header");
static_assert(sizeof(iSndDspHeader) == ISNDDSP_HEADER_SIZE, "the DSPADPCM header is 0x60 bytes");

static inline void iSwap16(U8* p)
{
    U8 t = p[0];
    p[0] = p[1];
    p[1] = t;
}

static inline void iSwap32(U8* p)
{
    U8 t = p[0];
    p[0] = p[3];
    p[3] = t;
    t = p[1];
    p[1] = p[2];
    p[2] = t;
}

static inline U32 iReadBE32(const U8* p)
{
    return ((U32)p[0] << 24) | ((U32)p[1] << 16) | ((U32)p[2] << 8) | (U32)p[3];
}

void iSndDspSwapHeader(void* header)
{
    U8* p = (U8*)header;

    // num_samples, num_nibbles, sample_rate
    for (U32 c = 0x00; c < 0x0C; c += 4)
    {
        iSwap32(p + c);
    }

    // loop_flag, format
    iSwap16(p + 0x0C);
    iSwap16(p + 0x0E);

    // loop_start, loop_end, cur_addr
    for (U32 c = 0x10; c < 0x1C; c += 4)
    {
        iSwap32(p + c);
    }

    // coef[16], gain, the context, the loop context and the padding
    for (U32 c = 0x1C; c < ISNDDSP_HEADER_SIZE; c += 2)
    {
        iSwap16(p + c);
    }
}

U32 iSndDspNibbleToSample(U32 nibble)
{
    U32 frame = nibble / 16;
    U32 within = nibble % 16;

    return frame * ISNDDSP_FRAME_SAMPLES + (within >= 2 ? within - 2 : 0);
}

U32 iSndDspBytesFor(U32 samples)
{
    return (samples + ISNDDSP_FRAME_SAMPLES - 1) / ISNDDSP_FRAME_SAMPLES * ISNDDSP_FRAME_BYTES;
}

U32 iSndDspDataOffset(const void* data, U32 bytes, const iSndDspHeader* h)
{
    const U8* p = (const U8*)data;

    // Some cutscene sounds stop a few bytes into their last frame, so the size
    // is not a test.
    if (bytes >= ISNDDSP_HEADER_SIZE && iReadBE32(p + 0x00) == h->num_samples &&
        iReadBE32(p + 0x04) == h->num_nibbles && iReadBE32(p + 0x08) == h->sample_rate)
    {
        return ISNDDSP_HEADER_SIZE;
    }

    return 0;
}

static inline S16 iClamp16(S32 v)
{
    if (v < -32768)
    {
        return -32768;
    }

    if (v > 32767)
    {
        return 32767;
    }

    return (S16)v;
}

U32 iSndDspDecode(const void* src, U32 srcBytes, const iSndDspHeader* h, S16* out)
{
    const U8* p = (const U8*)src;
    U32 total = h->num_samples;
    U32 frames = (srcBytes + ISNDDSP_FRAME_BYTES - 1) / ISNDDSP_FRAME_BYTES;

    S32 hist1 = h->yn1;
    S32 hist2 = h->yn2;
    U32 n = 0;

    for (U32 f = 0; f < frames && n < total; f++)
    {
        const U8* frame = p + f * ISNDDSP_FRAME_BYTES;
        U32 avail = srcBytes - f * ISNDDSP_FRAME_BYTES;
        S32 scale = 1 << (frame[0] & 0xF);
        U32 pair = (frame[0] >> 4) & 7;
        S32 c1 = h->coef[pair * 2];
        S32 c2 = h->coef[pair * 2 + 1];

        // A last frame cut short decodes as far as its bytes go.
        U32 count = avail >= ISNDDSP_FRAME_BYTES ? ISNDDSP_FRAME_SAMPLES : (avail - 1) * 2;

        for (U32 i = 0; i < count && n < total; i++)
        {
            U8 b = frame[1 + i / 2];
            S32 nibble = (i & 1) ? (b & 0xF) : (b >> 4);
            if (nibble >= 8)
            {
                nibble -= 16;
            }

            S32 s = ((nibble * scale) << 11) + 1024 + c1 * hist1 + c2 * hist2;
            S16 sample = iClamp16(s >> 11);

            out[n++] = sample;
            hist2 = hist1;
            hist1 = sample;
        }
    }

    U32 decoded = n;
    if (n < total)
    {
        memset(out + n, 0, (size_t)(total - n) * sizeof(S16));
    }

    return decoded;
}
