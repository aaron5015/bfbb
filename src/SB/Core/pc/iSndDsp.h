#ifndef ISNDDSP_H
#define ISNDDSP_H

#include <types.h>

// PC-only: GameCube DSP ADPCM, the format of every SND and SNDS asset on the
// GameCube disc.
//
// Only <types.h> and the C library, so tools/gcswap/sndcheck.py can compile
// this file on its own and run it over a whole disc.
//
// The samples are 8-byte frames: a predictor/scale byte, then fourteen 4-bit
// samples, high nibble first. The predictor picks one of eight coefficient
// pairs from the sound's header. The header is not in the asset; it is the
// sound's entry in the GameCube SNDI table (see iSnd.cpp). A cutscene's SNDS
// asset is the exception and carries a copy of its header in front of the
// frames; iSndDspDataOffset finds it.

// The DSPADPCM header, in host byte order. Positions are nibble addresses: two
// per byte, with each frame's first two nibbles taken by its header byte, so
// sample 0 is nibble 2.
struct iSndDspHeader
{
    U32 num_samples; // 0x00
    U32 num_nibbles; // 0x04
    U32 sample_rate; // 0x08
    U16 loop_flag; // 0x0C
    U16 format; // 0x0E -- 0 = ADPCM
    U32 loop_start; // 0x10
    U32 loop_end; // 0x14
    U32 cur_addr; // 0x18
    S16 coef[16]; // 0x1C
    U16 gain; // 0x3C
    U16 pred_scale; // 0x3E
    S16 yn1; // 0x40
    S16 yn2; // 0x42
    U16 loop_pred_scale; // 0x44
    S16 loop_yn1; // 0x46
    S16 loop_yn2; // 0x48
    U16 pad[11]; // 0x4A
};

#define ISNDDSP_HEADER_SIZE 0x60
#define ISNDDSP_FRAME_BYTES 8
#define ISNDDSP_FRAME_SAMPLES 14

// Byte-swaps a big-endian header in place.
void iSndDspSwapHeader(void* header);

// The sample a nibble address falls on.
U32 iSndDspNibbleToSample(U32 nibble);

// The bytes `samples` samples take.
U32 iSndDspBytesFor(U32 samples);

// Where the frames start in an asset whose entry is `h`: 0, or past the
// big-endian copy of `h` a cutscene sound begins with.
U32 iSndDspDataOffset(const void* data, U32 bytes, const iSndDspHeader* h);

// Decodes h->num_samples samples of mono DSP ADPCM into `out`, starting from
// the header's history. Samples past the end of `src` are silence. Returns how
// many were decoded from data.
U32 iSndDspDecode(const void* src, U32 srcBytes, const iSndDspHeader* h, S16* out);

#endif
