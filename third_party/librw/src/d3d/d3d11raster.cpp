#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define WITH_D3D
#include "../rwbase.h"
#include "../rwplg.h"
#include "../rwerror.h"
#include "../rwrender.h"
#include "../rwengine.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "rwd3d.h"
#include "rwd3d11.h"
#include "rwd3dimpl.h"

#define PLUGIN_ID 0

namespace rw {
namespace d3d {

// The two device implementations live one namespace deeper than the interface
// they implement, so that a build can carry both. rw::d3d is the interface --
// the raster layer, the immediate mode and the pipelines are written against it
// and are built once -- and d3ddispatch.cpp forwards each of its entry points
// to whichever of these is running.
//
// A call in here that passes one of rw::d3d's enumerators has to name this
// namespace: the enumerator's own namespace is rw::d3d, so argument-dependent
// lookup adds the forwarder to the candidates beside the function meant, and
// the two have the same signature.
namespace impl11 {

#ifdef RW_D3D11

// D3DFMT_ is what the whole d3d driver speaks -- the native texture reader
// writes it into D3dRaster::format and the xbox converter hands it out -- so it
// stays the currency here and is translated at the one point a D3D11 resource
// is made.
DXGI_FORMAT
formatToDXGI(uint32 format)
{
	switch(format){
	case D3DFMT_A8R8G8B8:	return DXGI_FORMAT_B8G8R8A8_UNORM;
	case D3DFMT_X8R8G8B8:	return DXGI_FORMAT_B8G8R8X8_UNORM;
	case D3DFMT_A8B8G8R8:	return DXGI_FORMAT_R8G8B8A8_UNORM;
	case D3DFMT_R5G6B5:	return DXGI_FORMAT_B5G6R5_UNORM;
	case D3DFMT_A1R5G5B5:	return DXGI_FORMAT_B5G5R5A1_UNORM;
	case D3DFMT_A4R4G4B4:	return DXGI_FORMAT_B4G4R4A4_UNORM;
	case D3DFMT_A8:		return DXGI_FORMAT_A8_UNORM;
	// No luminance and no X1R5G5B5: widenLevel turns both into B8G8R8A8, and
	// this is the format the widened texels are in.
	case D3DFMT_L8:
	case D3DFMT_A8L8:
	case D3DFMT_X1R5G5B5:	return DXGI_FORMAT_B8G8R8A8_UNORM;
	case D3DFMT_DXT1:	return DXGI_FORMAT_BC1_UNORM;
	case D3DFMT_DXT2:
	case D3DFMT_DXT3:	return DXGI_FORMAT_BC2_UNORM;
	case D3DFMT_DXT4:
	case D3DFMT_DXT5:	return DXGI_FORMAT_BC3_UNORM;
	case D3DFMT_D16:	return DXGI_FORMAT_D16_UNORM;
	case D3DFMT_D24S8:
	case D3DFMT_D24X8:	return DXGI_FORMAT_D24_UNORM_S8_UINT;
	case D3DFMT_D32:	return DXGI_FORMAT_D32_FLOAT;
	}
	return DXGI_FORMAT_UNKNOWN;
}

// Whether the device will make a texture in this format at all. B5G6R5 and
// B4G4R4A4 arrived with Windows 8; before that, and on a driver that still
// declines them, the widened path stands in.
static bool32
formatSupported(DXGI_FORMAT fmt)
{
	UINT support = 0;
	if(FAILED(d3d11device->CheckFormatSupport(fmt, &support)))
		return 0;
	return (support & D3D11_FORMAT_SUPPORT_TEXTURE2D) != 0;
}

// Whether the raster's texels have to be widened to B8G8R8A8 on the way to the
// GPU. Three reasons, and they are all D3D11 having dropped something:
//
//   - No luminance format. R8 would put the value in red alone and leave green
//     and blue at zero, where D3D9 replicated it across all three.
//   - No X1R5G5B5. B5G5R5A1 is not a stand-in, because the bit D3D9 ignored
//     becomes alpha -- and a texture written with it clear is wholly
//     transparent rather than opaque.
//   - The two packed formats that need Windows 8, on a device without them.
static bool32
widensToBGRA(uint32 format)
{
	switch(format){
	case D3DFMT_L8:
	case D3DFMT_A8L8:
	case D3DFMT_X1R5G5B5:
		return 1;
	case D3DFMT_R5G6B5:
	case D3DFMT_A4R4G4B4:
		return !formatSupported(formatToDXGI(format));
	}
	return 0;
}

// One level's texels, widened into B8G8R8A8 -- byte order blue, green, red,
// alpha, which is the uint32 written here on a little-endian machine.
static void
widenLevel(uint32 format, const uint8 *src, uint32 srcPitch,
	int32 width, int32 height, uint32 *dst)
{
	for(int32 y = 0; y < height; y++){
		const uint8 *row = src + y*srcPitch;
		for(int32 x = 0; x < width; x++){
			uint32 r, g, b, a;
			switch(format){
			case D3DFMT_L8:
				r = g = b = row[x];
				a = 0xFF;
				break;
			case D3DFMT_A8L8:
				r = g = b = row[x*2];
				a = row[x*2+1];
				break;
			default: {
				uint32 v = row[x*2] | (row[x*2+1] << 8);
				if(format == D3DFMT_R5G6B5){
					r = (v >> 11) & 0x1F; r = (r << 3) | (r >> 2);
					g = (v >> 5) & 0x3F;  g = (g << 2) | (g >> 4);
					b = v & 0x1F;         b = (b << 3) | (b >> 2);
					a = 0xFF;
				}else if(format == D3DFMT_A4R4G4B4){
					a = ((v >> 12) & 0xF) * 0x11;
					r = ((v >> 8) & 0xF) * 0x11;
					g = ((v >> 4) & 0xF) * 0x11;
					b = (v & 0xF) * 0x11;
				}else{	// D3DFMT_X1R5G5B5
					r = (v >> 10) & 0x1F; r = (r << 3) | (r >> 2);
					g = (v >> 5) & 0x1F;  g = (g << 3) | (g >> 2);
					b = v & 0x1F;         b = (b << 3) | (b >> 2);
					a = 0xFF;
				}
				break;
			}
			}
			dst[y*width + x] = (a << 24) | (r << 16) | (g << 8) | b;
		}
	}
}

static bool
isBlockCompressed(uint32 format)
{
	return format == D3DFMT_DXT1 || format == D3DFMT_DXT2 || format == D3DFMT_DXT3 ||
	       format == D3DFMT_DXT4 || format == D3DFMT_DXT5;
}

// Bytes from the start of one row of the source image to the start of the next.
// Taken from the level's own size rather than recomputed from the format: for a
// block format a "row" is a row of 4x4 blocks, and createTexture already did
// that arithmetic when it laid the level out.
static uint32
levelRowPitch(uint32 format, RasterLevels::Level *level)
{
	int32 rows = isBlockCompressed(format) ? (level->height + 3)/4 : level->height;
	if(rows <= 0)
		rows = 1;
	return (uint32)(level->size / rows);
}

// --- textures ---------------------------------------------------------------

// Make the GPU texture and its view from the system copy. Deferred to the first
// upload rather than done in rasterCreate, because a raster's texels arrive
// after it exists and a texture with no data in it is nothing to make early.
static bool32
createTextureResource(Raster *raster, D3dRaster *natras)
{
	RasterLevels *levels = (RasterLevels*)natras->texture;
	DXGI_FORMAT fmt = widensToBGRA(natras->format) ?
		DXGI_FORMAT_B8G8R8A8_UNORM : formatToDXGI(natras->format);
	if(fmt == DXGI_FORMAT_UNKNOWN || levels == nil)
		return 0;

	D3D11_TEXTURE2D_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.Width = levels->levels[0].width;
	desc.Height = levels->levels[0].height;
	desc.MipLevels = levels->numlevels;
	desc.ArraySize = 1;
	desc.Format = fmt;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	ID3D11Texture2D *tex = nil;
	if(FAILED(d3d11device->CreateTexture2D(&desc, nil, &tex)))
		return 0;
	ID3D11ShaderResourceView *srv = nil;
	if(FAILED(d3d11device->CreateShaderResourceView(tex, nil, &srv))){
		tex->Release();
		return 0;
	}
	natras->tex11 = tex;
	natras->srv = srv;
	d3d11Globals.numTextures++;
	return 1;
}

// Push the system copy at the GPU. Cheap when nothing has changed, which is the
// common case: the game writes a raster's texels once at load.
void
rasterUpload(Raster *raster)
{
	D3dRaster *natras = GETD3DRASTEREXT(raster);
	if(!natras->dirty)
		return;
	natras->dirty = 0;

	if(natras->tex11 == nil && !createTextureResource(raster, natras))
		return;

	RasterLevels *levels = (RasterLevels*)natras->texture;
	bool32 widen = widensToBGRA(natras->format);
	for(int32 i = 0; i < levels->numlevels; i++){
		RasterLevels::Level *level = &levels->levels[i];
		if(!widen){
			d3d11context->UpdateSubresource((ID3D11Texture2D*)natras->tex11, i, nil,
				level->data, levelRowPitch(natras->format, level), 0);
			continue;
		}
		uint32 *wide = rwNewT(uint32, level->width*level->height,
			MEMDUR_FUNCTION | ID_DRIVER);
		widenLevel(natras->format, level->data,
			levelRowPitch(natras->format, level),
			level->width, level->height, wide);
		d3d11context->UpdateSubresource((ID3D11Texture2D*)natras->tex11, i, nil,
			wide, level->width*4, 0);
		rwFree(wide);
	}
}

void*
rasterShaderResource(Raster *raster)
{
	D3dRaster *natras = GETD3DRASTEREXT(raster);
	rasterUpload(raster);
	return natras->srv;
}

// --- camera targets and z buffers -------------------------------------------

Raster*
rasterCreateCameraTexture(Raster *raster)
{
	D3dRaster *natras = GETD3DRASTEREXT(raster);
	DXGI_FORMAT fmt = formatToDXGI(natras->format);
	if(fmt == DXGI_FORMAT_UNKNOWN){
		RWERROR((ERR_NOTEXTURE));
		return nil;
	}

	D3D11_TEXTURE2D_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.Width = raster->width;
	desc.Height = raster->height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = fmt;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

	ID3D11Texture2D *tex = nil;
	if(FAILED(d3d11device->CreateTexture2D(&desc, nil, &tex))){
		RWERROR((ERR_NOTEXTURE));
		return nil;
	}
	if(FAILED(d3d11device->CreateRenderTargetView(tex, nil, (ID3D11RenderTargetView**)&natras->rtv)) ||
	   FAILED(d3d11device->CreateShaderResourceView(tex, nil, (ID3D11ShaderResourceView**)&natras->srv))){
		tex->Release();
		RWERROR((ERR_NOTEXTURE));
		return nil;
	}
	natras->tex11 = tex;
	// No system copy: a camera texture is written by the GPU, and reading it
	// back goes through a staging texture rather than through this.
	natras->texture = nil;
	d3d11Globals.numTextures++;
	return raster;
}

Raster*
rasterCreateCamera(Raster *raster)
{
	D3dRaster *natras = GETD3DRASTEREXT(raster);
	natras->autogenMipmap = 0;
	natras->format = D3DFMT_A8R8G8B8;
	raster->depth = 32;
	// nil means the back buffer, the same convention the D3D9 backend uses.
	natras->texture = nil;
	return raster;
}

Raster*
rasterCreateZbuffer(Raster *raster)
{
	D3dRaster *natras = GETD3DRASTEREXT(raster);
	natras->autogenMipmap = 0;
	natras->format = D3DFMT_D24S8;
	raster->depth = 32;

	D3D11_TEXTURE2D_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.Width = raster->width;
	desc.Height = raster->height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

	ID3D11Texture2D *tex = nil;
	if(FAILED(d3d11device->CreateTexture2D(&desc, nil, &tex))){
		RWERROR((ERR_NOTEXTURE));
		return nil;
	}
	if(FAILED(d3d11device->CreateDepthStencilView(tex, nil, (ID3D11DepthStencilView**)&natras->dsv))){
		tex->Release();
		RWERROR((ERR_NOTEXTURE));
		return nil;
	}
	natras->tex11 = tex;
	natras->texture = nil;
	return raster;
}

// --- reading a target back --------------------------------------------------

// The frame buffer and camera textures live only on the GPU, so a lock is a
// copy to a staging texture and a read out of that. The result is handed back
// in a system buffer the raster keeps, which is what raster->pixels points at
// until unlock.
uint8*
rasterLockTarget(Raster *raster, int32 level, int32 lockMode)
{
	D3dRaster *natras = GETD3DRASTEREXT(raster);

	if(lockMode & Raster::LOCKWRITE){
		assert(0 && "can't lock a render target for writing");
		return nil;
	}

	ID3D11Texture2D *src;
	if(raster->type == Raster::CAMERA){
		// The virtual screen is what the scene was drawn into; the back buffer
		// holds the previous frame's letterboxed blit, in another channel
		// order. Only with no virtual screen is the back buffer the picture.
		src = virtualScreenTexture();
		if(src)
			src->AddRef();
		else if(FAILED(d3d11Globals.swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&src)))
			return nil;
	}else{
		src = (ID3D11Texture2D*)natras->tex11;
		if(src == nil)
			return nil;
		src->AddRef();
	}

	D3D11_TEXTURE2D_DESC desc;
	src->GetDesc(&desc);
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	desc.MiscFlags = 0;

	ID3D11Texture2D *staging = nil;
	if(FAILED(d3d11device->CreateTexture2D(&desc, nil, &staging))){
		src->Release();
		return nil;
	}
	d3d11context->CopyResource(staging, src);
	src->Release();

	D3D11_MAPPED_SUBRESOURCE map;
	if(FAILED(d3d11context->Map(staging, 0, D3D11_MAP_READ, 0, &map))){
		staging->Release();
		return nil;
	}

	uint32 bpp = natras->bpp;
	uint8 *pixels = (uint8*)rwNew(desc.Width*desc.Height*bpp, MEMDUR_EVENT | ID_DRIVER);
	for(UINT y = 0; y < desc.Height; y++)
		memcpy(pixels + y*desc.Width*bpp, (uint8*)map.pData + y*map.RowPitch, desc.Width*bpp);
	d3d11context->Unmap(staging, 0);
	staging->Release();

	// Kept so unlock can free it; nothing else reads lockedSurf on this
	// backend.
	natras->lockedSurf = pixels;
	raster->pixels = pixels;
	raster->width = desc.Width;
	raster->height = desc.Height;
	raster->stride = desc.Width*bpp;
	return pixels;
}

void
rasterUnlockTarget(Raster *raster)
{
	D3dRaster *natras = GETD3DRASTEREXT(raster);
	rwFree(natras->lockedSurf);
	natras->lockedSurf = nil;
}

// --- teardown ---------------------------------------------------------------

void
rasterDestroy(Raster *raster, D3dRaster *natras)
{
	forgetRaster(raster);
	if(natras->srv){
		((ID3D11ShaderResourceView*)natras->srv)->Release();
		natras->srv = nil;
	}
	if(natras->rtv){
		((ID3D11RenderTargetView*)natras->rtv)->Release();
		natras->rtv = nil;
	}
	if(natras->dsv){
		((ID3D11DepthStencilView*)natras->dsv)->Release();
		natras->dsv = nil;
	}
	if(natras->tex11){
		((ID3D11Texture2D*)natras->tex11)->Release();
		natras->tex11 = nil;
		d3d11Globals.numTextures--;
	}
}

#endif
}
}
}
