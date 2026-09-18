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
#include "rwd3d9.h"
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

// The device's state, and how a D3D9-shaped driver reaches it.
//
// D3D9 sets one key at a time and the runtime assembles a pipeline; D3D11 binds
// four immutable objects. The whole driver -- the object pipelines, the
// immediate-mode paths, the application through SetRenderState -- speaks the
// first language, so this file keeps a shadow of the D3D9 render states, and
// flushCache turns whatever they currently say into the four objects. Objects
// are cached by their description, because the same handful recur all frame.

// --- the shadow states ------------------------------------------------------

// Stages 0 and 1 are the material and the environment map; stage 3 is the cel
// ramp, uploaded by d3drender.cpp's uploadToonConstants. Stage 2 is unused and
// binds the white texture.
#define NUMTEXSTAGES 4

static struct {
	// Blend
	uint32 srcblend, destblend;
	bool32 blendenable;
	uint32 colorwritemask;

	// Depth and stencil
	bool32 ztest, zwrite;
	bool32 stencilenable;
	uint32 stencilfail, stencilzfail, stencilpass, stencilfunc;
	uint32 stencilref, stencilmask, stencilwritemask;

	// Rasterizer
	uint32 cullmode;
	bool32 scissorenable;

	// Sampler, per stage
	struct {
		Raster *raster;
		uint32 addressingU, addressingV;
		uint32 filter;
		uint32 maxAnisotropy;
	} texstage[NUMTEXSTAGES];

	// What the application asked for, kept apart from what a pipeline asked
	// for: the two are ORed, and neither may erase the other.
	bool32 appVertexAlpha;
	bool32 pipelineVertexAlpha;
	bool32 vertexAlpha;

	// The stage-0 raster's own transparency, and whether the artist meant it
	// as a cutout. Both feed updateAlphaStates.
	bool32 textureAlpha;
	bool32 textureKeyed;

	// Alpha test. D3D11 has no such render state -- the pixel shader clips --
	// so these are uploaded rather than bound.
	uint32 alphafunc;
	uint32 alpharef;
	uint32 gsalpha;
	uint32 gsalpharef;

	bool32 fogenable;
	RGBA fogcolor;
} rwStateCache;

static bool32 stateDirty = 1;
static bool32 im2DActive;

// The shader-visible state the lighting and matrix uploads in d3drender.cpp
// keep. Its D3D9 twin lives in d3ddevice.cpp for the same reason: it belongs to
// whichever file owns the device.

// The material's colour and surface properties, to both shader stages. The
// pixel stage gets them for the per-pixel lighting path, which applies the
// material colour after lighting; uploaded whether or not that path is on,
// because this sits behind a cache test and a conditional upload would leave
// the constant stale from whenever the setting changed until the material did.
void
setMaterial(const RGBA &color, const SurfaceProperties &surfaceprops, float extraSurfProp)
{
	if(!equal(d3dShaderState.matColor, color)){
		rw::RGBAf col;
		convColor(&col, &color);
		impl11::setVertexShaderConstantF(VSLOC_matColor, (float*)&col, 1);
		impl11::setPixelShaderConstantF(PSLOC_ppMatColor, (float*)&col, 1);
		d3dShaderState.matColor = color;
	}

	if(d3dShaderState.surfProps.ambient != surfaceprops.ambient ||
	   d3dShaderState.surfProps.specular != surfaceprops.specular ||
	   d3dShaderState.surfProps.diffuse != surfaceprops.diffuse ||
	   d3dShaderState.extraSurfProp != extraSurfProp){
		float surfProps[4];
		surfProps[0] = surfaceprops.ambient;
		surfProps[1] = surfaceprops.specular;
		surfProps[2] = surfaceprops.diffuse;
		surfProps[3] = extraSurfProp;
		impl11::setVertexShaderConstantF(VSLOC_surfProps, surfProps, 1);
		impl11::setPixelShaderConstantF(PSLOC_ppSurfProps, surfProps, 1);
		d3dShaderState.surfProps = surfaceprops;
		d3dShaderState.extraSurfProp = extraSurfProp;
	}
}

// --- state object caches ----------------------------------------------------

// Small and linear on purpose. A frame of BFBB uses a few dozen distinct
// pipeline states; a hash would cost more to maintain than the walk saves.
#define MAXCACHED 128

template<typename Desc, typename State>
struct StateCache
{
	struct Entry {
		Desc desc;
		State *state;
	} entries[MAXCACHED];
	int32 num;
	int32 next;

	State *find(const Desc *desc)
	{
		for(int32 i = 0; i < num; i++)
			if(memcmp(&entries[i].desc, desc, sizeof(Desc)) == 0)
				return entries[i].state;
		return nil;
	}

	void add(const Desc *desc, State *state)
	{
		int32 i;
		if(num < MAXCACHED)
			i = num++;
		else{
			// Round robin past the end, rather than hand the caller a state
			// nothing owns. The context holds its own reference to whatever
			// is bound, so releasing one here cannot pull a state out from
			// under a draw that is still using it.
			i = next;
			next = (next+1) % MAXCACHED;
			entries[i].state->Release();
		}
		entries[i].desc = *desc;
		entries[i].state = state;
	}

	void releaseAll(void)
	{
		for(int32 i = 0; i < num; i++)
			entries[i].state->Release();
		num = 0;
		next = 0;
	}
};

// What a stage with no raster samples. D3D9 binds a 1x1 white texture there so
// that a shader multiplying by it is unchanged; a null view would sample zero
// and turn the draw black.
static ID3D11Texture2D *whiteTex;
static ID3D11ShaderResourceView *whiteView;

void
createWhiteTexture(void)
{
	uint32 texel = 0xFFFFFFFF;
	D3D11_TEXTURE2D_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.Width = 1;
	desc.Height = 1;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_IMMUTABLE;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	D3D11_SUBRESOURCE_DATA init;
	memset(&init, 0, sizeof(init));
	init.pSysMem = &texel;
	init.SysMemPitch = 4;
	if(SUCCEEDED(d3d11device->CreateTexture2D(&desc, &init, &whiteTex)))
		d3d11device->CreateShaderResourceView(whiteTex, nil, &whiteView);
}

void
destroyWhiteTexture(void)
{
	if(whiteView){ whiteView->Release(); whiteView = nil; }
	if(whiteTex){ whiteTex->Release(); whiteTex = nil; }
}

static StateCache<D3D11_BLEND_DESC, ID3D11BlendState> blendCache;
static StateCache<D3D11_DEPTH_STENCIL_DESC, ID3D11DepthStencilState> depthCache;
static StateCache<D3D11_RASTERIZER_DESC, ID3D11RasterizerState> rasterCache;
static StateCache<D3D11_SAMPLER_DESC, ID3D11SamplerState> samplerCache;

void
releaseStateObjects(void)
{
	blendCache.releaseAll();
	depthCache.releaseAll();
	rasterCache.releaseAll();
	samplerCache.releaseAll();
}

// --- translation ------------------------------------------------------------

static D3D11_BLEND
blendFactor(uint32 rwblend)
{
	switch(rwblend){
	case BLENDZERO:			return D3D11_BLEND_ZERO;
	case BLENDONE:			return D3D11_BLEND_ONE;
	case BLENDSRCCOLOR:		return D3D11_BLEND_SRC_COLOR;
	case BLENDINVSRCCOLOR:		return D3D11_BLEND_INV_SRC_COLOR;
	case BLENDSRCALPHA:		return D3D11_BLEND_SRC_ALPHA;
	case BLENDINVSRCALPHA:		return D3D11_BLEND_INV_SRC_ALPHA;
	case BLENDDESTALPHA:		return D3D11_BLEND_DEST_ALPHA;
	case BLENDINVDESTALPHA:		return D3D11_BLEND_INV_DEST_ALPHA;
	case BLENDDESTCOLOR:		return D3D11_BLEND_DEST_COLOR;
	case BLENDINVDESTCOLOR:		return D3D11_BLEND_INV_DEST_COLOR;
	case BLENDSRCALPHASAT:		return D3D11_BLEND_SRC_ALPHA_SAT;
	}
	return D3D11_BLEND_ONE;
}

// The same factor, for the alpha channel.
//
// D3D11 keeps a separate blend for alpha and will not take a colour factor
// there: SrcBlendAlpha or DestBlendAlpha set to one of the _COLOR values makes
// CreateBlendState fail outright, and a draw with no blend state writes its
// source unblended. D3D9 has one pair for both and takes the alpha component of
// whatever colour factor was named, which is what these map to.
static D3D11_BLEND
blendFactorAlpha(uint32 rwblend)
{
	switch(rwblend){
	case BLENDSRCCOLOR:	return D3D11_BLEND_SRC_ALPHA;
	case BLENDINVSRCCOLOR:	return D3D11_BLEND_INV_SRC_ALPHA;
	case BLENDDESTCOLOR:	return D3D11_BLEND_DEST_ALPHA;
	case BLENDINVDESTCOLOR:	return D3D11_BLEND_INV_DEST_ALPHA;
	}
	return blendFactor(rwblend);
}

static D3D11_COMPARISON_FUNC
comparison(uint32 rwfunc)
{
	switch(rwfunc){
	case STENCILNEVER:		return D3D11_COMPARISON_NEVER;
	case STENCILLESS:		return D3D11_COMPARISON_LESS;
	case STENCILEQUAL:		return D3D11_COMPARISON_EQUAL;
	case STENCILLESSEQUAL:		return D3D11_COMPARISON_LESS_EQUAL;
	case STENCILGREATER:		return D3D11_COMPARISON_GREATER;
	case STENCILNOTEQUAL:		return D3D11_COMPARISON_NOT_EQUAL;
	case STENCILGREATEREQUAL:	return D3D11_COMPARISON_GREATER_EQUAL;
	case STENCILALWAYS:		return D3D11_COMPARISON_ALWAYS;
	}
	return D3D11_COMPARISON_ALWAYS;
}

static D3D11_STENCIL_OP
stencilOp(uint32 rwop)
{
	switch(rwop){
	case STENCILKEEP:	return D3D11_STENCIL_OP_KEEP;
	case STENCILZERO:	return D3D11_STENCIL_OP_ZERO;
	case STENCILREPLACE:	return D3D11_STENCIL_OP_REPLACE;
	case STENCILINCSAT:	return D3D11_STENCIL_OP_INCR_SAT;
	case STENCILDECSAT:	return D3D11_STENCIL_OP_DECR_SAT;
	case STENCILINVERT:	return D3D11_STENCIL_OP_INVERT;
	case STENCILINC:	return D3D11_STENCIL_OP_INCR;
	case STENCILDEC:	return D3D11_STENCIL_OP_DECR;
	}
	return D3D11_STENCIL_OP_KEEP;
}

static D3D11_TEXTURE_ADDRESS_MODE
addressMode(uint32 rwaddr)
{
	switch(rwaddr){
	case Texture::WRAP:	return D3D11_TEXTURE_ADDRESS_WRAP;
	case Texture::MIRROR:	return D3D11_TEXTURE_ADDRESS_MIRROR;
	case Texture::CLAMP:	return D3D11_TEXTURE_ADDRESS_CLAMP;
	case Texture::BORDER:	return D3D11_TEXTURE_ADDRESS_BORDER;
	}
	return D3D11_TEXTURE_ADDRESS_WRAP;
}

// Whether a filter mode looks at the mip chain at all. NEAREST and LINEAR do
// not: the D3D9 backend gives them D3DTEXF_NONE, which is the base level and
// nothing else, and a sampler that walks the chain instead makes a distant
// surface a different colour.
static bool
filterUsesMipmaps(uint32 rwfilter)
{
	return rwfilter != Texture::NEAREST && rwfilter != Texture::LINEAR;
}

static D3D11_FILTER
filterMode(uint32 rwfilter, uint32 maxAniso)
{
	if(maxAniso > 1)
		return D3D11_FILTER_ANISOTROPIC;
	switch(rwfilter){
	case Texture::NEAREST:
		return D3D11_FILTER_MIN_MAG_MIP_POINT;
	case Texture::LINEAR:
		return D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
	case Texture::MIPNEAREST:
		return D3D11_FILTER_MIN_MAG_MIP_POINT;
	case Texture::MIPLINEAR:
		return D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR;
	case Texture::LINEARMIPNEAREST:
		return D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
	case Texture::LINEARMIPLINEAR:
		return D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	}
	return D3D11_FILTER_MIN_MAG_MIP_LINEAR;
}

// --- building and binding ---------------------------------------------------

static void
bindBlendState(void)
{
	D3D11_BLEND_DESC desc;
	memset(&desc, 0, sizeof(desc));

	desc.AlphaToCoverageEnable = FALSE;
	desc.IndependentBlendEnable = FALSE;

	D3D11_RENDER_TARGET_BLEND_DESC *rt = &desc.RenderTarget[0];
	rt->BlendEnable = rwStateCache.blendenable != 0;
	rt->SrcBlend = blendFactor(rwStateCache.srcblend);
	rt->DestBlend = blendFactor(rwStateCache.destblend);
	rt->BlendOp = D3D11_BLEND_OP_ADD;
	rt->SrcBlendAlpha = blendFactorAlpha(rwStateCache.srcblend);
	rt->DestBlendAlpha = blendFactorAlpha(rwStateCache.destblend);
	rt->BlendOpAlpha = D3D11_BLEND_OP_ADD;
	// The librw mask bits are in the same order as D3D11's, so the value
	// passes straight through.
	rt->RenderTargetWriteMask = (UINT8)rwStateCache.colorwritemask;

	ID3D11BlendState *state = blendCache.find(&desc);
	if(state == nil){
		// Refused rather than bound: leaving the previous state in place would
		// make the next draw blend by whatever the last one asked for.
		if(FAILED(d3d11device->CreateBlendState(&desc, &state)))
			state = nil;
		else
			blendCache.add(&desc, state);
	}
	float blendFactorRGBA[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	d3d11context->OMSetBlendState(state, blendFactorRGBA, 0xFFFFFFFF);
}

static void
bindDepthStencilState(void)
{
	D3D11_DEPTH_STENCIL_DESC desc;
	memset(&desc, 0, sizeof(desc));

	// D3D11 gates the write on DepthEnable as well as the test, where D3D9
	// keeps them apart -- so a draw that writes depth without testing it needs
	// the stage on and the comparison always true. The D3D9 backend says the
	// same thing with ZENABLE plus ZFUNC.
	desc.DepthEnable = (rwStateCache.ztest || rwStateCache.zwrite) != 0;
	desc.DepthWriteMask = rwStateCache.zwrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
	desc.DepthFunc = rwStateCache.ztest ? D3D11_COMPARISON_LESS_EQUAL : D3D11_COMPARISON_ALWAYS;
	desc.StencilEnable = rwStateCache.stencilenable != 0;
	desc.StencilReadMask = (UINT8)rwStateCache.stencilmask;
	desc.StencilWriteMask = (UINT8)rwStateCache.stencilwritemask;

	D3D11_DEPTH_STENCILOP_DESC op;
	op.StencilFailOp = stencilOp(rwStateCache.stencilfail);
	op.StencilDepthFailOp = stencilOp(rwStateCache.stencilzfail);
	op.StencilPassOp = stencilOp(rwStateCache.stencilpass);
	op.StencilFunc = comparison(rwStateCache.stencilfunc);
	desc.FrontFace = op;
	desc.BackFace = op;

	ID3D11DepthStencilState *state = depthCache.find(&desc);
	if(state == nil){
		if(FAILED(d3d11device->CreateDepthStencilState(&desc, &state)))
			return;
		depthCache.add(&desc, state);
	}
	d3d11context->OMSetDepthStencilState(state, rwStateCache.stencilref);
}

static void
bindRasterizerState(void)
{
	D3D11_RASTERIZER_DESC desc;
	memset(&desc, 0, sizeof(desc));

	desc.FillMode = D3D11_FILL_SOLID;
	switch(rwStateCache.cullmode){
	case CULLNONE:	desc.CullMode = D3D11_CULL_NONE; break;
	case CULLBACK:	desc.CullMode = D3D11_CULL_BACK; break;
	case CULLFRONT:	desc.CullMode = D3D11_CULL_FRONT; break;
	default:	desc.CullMode = D3D11_CULL_NONE; break;
	}
	// RenderWare's front face is counter-clockwise: librw maps CULLBACK onto
	// D3DCULL_CW, which is D3D9 for "cull the clockwise ones", and GL3 pairs
	// GL_BACK with OpenGL's CCW default. D3D11 says the same thing the other
	// way round, as a property of the rasterizer rather than of the cull.
	desc.FrontCounterClockwise = TRUE;
	desc.DepthClipEnable = TRUE;
	desc.ScissorEnable = rwStateCache.scissorenable != 0;
	// Off for a 2D primitive, for the reason in bindBlendState.
	desc.MultisampleEnable = !im2DActive;

	ID3D11RasterizerState *state = rasterCache.find(&desc);
	if(state == nil){
		if(FAILED(d3d11device->CreateRasterizerState(&desc, &state)))
			return;
		rasterCache.add(&desc, state);
	}
	d3d11context->RSSetState(state);
}

static void
bindSamplers(void)
{
	ID3D11SamplerState *samplers[NUMTEXSTAGES];
	for(int i = 0; i < NUMTEXSTAGES; i++){
		D3D11_SAMPLER_DESC desc;
		memset(&desc, 0, sizeof(desc));
		desc.Filter = filterMode(rwStateCache.texstage[i].filter,
		                         rwStateCache.texstage[i].maxAnisotropy);
		desc.AddressU = addressMode(rwStateCache.texstage[i].addressingU);
		desc.AddressV = addressMode(rwStateCache.texstage[i].addressingV);
		desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		desc.MaxAnisotropy = rwStateCache.texstage[i].maxAnisotropy;
		desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
		desc.MinLOD = 0.0f;
		desc.MaxLOD = filterUsesMipmaps(rwStateCache.texstage[i].filter) ?
			D3D11_FLOAT32_MAX : 0.0f;

		ID3D11SamplerState *state = samplerCache.find(&desc);
		if(state == nil){
			if(FAILED(d3d11device->CreateSamplerState(&desc, &state)))
				state = nil;
			else
				samplerCache.add(&desc, state);
		}
		samplers[i] = state;
	}
	d3d11context->PSSetSamplers(0, NUMTEXSTAGES, samplers);
}

static void
bindTextures(void)
{
	ID3D11ShaderResourceView *views[NUMTEXSTAGES];
	for(int i = 0; i < NUMTEXSTAGES; i++){
		Raster *raster = rwStateCache.texstage[i].raster;
		views[i] = raster ? (ID3D11ShaderResourceView*)rasterShaderResource(raster) : whiteView;
		if(views[i] == nil)
			views[i] = whiteView;
	}
	d3d11context->PSSetShaderResources(0, NUMTEXSTAGES, views);
}

void
flushCache(void)
{
	if(d3dShaderState.fogDirty){
		impl11::setVertexShaderConstantF(VSLOC_fogData, (float*)&d3dShaderState.fogData, 1);
		impl11::setPixelShaderConstantF(PSLOC_fogColor, (float*)&d3dShaderState.fogColor, 1);
		d3dShaderState.fogDirty = false;
	}
	// A raster whose texels were rewritten while it stayed bound changes no
	// state, so nothing below would run and the GPU would keep the texels it
	// was given the first time. rasterShaderResource is what pushes them, and
	// bindTextures is the only thing that calls it.
	for(int i = 0; i < NUMTEXSTAGES && !stateDirty; i++){
		Raster *raster = rwStateCache.texstage[i].raster;
		if(raster && GETD3DRASTEREXT(raster)->dirty)
			stateDirty = 1;
	}

	uploadShaderConstants();
	if(!stateDirty)
		return;
	stateDirty = 0;
	bindBlendState();
	bindDepthStencilState();
	bindRasterizerState();
	bindSamplers();
	bindTextures();
}

// --- what the driver calls --------------------------------------------------

// What blends, what is tested, and where the test cuts. This is d3ddevice.cpp's
// updateAlphaStates; the reasoning is written out there and applies unchanged.
// What differs is only where the answers go: D3D11 has no alpha test state, so
// the comparison and the reference are uploaded and every pixel shader clips.
static void
updateAlphaStates(void)
{
	bool32 cutout = rwStateCache.textureAlpha && rwStateCache.textureKeyed &&
	                !rwStateCache.vertexAlpha && rwStateCache.zwrite &&
	                !im2DActive;
	bool32 test = rwStateCache.vertexAlpha || rwStateCache.textureAlpha;
	bool32 blend = rwStateCache.vertexAlpha ||
	               (rwStateCache.textureAlpha && !cutout);

	uint32 alpharef = cutout && rwStateCache.alpharef <= 1 ?
	                  ALPHACUTOUTREF : rwStateCache.alpharef;

	if(rwStateCache.blendenable != blend){
		rwStateCache.blendenable = blend;
		stateDirty = 1;
	}
	// ALPHAALWAYS is how the shader is told not to clip at all.
	setAlphaTestConstants(test ? rwStateCache.alphafunc : ALPHAALWAYS, alpharef);
}

// Blending follows alpha, the same rule the D3D9 backend applies: a draw blends
// when the application asked for it or when the geometry the pipeline instanced
// has alpha in it. Neither may erase the other, which is why they are separate.
static void
updateVertexAlpha(void)
{
	rwStateCache.vertexAlpha = rwStateCache.appVertexAlpha ||
	                           rwStateCache.pipelineVertexAlpha;
	updateAlphaStates();
}

bool32
getBlendEnabled(void)
{
	return rwStateCache.blendenable;
}

void
setPipelineVertexAlpha(bool32 enable)
{
	rwStateCache.pipelineVertexAlpha = enable;
	updateVertexAlpha();
}

// Bracket a 2D primitive. A screen-space quad's edges are placed in pixels
// rather than found by the rasterizer, so nothing about them wants
// multisampling: a letterbox bar ends where the game said it ends.
void
setIm2DActive(bool32 active)
{
	if(im2DActive != active){
		im2DActive = active;
		stateDirty = 1;
		updateAlphaStates();
	}
}

bool32
getIm2DActive(void)
{
	return im2DActive;
}

// A raster on its way out. The stage keeps a pointer, and comparing it against
// a reallocated one would say nothing changed.
void
forgetRaster(Raster *raster)
{
	for(int i = 0; i < NUMTEXSTAGES; i++)
		if(rwStateCache.texstage[i].raster == raster){
			rwStateCache.texstage[i].raster = nil;
			stateDirty = 1;
		}
}

void
setRasterStage(uint32 stage, Raster *raster)
{
	if(stage >= NUMTEXSTAGES)
		return;
	if(rwStateCache.texstage[stage].raster == raster)
		return;
	rwStateCache.texstage[stage].raster = raster;
	stateDirty = 1;

	if(stage != 0)
		return;
	bool32 alpha = 0, keyed = 0;
	if(raster){
		D3dRaster *natras = GETD3DRASTEREXT(raster);
		alpha = natras->alphaKind != ALPHAOPAQUE;
		keyed = natras->alphaKind == ALPHAKEYED;
	}
	if(rwStateCache.textureAlpha != alpha || rwStateCache.textureKeyed != keyed){
		rwStateCache.textureAlpha = alpha;
		rwStateCache.textureKeyed = keyed;
		updateAlphaStates();
	}
}

void
setTexture(uint32 stage, Texture *tex)
{
	if(stage >= NUMTEXSTAGES)
		return;
	if(tex == nil || tex->raster == nil){
		setRasterStage(stage, nil);
		return;
	}
	if(rwStateCache.texstage[stage].filter != (uint32)tex->getFilter() ||
	   rwStateCache.texstage[stage].addressingU != (uint32)tex->getAddressU() ||
	   rwStateCache.texstage[stage].addressingV != (uint32)tex->getAddressV() ||
	   rwStateCache.texstage[stage].maxAnisotropy != (uint32)tex->getMaxAnisotropy()){
		rwStateCache.texstage[stage].filter = tex->getFilter();
		rwStateCache.texstage[stage].addressingU = tex->getAddressU();
		rwStateCache.texstage[stage].addressingV = tex->getAddressV();
		rwStateCache.texstage[stage].maxAnisotropy = tex->getMaxAnisotropy();
		stateDirty = 1;
	}
	setRasterStage(stage, tex->raster);
}

void
setRwRenderState(int32 state, void *pvalue)
{
	uint32 value = (uint32)(uintptr)pvalue;
	uint32 bval = value ? 1 : 0;

	switch((RenderState)state){
	case TEXTURERASTER:
		setRasterStage(0, (Raster*)pvalue);
		break;
	case TEXTUREADDRESS:
		rwStateCache.texstage[0].addressingU = value;
		rwStateCache.texstage[0].addressingV = value;
		stateDirty = 1;
		break;
	case TEXTUREADDRESSU:
		rwStateCache.texstage[0].addressingU = value;
		stateDirty = 1;
		break;
	case TEXTUREADDRESSV:
		rwStateCache.texstage[0].addressingV = value;
		stateDirty = 1;
		break;
	case TEXTUREFILTER:
		rwStateCache.texstage[0].filter = value;
		stateDirty = 1;
		break;
	case VERTEXALPHA:
		rwStateCache.appVertexAlpha = bval;
		updateVertexAlpha();
		break;
	case SRCBLEND:
		rwStateCache.srcblend = value;
		stateDirty = 1;
		break;
	case DESTBLEND:
		rwStateCache.destblend = value;
		stateDirty = 1;
		break;
	case ZTESTENABLE:
		rwStateCache.ztest = bval;
		stateDirty = 1;
		break;
	case ZWRITEENABLE:
		rwStateCache.zwrite = bval;
		stateDirty = 1;
		// A cutout is a statement about the depth buffer, so whether this
		// draw writes one is part of the answer.
		updateAlphaStates();
		break;
	case FOGENABLE:
		rwStateCache.fogenable = bval;
		d3dShaderState.fogData.disable = bval ? 0.0f : 1.0f;
		d3dShaderState.fogDirty = true;
		break;
	case FOGCOLOR: {
		RGBA c;
		c.red = value;
		c.green = value>>8;
		c.blue = value>>16;
		c.alpha = value>>24;
		rwStateCache.fogcolor = c;
		convColor(&d3dShaderState.fogColor, &c);
		d3dShaderState.fogDirty = true;
		} break;
	case CULLMODE:
		rwStateCache.cullmode = value;
		stateDirty = 1;
		break;

	case STENCILENABLE:
		rwStateCache.stencilenable = bval;
		stateDirty = 1;
		break;
	case STENCILFAIL:
		rwStateCache.stencilfail = value;
		stateDirty = 1;
		break;
	case STENCILZFAIL:
		rwStateCache.stencilzfail = value;
		stateDirty = 1;
		break;
	case STENCILPASS:
		rwStateCache.stencilpass = value;
		stateDirty = 1;
		break;
	case STENCILFUNCTION:
		rwStateCache.stencilfunc = value;
		stateDirty = 1;
		break;
	case STENCILFUNCTIONREF:
		rwStateCache.stencilref = value;
		stateDirty = 1;
		break;
	case STENCILFUNCTIONMASK:
		rwStateCache.stencilmask = value;
		stateDirty = 1;
		break;
	case STENCILFUNCTIONWRITEMASK:
		rwStateCache.stencilwritemask = value;
		stateDirty = 1;
		break;

	// No alpha test render state exists in D3D11. These go to the pixel
	// shader, which clips, so they are uploaded rather than bound.
	case ALPHATESTFUNC:
		rwStateCache.alphafunc = value;
		updateAlphaStates();
		break;
	case ALPHATESTREF:
		rwStateCache.alpharef = value;
		updateAlphaStates();
		break;
	case GSALPHATEST:
		rwStateCache.gsalpha = value;
		break;
	case GSALPHATESTREF:
		rwStateCache.gsalpharef = value;
		break;
	case COLORWRITEMASK:
		rwStateCache.colorwritemask = value;
		stateDirty = 1;
		break;
	}
}

void*
getRwRenderState(int32 state)
{
	uint32 val = 0;
	switch((RenderState)state){
	case TEXTURERASTER:
		return rwStateCache.texstage[0].raster;
	case TEXTUREADDRESS:
		val = rwStateCache.texstage[0].addressingU == rwStateCache.texstage[0].addressingV ?
			rwStateCache.texstage[0].addressingU : 0;
		break;
	case TEXTUREADDRESSU:	val = rwStateCache.texstage[0].addressingU; break;
	case TEXTUREADDRESSV:	val = rwStateCache.texstage[0].addressingV; break;
	case TEXTUREFILTER:	val = rwStateCache.texstage[0].filter; break;
	case VERTEXALPHA:	val = rwStateCache.appVertexAlpha; break;
	case SRCBLEND:		val = rwStateCache.srcblend; break;
	case DESTBLEND:		val = rwStateCache.destblend; break;
	case ZTESTENABLE:	val = rwStateCache.ztest; break;
	case ZWRITEENABLE:	val = rwStateCache.zwrite; break;
	case FOGENABLE:		val = rwStateCache.fogenable; break;
	case FOGCOLOR:
		val = (uint32)rwStateCache.fogcolor.red |
		      (uint32)rwStateCache.fogcolor.green<<8 |
		      (uint32)rwStateCache.fogcolor.blue<<16 |
		      (uint32)rwStateCache.fogcolor.alpha<<24;
		break;
	case CULLMODE:		val = rwStateCache.cullmode; break;
	case STENCILENABLE:	val = rwStateCache.stencilenable; break;
	case STENCILFAIL:	val = rwStateCache.stencilfail; break;
	case STENCILZFAIL:	val = rwStateCache.stencilzfail; break;
	case STENCILPASS:	val = rwStateCache.stencilpass; break;
	case STENCILFUNCTION:	val = rwStateCache.stencilfunc; break;
	case STENCILFUNCTIONREF:	val = rwStateCache.stencilref; break;
	case STENCILFUNCTIONMASK:	val = rwStateCache.stencilmask; break;
	case STENCILFUNCTIONWRITEMASK:	val = rwStateCache.stencilwritemask; break;
	case ALPHATESTFUNC:	val = rwStateCache.alphafunc; break;
	case ALPHATESTREF:	val = rwStateCache.alpharef; break;
	case GSALPHATEST:	val = rwStateCache.gsalpha; break;
	case GSALPHATESTREF:	val = rwStateCache.gsalpharef; break;
	case COLORWRITEMASK:	val = rwStateCache.colorwritemask; break;
	}
	return (void*)(uintptr)val;
}

// Say that nothing the context holds can be trusted any more, without touching
// what the render states SAY. The two are different questions: the shadow is
// the application's answer and survives, the bindings are the device's and do
// not -- so a pass that sets its own state behind the cache's back ends by
// calling this rather than resetRenderState, which would hand the game back its
// defaults every frame.
void
invalidateDeviceState(void)
{
	stateDirty = 1;
}

// Let go of the textures, because what is about to become a render target may
// be one of them. A resource cannot be an output and an input at once: the
// runtime answers that by silently forcing the shader resource to null, and a
// draw that samples the slot afterwards reads black rather than the texture the
// game thinks is there. Marking the state dirty is what puts them back.
void
unbindTextures(void)
{
	ID3D11ShaderResourceView *none[NUMTEXSTAGES] = { nil };
	d3d11context->PSSetShaderResources(0, NUMTEXSTAGES, none);
	stateDirty = 1;
}

// The state the device comes up in, which is also what the render states are
// documented to start at.
void
resetRenderState(void)
{
	memset(&rwStateCache, 0, sizeof(rwStateCache));
	rwStateCache.srcblend = BLENDSRCALPHA;
	rwStateCache.destblend = BLENDINVSRCALPHA;
	rwStateCache.colorwritemask = 0xF;
	rwStateCache.ztest = 1;
	rwStateCache.zwrite = 1;
	rwStateCache.cullmode = CULLBACK;
	rwStateCache.stencilfunc = STENCILALWAYS;
	rwStateCache.stencilmask = 0xFF;
	rwStateCache.stencilwritemask = 0xFF;
	rwStateCache.alphafunc = ALPHAGREATEREQUAL;
	rwStateCache.alpharef = 10;
	for(int i = 0; i < NUMTEXSTAGES; i++){
		rwStateCache.texstage[i].addressingU = Texture::WRAP;
		rwStateCache.texstage[i].addressingV = Texture::WRAP;
		rwStateCache.texstage[i].filter = Texture::LINEAR;
		rwStateCache.texstage[i].maxAnisotropy = 1;
	}
	im2DActive = 0;
	stateDirty = 1;
	updateAlphaStates();
}

// The D3D9 interface the pipelines still call. D3DRS_ keys have no counterpart
// in D3D11, and every one that matters already has a librw render state above,
// so these exist to keep shared code linking and do nothing.
void setRenderState(uint32 state, uint32 value) { (void)state; (void)value; }
void getRenderState(uint32 state, uint32 *value) { (void)state; *value = 0; }
void setTextureStageState(uint32 stage, uint32 type, uint32 value) { (void)stage; (void)type; (void)value; }
void getTextureStageState(uint32 stage, uint32 type, uint32 *value) { (void)stage; (void)type; *value = 0; }
void setSamplerState(uint32 stage, uint32 type, uint32 value) { (void)stage; (void)type; (void)value; }
void getSamplerState(uint32 stage, uint32 type, uint32 *value) { (void)stage; (void)type; *value = 0; }

#endif
}
}
}
