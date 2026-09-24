// GameCube native textures, read into a raster for the current platform.
//
// RenderWare's GameCube driver writes a native texture as a big-endian struct:
//
//   0x00  u32  platform (6)
//   0x04  u32  filter and addressing
//   0x08  12 bytes, not needed here
//   0x18  char name[32]
//   0x38  char mask[32]
//   0x58  u32  RW raster format
//   0x5C  u16  width, u16 height
//   0x60  u8   depth, u8 levels, u8 GX texture format, u8 GX palette format
//   0x64  8 bytes, not needed here
//   0x6C  palette (for C4/C8), then the levels, largest first, in GX tiles
//
// Only level 0 is decoded. It becomes an RGBA image and goes through
// Raster::createFromImage like any other image, so the result is a raster of
// whatever platform is current and nothing downstream knows where it came from.
//
// Mods that mix GameCube assets into an Xbox package ship these (BFBBMix has
// four); no retail Xbox package does.

#include <stdio.h>
#include <string.h>

#include "rwbase.h"
#include "rwerror.h"
#include "rwplg.h"
#include "rwpipeline.h"
#include "rwobjects.h"
#include "rwengine.h"

#define PLUGIN_ID 0

namespace rw {

enum
{
	GX_TF_I4 = 0,
	GX_TF_I8 = 1,
	GX_TF_IA4 = 2,
	GX_TF_IA8 = 3,
	GX_TF_RGB565 = 4,
	GX_TF_RGB5A3 = 5,
	GX_TF_RGBA8 = 6,
	GX_TF_C4 = 8,
	GX_TF_C8 = 9,
	GX_TF_CMPR = 14,

	GX_TL_IA8 = 0,
	GX_TL_RGB565 = 1,
	GX_TL_RGB5A3 = 2,

	GC_HEADER_SIZE = 0x6C
};

static uint32
be16(const uint8 *p)
{
	return (uint32)p[0]<<8 | p[1];
}

static uint32
be32(const uint8 *p)
{
	return (uint32)p[0]<<24 | (uint32)p[1]<<16 | (uint32)p[2]<<8 | p[3];
}

static void
rgb565(uint32 v, uint8 *out)
{
	uint32 r = (v>>11)&0x1F, g = (v>>5)&0x3F, b = v&0x1F;
	out[0] = r<<3 | r>>2;
	out[1] = g<<2 | g>>4;
	out[2] = b<<3 | b>>2;
	out[3] = 255;
}

// Top bit set: opaque RGB555. Clear: three bits of alpha, then RGB444.
static void
rgb5a3(uint32 v, uint8 *out)
{
	if(v & 0x8000){
		uint32 r = (v>>10)&0x1F, g = (v>>5)&0x1F, b = v&0x1F;
		out[0] = r<<3 | r>>2;
		out[1] = g<<3 | g>>2;
		out[2] = b<<3 | b>>2;
		out[3] = 255;
	}else{
		uint32 a = (v>>12)&7;
		out[0] = ((v>>8)&0xF)*17;
		out[1] = ((v>>4)&0xF)*17;
		out[2] = (v&0xF)*17;
		out[3] = a<<5 | a<<2 | a>>1;
	}
}

// High byte alpha, low byte intensity.
static void
ia8(uint32 v, uint8 *out)
{
	out[0] = out[1] = out[2] = v&0xFF;
	out[3] = v>>8;
}

static void
paletteColor(uint32 v, int32 palfmt, uint8 *out)
{
	switch(palfmt){
	case GX_TL_IA8: ia8(v, out); break;
	case GX_TL_RGB565: rgb565(v, out); break;
	default: rgb5a3(v, out); break;
	}
}

// Width and height of one tile, in texels, and its size in bytes.
static bool32
tileShape(int32 fmt, int32 *tw, int32 *th, int32 *bytes)
{
	switch(fmt){
	case GX_TF_I4:
	case GX_TF_C4:
	case GX_TF_CMPR:
		*tw = 8; *th = 8; *bytes = 32; return 1;
	case GX_TF_I8:
	case GX_TF_IA4:
	case GX_TF_C8:
		*tw = 8; *th = 4; *bytes = 32; return 1;
	case GX_TF_IA8:
	case GX_TF_RGB565:
	case GX_TF_RGB5A3:
		*tw = 4; *th = 4; *bytes = 32; return 1;
	case GX_TF_RGBA8:
		*tw = 4; *th = 4; *bytes = 64; return 1;
	}
	return 0;
}

// One tile's texels into `rgba`, a tw*th block of RGBA8888.
static void
decodeTile(int32 fmt, const uint8 *src, const uint8 *pal, int32 palfmt, uint8 *rgba)
{
	int32 i;
	switch(fmt){
	case GX_TF_I4:
		for(i = 0; i < 64; i++){
			uint32 n = (src[i/2] >> (i&1 ? 0 : 4)) & 0xF;
			rgba[i*4+0] = rgba[i*4+1] = rgba[i*4+2] = rgba[i*4+3] = n*17;
		}
		break;
	case GX_TF_I8:
		for(i = 0; i < 32; i++)
			rgba[i*4+0] = rgba[i*4+1] = rgba[i*4+2] = rgba[i*4+3] = src[i];
		break;
	case GX_TF_IA4:
		for(i = 0; i < 32; i++){
			rgba[i*4+0] = rgba[i*4+1] = rgba[i*4+2] = (src[i]&0xF)*17;
			rgba[i*4+3] = (src[i]>>4)*17;
		}
		break;
	case GX_TF_IA8:
		for(i = 0; i < 16; i++)
			ia8(be16(src + i*2), rgba + i*4);
		break;
	case GX_TF_RGB565:
		for(i = 0; i < 16; i++)
			rgb565(be16(src + i*2), rgba + i*4);
		break;
	case GX_TF_RGB5A3:
		for(i = 0; i < 16; i++)
			rgb5a3(be16(src + i*2), rgba + i*4);
		break;
	case GX_TF_RGBA8:
		// Thirty-two bytes of AR pairs, then thirty-two of GB.
		for(i = 0; i < 16; i++){
			rgba[i*4+3] = src[i*2];
			rgba[i*4+0] = src[i*2+1];
			rgba[i*4+1] = src[32 + i*2];
			rgba[i*4+2] = src[32 + i*2+1];
		}
		break;
	case GX_TF_C4:
		for(i = 0; i < 64; i++){
			uint32 n = (src[i/2] >> (i&1 ? 0 : 4)) & 0xF;
			paletteColor(be16(pal + n*2), palfmt, rgba + i*4);
		}
		break;
	case GX_TF_C8:
		for(i = 0; i < 32; i++)
			paletteColor(be16(pal + src[i]*2), palfmt, rgba + i*4);
		break;
	case GX_TF_CMPR:
		// Four DXT1 blocks, left to right and top to bottom, with big-endian
		// endpoints and the leftmost texel in each row's top two bits.
		for(int32 sb = 0; sb < 4; sb++){
			const uint8 *blk = src + sb*8;
			uint32 c0 = be16(blk), c1 = be16(blk + 2);
			uint8 c[4][4];
			rgb565(c0, c[0]);
			rgb565(c1, c[1]);
			for(int32 k = 0; k < 3; k++){
				if(c0 > c1){
					c[2][k] = (2*c[0][k] + c[1][k])/3;
					c[3][k] = (c[0][k] + 2*c[1][k])/3;
				}else{
					c[2][k] = (c[0][k] + c[1][k])/2;
					c[3][k] = 0;
				}
			}
			c[2][3] = 255;
			c[3][3] = c0 > c1 ? 255 : 0;
			int32 bx = (sb&1)*4, by = (sb>>1)*4;
			for(int32 y = 0; y < 4; y++)
				for(int32 x = 0; x < 4; x++){
					uint32 idx = (blk[4+y] >> (6 - 2*x)) & 3;
					memcpy(rgba + ((by+y)*8 + bx+x)*4, c[idx], 4);
				}
		}
		break;
	}
}

Image*
readGCTextureImage(const uint8 *buf, uint32 length)
{
	if(length < GC_HEADER_SIZE)
		return nil;

	int32 width = be16(buf + 0x5C);
	int32 height = be16(buf + 0x5E);
	int32 fmt = buf[0x62];
	int32 palfmt = buf[0x63];

	int32 tw, th, tbytes;
	if(!tileShape(fmt, &tw, &th, &tbytes) || width <= 0 || height <= 0)
		return nil;

	const uint8 *pal = buf + GC_HEADER_SIZE;
	const uint8 *pix = pal;
	if(fmt == GX_TF_C4 || fmt == GX_TF_C8)
		pix += (fmt == GX_TF_C4 ? 16 : 256)*2;

	int32 tilesX = (width + tw-1)/tw;
	int32 tilesY = (height + th-1)/th;
	if(pix + tilesX*tilesY*tbytes > buf + length)
		return nil;

	Image *img = Image::create(width, height, 32);
	img->allocate();
	uint8 tile[8*8*4];
	for(int32 ty = 0; ty < tilesY; ty++)
		for(int32 tx = 0; tx < tilesX; tx++){
			decodeTile(fmt, pix, pal, palfmt, tile);
			pix += tbytes;
			for(int32 y = 0; y < th; y++){
				int32 py = ty*th + y;
				if(py >= height)
					break;
				for(int32 x = 0; x < tw; x++){
					int32 px = tx*tw + x;
					if(px >= width)
						break;
					memcpy(img->pixels + py*img->stride + px*4, tile + (y*tw + x)*4, 4);
				}
			}
		}
	return img;
}

Texture*
readNativeTextureGC(Stream *stream)
{
	uint32 length;
	if(!findChunk(stream, ID_STRUCT, &length, nil)){
		RWERROR((ERR_CHUNK, "STRUCT"));
		return nil;
	}

	uint8 *buf = rwNewT(uint8, length, MEMDUR_FUNCTION | ID_TEXTURE);
	if(stream->read8(buf, length) != length){
		rwFree(buf);
		return nil;
	}

	Image *img = readGCTextureImage(buf, length);
	if(img == nil){
		RWERROR((ERR_PLATFORM, 6));
		rwFree(buf);
		return nil;
	}

	Raster *raster = Raster::createFromImage(img);
	img->destroy();
	if(raster == nil){
		rwFree(buf);
		return nil;
	}

	Texture *tex = Texture::create(raster);
	tex->filterAddressing = be32(buf + 0x04);
	memcpy(tex->name, buf + 0x18, 32);
	tex->name[31] = '\0';
	memcpy(tex->mask, buf + 0x38, 32);
	tex->mask[31] = '\0';
	rwFree(buf);
	return tex;
}

}
