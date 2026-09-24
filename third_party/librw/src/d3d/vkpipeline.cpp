#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stddef.h>

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
#include "rwd3dimpl.h"
#ifdef RW_VULKAN
#include "vkincludes.h"
#endif
#include "rwd3dvk.h"

#define PLUGIN_ID 0

namespace rw {
namespace d3d {

// See d3d11device.cpp: a call in here that passes one of rw::d3d's enumerators
// has to name this namespace, or argument-dependent lookup finds the forwarder
// beside the function meant.
namespace implvk {

#ifdef RW_VULKAN

// The device's state, and how a D3D9-shaped driver reaches it.
//
// The whole driver sets one D3D9 render state at a time. Vulkan wants all of a
// draw's fixed state baked into a pipeline, so this file keeps a shadow of the
// states, as d3d11state.cpp does, and settles it into a pipeline when a draw
// comes. What Vulkan 1.3 lets a command buffer set per draw -- depth, stencil,
// culling -- is set that way instead, which keeps the pipeline count to one per
// shader pair, vertex layout, blend and target.

#define NUMTEXSTAGES 4

// --- shader constants -------------------------------------------------------

// D3D9's constant register file, as D3D11 keeps it: every VSLOC_/PSLOC_ is a
// c-register number, and dxc was told to put $Globals where fxc does, so a
// register is 16 bytes into the buffer. See d3d11shader.cpp for the sizes.
#define NUMVSCONST 256
#define NUMPSCONST 64
#define NUMVSINT 4
#define PSLOC_alphaTest 7

static float vsConstants[NUMVSCONST*4];
static float psConstants[NUMPSCONST*4];
static int32 vsIntConstants[NUMVSINT*4];
static bool32 constantsDirty = 1;

void
setVertexShaderConstantF(uint32 reg, const float32 *data, int32 numRegs)
{
	if(reg + numRegs > NUMVSCONST)
		return;
	memcpy(vsConstants + reg*4, data, numRegs*4*sizeof(float));
	constantsDirty = 1;
}

void
setVertexShaderConstantI(uint32 reg, const int32 *data, int32 numRegs)
{
	if(reg + numRegs > NUMVSINT)
		return;
	memcpy(vsIntConstants + reg*4, data, numRegs*4*sizeof(int32));
	constantsDirty = 1;
}

void
setPixelShaderConstantF(uint32 reg, const float32 *data, int32 numRegs)
{
	if(reg + numRegs > NUMPSCONST)
		return;
	memcpy(psConstants + reg*4, data, numRegs*4*sizeof(float));
	constantsDirty = 1;
}

void
setAlphaTestConstants(uint32 func, uint32 ref)
{
	float c[4];
	c[0] = (float)func;
	c[1] = ref/255.0f;
	c[2] = 0.0f;
	c[3] = 0.0f;
	implvk::setPixelShaderConstantF(PSLOC_alphaTest, c, 1);
}

// --- shaders ----------------------------------------------------------------

// The blobs spirv_h.py writes: a word count, then the module. It has already
// put every stage input at the location its semantic names, so a vertex shader
// is described by which locations it reads and what type each one is.
enum { INPUT_FLOAT, INPUT_SINT, INPUT_UINT };

struct VertexShader
{
	VkShaderModule module;
	uint32 inputMask;
	uint8 inputType[16];
	uint8 inputComponents[16];
};

struct PixelShader
{
	VkShaderModule module;
};

static VkShaderModule
createModule(const uint32 *blob)
{
	VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	ci.codeSize = blob[0]*4;
	ci.pCode = blob+1;
	VkShaderModule module = VK_NULL_HANDLE;
	vkCreateShaderModule(vkGlobals.device, &ci, nil, &module);
	return module;
}

// A vertex shader's stage inputs, read from the module: the variables in the
// Input storage class with a Location, and the scalar type under each.
static void
readInputs(const uint32 *code, uint32 numWords, VertexShader *vs)
{
	enum { OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypePointer = 32,
	       OpVariable = 59, OpDecorate = 71, DecLocation = 30, StorageInput = 1 };
	struct TypeInfo { uint8 kind; uint8 components; uint32 pointee; };
	uint32 bound = code[3];
	TypeInfo *types = rwNewT(TypeInfo, bound, MEMDUR_FUNCTION | ID_DRIVER);
	int32 *location = rwNewT(int32, bound, MEMDUR_FUNCTION | ID_DRIVER);
	memset(types, 0, bound*sizeof(TypeInfo));
	for(uint32 i = 0; i < bound; i++)
		location[i] = -1;

	for(uint32 pass = 0; pass < 2; pass++)
	for(uint32 i = 5; i < numWords; ){
		uint32 op = code[i] & 0xFFFF;
		uint32 count = code[i] >> 16;
		if(count == 0)
			break;
		const uint32 *w = code + i + 1;
		if(pass == 0){
			if(op == OpTypeInt && w[0] < bound){
				types[w[0]].kind = w[2] ? INPUT_SINT : INPUT_UINT;
				types[w[0]].components = 1;
			}else if(op == OpTypeFloat && w[0] < bound){
				types[w[0]].kind = INPUT_FLOAT;
				types[w[0]].components = 1;
			}else if(op == OpTypeVector && w[0] < bound && w[1] < bound){
				types[w[0]].kind = types[w[1]].kind;
				types[w[0]].components = (uint8)w[2];
			}else if(op == OpTypePointer && w[0] < bound){
				types[w[0]].pointee = w[2];
			}else if(op == OpDecorate && w[1] == DecLocation && w[0] < bound){
				location[w[0]] = (int32)w[2];
			}
		}else if(op == OpVariable && w[2] == StorageInput && w[1] < bound){
			int32 loc = location[w[1]];
			if(loc >= 0 && loc < 16 && w[0] < bound){
				TypeInfo *t = &types[types[w[0]].pointee % bound];
				vs->inputMask |= 1u << loc;
				vs->inputType[loc] = t->kind;
				vs->inputComponents[loc] = t->components;
			}
		}
		i += count;
	}
	rwFree(types);
	rwFree(location);
}

void*
createVertexShader(void *csosrc)
{
	const uint32 *blob = (const uint32*)csosrc;
	if(blob == nil)
		return nil;
	VertexShader *vs = rwNewT(VertexShader, 1, MEMDUR_EVENT | ID_DRIVER);
	memset(vs, 0, sizeof(*vs));
	vs->module = createModule(blob);
	if(vs->module == VK_NULL_HANDLE){
		rwFree(vs);
		return nil;
	}
	readInputs(blob+1, blob[0], vs);
	vkGlobals.numVertexShaders++;
	return vs;
}

void*
createPixelShader(void *csosrc)
{
	const uint32 *blob = (const uint32*)csosrc;
	if(blob == nil)
		return nil;
	PixelShader *ps = rwNewT(PixelShader, 1, MEMDUR_EVENT | ID_DRIVER);
	ps->module = createModule(blob);
	if(ps->module == VK_NULL_HANDLE){
		rwFree(ps);
		return nil;
	}
	vkGlobals.numPixelShaders++;
	return ps;
}

static void purgePipelines(void *shader);

void
destroyVertexShader(void *shader)
{
	VertexShader *vs = (VertexShader*)shader;
	if(vs == nil)
		return;
	purgePipelines(vs);
	deferDestroyShaderModule(vs->module);
	rwFree(vs);
	vkGlobals.numVertexShaders--;
}

void
destroyPixelShader(void *shader)
{
	PixelShader *ps = (PixelShader*)shader;
	if(ps == nil)
		return;
	purgePipelines(ps);
	deferDestroyShaderModule(ps->module);
	rwFree(ps);
	vkGlobals.numPixelShaders--;
}

// --- the shadow states ------------------------------------------------------

static struct {
	uint32 srcblend, destblend;
	bool32 blendenable;
	uint32 colorwritemask;

	bool32 ztest, zwrite;
	bool32 stencilenable;
	uint32 stencilfail, stencilzfail, stencilpass, stencilfunc;
	uint32 stencilref, stencilmask, stencilwritemask;

	uint32 cullmode;

	struct {
		Raster *raster;
		uint32 addressingU, addressingV;
		uint32 filter;
		uint32 maxAnisotropy;
	} texstage[NUMTEXSTAGES];

	bool32 appVertexAlpha;
	bool32 pipelineVertexAlpha;
	bool32 vertexAlpha;
	bool32 textureAlpha;
	bool32 textureKeyed;

	uint32 alphafunc;
	uint32 alpharef;
	uint32 gsalpha;
	uint32 gsalpharef;

	bool32 fogenable;
	RGBA fogcolor;
} rwStateCache;

static bool32 im2DActive;

// What the pipelines have set to draw with. Kept apart from what the command
// buffer has bound, which is below and forgotten with every new one.
static struct {
	VertexShader *vertexShader;
	PixelShader *pixelShader;
	void *declaration;
	void *indexBuffer;
	struct {
		void *buffer;
		uint32 offset;
		uint32 stride;
	} streams[3];
} bound;

void
setMaterial(const RGBA &color, const SurfaceProperties &surfaceprops, float extraSurfProp)
{
	if(!equal(d3dShaderState.matColor, color)){
		rw::RGBAf col;
		convColor(&col, &color);
		implvk::setVertexShaderConstantF(VSLOC_matColor, (float*)&col, 1);
		implvk::setPixelShaderConstantF(PSLOC_ppMatColor, (float*)&col, 1);
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
		implvk::setVertexShaderConstantF(VSLOC_surfProps, surfProps, 1);
		implvk::setPixelShaderConstantF(PSLOC_ppSurfProps, surfProps, 1);
		d3dShaderState.surfProps = surfaceprops;
		d3dShaderState.extraSurfProp = extraSurfProp;
	}
}

// d3ddevice.cpp's updateAlphaStates, answered the way d3d11state.cpp answers it:
// no alpha test state, so the comparison goes to the pixel shader.
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
	rwStateCache.blendenable = blend;
	setAlphaTestConstants(test ? rwStateCache.alphafunc : ALPHAALWAYS, alpharef);
}

static void
updateVertexAlpha(void)
{
	rwStateCache.vertexAlpha = rwStateCache.appVertexAlpha ||
	                           rwStateCache.pipelineVertexAlpha;
	updateAlphaStates();
}

bool32 getBlendEnabled(void) { return rwStateCache.blendenable; }

void
setPipelineVertexAlpha(bool32 enable)
{
	rwStateCache.pipelineVertexAlpha = enable;
	updateVertexAlpha();
}

void
setIm2DActive(bool32 active)
{
	if(im2DActive != active){
		im2DActive = active;
		updateAlphaStates();
	}
}

void
forgetRaster(Raster *raster)
{
	for(int i = 0; i < NUMTEXSTAGES; i++)
		if(rwStateCache.texstage[i].raster == raster)
			rwStateCache.texstage[i].raster = nil;
}

void
setRasterStage(uint32 stage, Raster *raster)
{
	if(stage >= NUMTEXSTAGES || rwStateCache.texstage[stage].raster == raster)
		return;
	rwStateCache.texstage[stage].raster = raster;
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
	rwStateCache.texstage[stage].filter = tex->getFilter();
	rwStateCache.texstage[stage].addressingU = tex->getAddressU();
	rwStateCache.texstage[stage].addressingV = tex->getAddressV();
	rwStateCache.texstage[stage].maxAnisotropy = tex->getMaxAnisotropy();
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
		break;
	case TEXTUREADDRESSU:
		rwStateCache.texstage[0].addressingU = value;
		break;
	case TEXTUREADDRESSV:
		rwStateCache.texstage[0].addressingV = value;
		break;
	case TEXTUREFILTER:
		rwStateCache.texstage[0].filter = value;
		break;
	case VERTEXALPHA:
		rwStateCache.appVertexAlpha = bval;
		updateVertexAlpha();
		break;
	case SRCBLEND:
		rwStateCache.srcblend = value;
		break;
	case DESTBLEND:
		rwStateCache.destblend = value;
		break;
	case ZTESTENABLE:
		rwStateCache.ztest = bval;
		break;
	case ZWRITEENABLE:
		rwStateCache.zwrite = bval;
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
		break;
	case STENCILENABLE:
		rwStateCache.stencilenable = bval;
		break;
	case STENCILFAIL:
		rwStateCache.stencilfail = value;
		break;
	case STENCILZFAIL:
		rwStateCache.stencilzfail = value;
		break;
	case STENCILPASS:
		rwStateCache.stencilpass = value;
		break;
	case STENCILFUNCTION:
		rwStateCache.stencilfunc = value;
		break;
	case STENCILFUNCTIONREF:
		rwStateCache.stencilref = value;
		break;
	case STENCILFUNCTIONMASK:
		rwStateCache.stencilmask = value;
		break;
	case STENCILFUNCTIONWRITEMASK:
		rwStateCache.stencilwritemask = value;
		break;
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
	updateAlphaStates();
}

// D3DRS_ keys have no counterpart here, and every one that matters has a librw
// render state above. See d3d11state.cpp.
void setRenderState(uint32 state, uint32 value) { (void)state; (void)value; }
void getRenderState(uint32 state, uint32 *value) { (void)state; *value = 0; }
void setTextureStageState(uint32 stage, uint32 type, uint32 value) { (void)stage; (void)type; (void)value; }
void getTextureStageState(uint32 stage, uint32 type, uint32 *value) { (void)stage; (void)type; *value = 0; }
void setSamplerState(uint32 stage, uint32 type, uint32 value) { (void)stage; (void)type; (void)value; }
void getSamplerState(uint32 stage, uint32 type, uint32 *value) { (void)stage; (void)type; *value = 0; }

void
flushCache(void)
{
	if(d3dShaderState.fogDirty){
		implvk::setVertexShaderConstantF(VSLOC_fogData, (float*)&d3dShaderState.fogData, 1);
		implvk::setPixelShaderConstantF(PSLOC_fogColor, (float*)&d3dShaderState.fogColor, 1);
		d3dShaderState.fogDirty = false;
	}
}

// --- what the pipelines bind ------------------------------------------------

void
setVertexShader(void *vs)
{
	bound.vertexShader = (VertexShader*)vs;
}

void
setPixelShader(void *ps)
{
	bound.pixelShader = (PixelShader*)ps;
}

void
setVertexDeclaration(void *declaration)
{
	bound.declaration = declaration;
}

void
forgetVertexDeclaration(void *declaration)
{
	if(bound.declaration == declaration)
		bound.declaration = nil;
}

void
setIndices(void *indexBuffer)
{
	bound.indexBuffer = indexBuffer;
}

void *boundIndexBuffer(void) { return bound.indexBuffer; }

void
setStreamSource(int n, void *buffer, uint32 offset, uint32 stride)
{
	if(n < 0 || n > 2)
		return;
	bound.streams[n].buffer = buffer;
	bound.streams[n].offset = offset;
	bound.streams[n].stride = stride;
}

void
forgetBuffer(void *buffer)
{
	if(bound.indexBuffer == buffer)
		bound.indexBuffer = nil;
	for(int i = 0; i < 3; i++)
		if(bound.streams[i].buffer == buffer)
			bound.streams[i].buffer = nil;
}

// --- samplers ---------------------------------------------------------------

struct SamplerKey
{
	uint32 filter;
	uint32 addressU, addressV;
	uint32 maxAnisotropy;
};

#define MAXSAMPLERS 64
static struct {
	SamplerKey key;
	VkSampler sampler;
} samplers[MAXSAMPLERS];
static int32 numSamplers;
static VkSampler blitSampler;

static VkSamplerAddressMode
addressMode(uint32 rwaddr)
{
	switch(rwaddr){
	case Texture::MIRROR:	return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
	case Texture::CLAMP:	return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	case Texture::BORDER:	return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	}
	return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

static VkSampler
getSampler(const SamplerKey *key)
{
	for(int32 i = 0; i < numSamplers; i++)
		if(memcmp(&samplers[i].key, key, sizeof(SamplerKey)) == 0)
			return samplers[i].sampler;

	VkSamplerCreateInfo ci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	bool32 linear = key->filter == Texture::LINEAR || key->filter == Texture::LINEARMIPNEAREST ||
	                key->filter == Texture::LINEARMIPLINEAR;
	ci.magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
	ci.minFilter = ci.magFilter;
	ci.mipmapMode = key->filter == Texture::MIPLINEAR || key->filter == Texture::LINEARMIPLINEAR ?
		VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
	ci.addressModeU = addressMode(key->addressU);
	ci.addressModeV = addressMode(key->addressV);
	ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	// NEAREST and LINEAR read the base level and nothing else; see
	// d3d11state.cpp's filterUsesMipmaps.
	ci.maxLod = key->filter == Texture::NEAREST || key->filter == Texture::LINEAR ? 0.0f : VK_LOD_CLAMP_NONE;
	if(key->maxAnisotropy > 1 && vkGlobals.maxAnisotropy > 1.0f){
		ci.magFilter = ci.minFilter = VK_FILTER_LINEAR;
		ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		ci.anisotropyEnable = VK_TRUE;
		ci.maxAnisotropy = key->maxAnisotropy < vkGlobals.maxAnisotropy ?
			(float)key->maxAnisotropy : vkGlobals.maxAnisotropy;
	}
	ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;

	VkSampler sampler = VK_NULL_HANDLE;
	vkCreateSampler(vkGlobals.device, &ci, nil, &sampler);
	// Past the end the last slot is recycled. The handful a game uses fits many
	// times over, and a sampler still named by a descriptor set this frame
	// cannot be destroyed, so it is leaked to the device instead.
	int32 i = numSamplers < MAXSAMPLERS ? numSamplers++ : MAXSAMPLERS-1;
	samplers[i].key = *key;
	samplers[i].sampler = sampler;
	return sampler;
}

// --- layouts and descriptor sets --------------------------------------------

// One set layout for every shader, matching the bindings make_shaders.cmd asks
// dxc for: the three constant blocks as dynamic uniform buffers, then four
// textures and their four samplers.
enum {
	BINDING_VSGLOBALS = 0,
	BINDING_VSINTS = 1,
	BINDING_PSGLOBALS = 2,
	BINDING_TEXTURE0 = 3,
	BINDING_SAMPLER0 = 7,
	NUMBINDINGS = 11
};

static VkDescriptorSetLayout setLayout;
static VkPipelineLayout pipelineLayout;

// Sets are allocated per frame from its slot's pools, which are all reset when
// the next frame in that slot opens, so nothing tracks which set is still in
// use.
#define SETSPERPOOL 1024
struct PoolSet
{
	VkDescriptorPool *pools;
	int32 numPools;
	int32 maxPools;
	int32 currentPool;
	uint32 serial;
};
static PoolSet poolSets[FRAMESINFLIGHT];

static VkDescriptorPool
newPool(void)
{
	VkDescriptorPoolSize sizes[3];
	sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	sizes[0].descriptorCount = 3*SETSPERPOOL;
	sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	sizes[1].descriptorCount = NUMTEXSTAGES*SETSPERPOOL;
	sizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLER;
	sizes[2].descriptorCount = NUMTEXSTAGES*SETSPERPOOL;
	VkDescriptorPoolCreateInfo ci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	ci.maxSets = SETSPERPOOL;
	ci.poolSizeCount = 3;
	ci.pPoolSizes = sizes;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	vkCreateDescriptorPool(vkGlobals.device, &ci, nil, &pool);
	return pool;
}

static VkDescriptorSet
allocateSet(void)
{
	// The slot is the open frame's; a set is only ever wanted while recording.
	frameCommands();
	PoolSet *ps = &poolSets[frameSlot()];
	if(ps->serial != frameSerial()){
		for(int32 i = 0; i < ps->numPools; i++)
			vkResetDescriptorPool(vkGlobals.device, ps->pools[i], 0);
		ps->currentPool = 0;
		ps->serial = frameSerial();
	}
	for(;;){
		if(ps->currentPool == ps->numPools){
			if(ps->numPools == ps->maxPools){
				int32 n = ps->maxPools ? ps->maxPools*2 : 4;
				VkDescriptorPool *p = rwNewT(VkDescriptorPool, n, MEMDUR_EVENT | ID_DRIVER);
				if(ps->pools){
					memcpy(p, ps->pools, ps->numPools*sizeof(VkDescriptorPool));
					rwFree(ps->pools);
				}
				ps->pools = p;
				ps->maxPools = n;
			}
			ps->pools[ps->numPools] = newPool();
			if(ps->pools[ps->numPools] == VK_NULL_HANDLE)
				return VK_NULL_HANDLE;
			ps->numPools++;
		}
		VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		ai.descriptorPool = ps->pools[ps->currentPool];
		ai.descriptorSetCount = 1;
		ai.pSetLayouts = &setLayout;
		VkDescriptorSet set = VK_NULL_HANDLE;
		if(vkAllocateDescriptorSets(vkGlobals.device, &ai, &set) == VK_SUCCESS)
			return set;
		ps->currentPool++;
	}
}

struct SetKey
{
	VkImageView views[NUMTEXSTAGES];
	VkSampler samplers[NUMTEXSTAGES];
	VkBuffer constants;
};

static VkDescriptorSet
getSet(const SetKey *key)
{
	VkDescriptorSet set = allocateSet();
	if(set == VK_NULL_HANDLE)
		return set;

	VkDescriptorBufferInfo bufs[3];
	bufs[0].buffer = key->constants;
	bufs[0].offset = 0;
	bufs[0].range = sizeof(vsConstants);
	bufs[1].buffer = key->constants;
	bufs[1].offset = 0;
	bufs[1].range = sizeof(vsIntConstants);
	bufs[2].buffer = key->constants;
	bufs[2].offset = 0;
	bufs[2].range = sizeof(psConstants);
	VkDescriptorImageInfo images[NUMTEXSTAGES], samps[NUMTEXSTAGES];
	for(int i = 0; i < NUMTEXSTAGES; i++){
		images[i].sampler = VK_NULL_HANDLE;
		images[i].imageView = key->views[i];
		images[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		samps[i].sampler = key->samplers[i];
		samps[i].imageView = VK_NULL_HANDLE;
		samps[i].imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	}

	VkWriteDescriptorSet w[3+2*NUMTEXSTAGES];
	int n = 0;
	for(int i = 0; i < 3; i++, n++){
		memset(&w[n], 0, sizeof(w[n]));
		w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w[n].dstSet = set;
		w[n].dstBinding = i;
		w[n].descriptorCount = 1;
		w[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		w[n].pBufferInfo = &bufs[i];
	}
	for(int i = 0; i < NUMTEXSTAGES; i++, n += 2){
		memset(&w[n], 0, 2*sizeof(w[n]));
		w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w[n].dstSet = set;
		w[n].dstBinding = BINDING_TEXTURE0 + i;
		w[n].descriptorCount = 1;
		w[n].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		w[n].pImageInfo = &images[i];
		w[n+1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w[n+1].dstSet = set;
		w[n+1].dstBinding = BINDING_SAMPLER0 + i;
		w[n+1].descriptorCount = 1;
		w[n+1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
		w[n+1].pImageInfo = &samps[i];
	}
	vkUpdateDescriptorSets(vkGlobals.device, n, w, 0, nil);
	return set;
}

// --- pipelines --------------------------------------------------------------

struct PipelineKey
{
	void *vertexShader;
	void *pixelShader;
	uint32 topology;
	uint32 colorFormat;
	uint32 depthFormat;
	uint32 samples;
	uint32 blendEnable;
	uint32 srcBlend;
	uint32 destBlend;
	uint32 writeMask;
	uint32 numAttributes;
	uint32 numBindings;
	VkVertexInputAttributeDescription attributes[16];
	VkVertexInputBindingDescription bindings[3];
};

struct PipelineEntry
{
	PipelineKey key;
	uint32 hash;
	VkPipeline pipeline;
};

static PipelineEntry *pipelines;
static int32 numPipelines;
static int32 maxPipelines;

static uint32
hashKey(const PipelineKey *key)
{
	const uint8 *p = (const uint8*)key;
	uint32 h = 2166136261u;
	for(size_t i = 0; i < sizeof(PipelineKey); i++)
		h = (h ^ p[i]) * 16777619u;
	return h;
}

static VkBlendFactor
blendFactor(uint32 rwblend)
{
	switch(rwblend){
	case BLENDZERO:			return VK_BLEND_FACTOR_ZERO;
	case BLENDONE:			return VK_BLEND_FACTOR_ONE;
	case BLENDSRCCOLOR:		return VK_BLEND_FACTOR_SRC_COLOR;
	case BLENDINVSRCCOLOR:		return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
	case BLENDSRCALPHA:		return VK_BLEND_FACTOR_SRC_ALPHA;
	case BLENDINVSRCALPHA:		return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	case BLENDDESTALPHA:		return VK_BLEND_FACTOR_DST_ALPHA;
	case BLENDINVDESTALPHA:		return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
	case BLENDDESTCOLOR:		return VK_BLEND_FACTOR_DST_COLOR;
	case BLENDINVDESTCOLOR:		return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
	case BLENDSRCALPHASAT:		return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
	}
	return VK_BLEND_FACTOR_ONE;
}

// The dynamic states every pipeline here declares, the blit's included, so that
// binding one never leaves another's state undefined.
static const VkDynamicState dynamicStates[] = {
	VK_DYNAMIC_STATE_VIEWPORT,
	VK_DYNAMIC_STATE_SCISSOR,
	VK_DYNAMIC_STATE_CULL_MODE,
	VK_DYNAMIC_STATE_FRONT_FACE,
	VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
	VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
	VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
	VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
	VK_DYNAMIC_STATE_STENCIL_OP,
	VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
	VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
	VK_DYNAMIC_STATE_STENCIL_REFERENCE,
};

static bool32
formatHasStencil(VkFormat fmt)
{
	return fmt == VK_FORMAT_D16_UNORM_S8_UINT || fmt == VK_FORMAT_D24_UNORM_S8_UINT ||
	       fmt == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

static VkPipeline
createPipeline(const PipelineKey *key, VkShaderModule vs, VkShaderModule ps)
{
	VkPipelineShaderStageCreateInfo stages[2];
	memset(stages, 0, sizeof(stages));
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vs;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = ps;
	stages[1].pName = "main";

	VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	vi.vertexBindingDescriptionCount = key->numBindings;
	vi.pVertexBindingDescriptions = key->bindings;
	vi.vertexAttributeDescriptionCount = key->numAttributes;
	vi.pVertexAttributeDescriptions = key->attributes;

	VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	ia.topology = (VkPrimitiveTopology)key->topology;

	VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	vp.viewportCount = 1;
	vp.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	ms.rasterizationSamples = (VkSampleCountFlagBits)key->samples;

	VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };

	VkPipelineColorBlendAttachmentState att;
	memset(&att, 0, sizeof(att));
	att.blendEnable = key->blendEnable ? VK_TRUE : VK_FALSE;
	att.srcColorBlendFactor = blendFactor(key->srcBlend);
	att.dstColorBlendFactor = blendFactor(key->destBlend);
	att.colorBlendOp = VK_BLEND_OP_ADD;
	// D3D9 has one factor pair and takes the alpha component of a colour
	// factor for the alpha channel. Vulkan does the same with the same factor,
	// so unlike D3D11 the pair needs no translating.
	att.srcAlphaBlendFactor = att.srcColorBlendFactor;
	att.dstAlphaBlendFactor = att.dstColorBlendFactor;
	att.alphaBlendOp = VK_BLEND_OP_ADD;
	// librw's COLORWRITE bits are in Vulkan's order.
	att.colorWriteMask = key->writeMask;
	VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	cb.attachmentCount = 1;
	cb.pAttachments = &att;

	VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	dy.dynamicStateCount = sizeof(dynamicStates)/sizeof(dynamicStates[0]);
	dy.pDynamicStates = dynamicStates;

	VkFormat color = (VkFormat)key->colorFormat;
	VkPipelineRenderingCreateInfo rt = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
	rt.colorAttachmentCount = 1;
	rt.pColorAttachmentFormats = &color;
	rt.depthAttachmentFormat = (VkFormat)key->depthFormat;
	rt.stencilAttachmentFormat = formatHasStencil((VkFormat)key->depthFormat) ?
		(VkFormat)key->depthFormat : VK_FORMAT_UNDEFINED;

	VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	ci.pNext = &rt;
	ci.stageCount = 2;
	ci.pStages = stages;
	ci.pVertexInputState = &vi;
	ci.pInputAssemblyState = &ia;
	ci.pViewportState = &vp;
	ci.pRasterizationState = &rs;
	ci.pMultisampleState = &ms;
	ci.pDepthStencilState = &ds;
	ci.pColorBlendState = &cb;
	ci.pDynamicState = &dy;
	ci.layout = pipelineLayout;

	VkPipeline pipeline = VK_NULL_HANDLE;
	if(vkCreateGraphicsPipelines(vkGlobals.device, vkGlobals.pipelineCache, 1, &ci, nil, &pipeline) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return pipeline;
}

static VkPipeline
getPipeline(const PipelineKey *key, VkShaderModule vs, VkShaderModule ps)
{
	uint32 h = hashKey(key);
	for(int32 i = 0; i < numPipelines; i++)
		if(pipelines[i].hash == h && memcmp(&pipelines[i].key, key, sizeof(PipelineKey)) == 0)
			return pipelines[i].pipeline;

	VkPipeline p = createPipeline(key, vs, ps);
	if(p == VK_NULL_HANDLE)
		return p;
	if(numPipelines == maxPipelines){
		int32 n = maxPipelines ? maxPipelines*2 : 256;
		PipelineEntry *e = rwNewT(PipelineEntry, n, MEMDUR_EVENT | ID_DRIVER);
		if(pipelines){
			memcpy(e, pipelines, numPipelines*sizeof(PipelineEntry));
			rwFree(pipelines);
		}
		pipelines = e;
		maxPipelines = n;
	}
	// memcpy and not assignment: the padding is compared too.
	memcpy(&pipelines[numPipelines].key, key, sizeof(PipelineKey));
	pipelines[numPipelines].hash = h;
	pipelines[numPipelines].pipeline = p;
	numPipelines++;
	return p;
}

static void
purgePipelines(void *shader)
{
	for(int32 i = 0; i < numPipelines; ){
		if(pipelines[i].key.vertexShader == shader || pipelines[i].key.pixelShader == shader){
			deferDestroyPipeline(pipelines[i].pipeline);
			pipelines[i] = pipelines[--numPipelines];
		}else
			i++;
	}
	invalidateBindings();
}

// --- vertex input -----------------------------------------------------------

// Keep in step with LOCATIONS in shaders/spirv_h.py.
static int32
locationForUsage(uint32 usage, uint32 index)
{
	switch(usage){
	case D3DDECLUSAGE_POSITION:	return index == 0 ? 0 : -1;
	case D3DDECLUSAGE_NORMAL:	return index == 0 ? 1 : -1;
	case D3DDECLUSAGE_COLOR:	return index < 2 ? 2 + (int32)index : -1;
	case D3DDECLUSAGE_BLENDWEIGHT:	return index == 0 ? 4 : -1;
	case D3DDECLUSAGE_BLENDINDICES:	return index == 0 ? 5 : -1;
	case D3DDECLUSAGE_TEXCOORD:	return index < 8 ? 6 + (int32)index : -1;
	case D3DDECLUSAGE_TANGENT:	return index == 0 ? 14 : -1;
	case D3DDECLUSAGE_BINORMAL:	return index == 0 ? 15 : -1;
	}
	return -1;
}

static VkFormat
declTypeFormat(uint32 type)
{
	switch(type){
	case D3DDECLTYPE_FLOAT1:	return VK_FORMAT_R32_SFLOAT;
	case D3DDECLTYPE_FLOAT2:	return VK_FORMAT_R32G32_SFLOAT;
	case D3DDECLTYPE_FLOAT3:	return VK_FORMAT_R32G32B32_SFLOAT;
	case D3DDECLTYPE_FLOAT4:	return VK_FORMAT_R32G32B32A32_SFLOAT;
	// BGRA bytes. Where the device will not take the format for a vertex, the
	// channels arrive swapped; openPipelines says so once.
	case D3DDECLTYPE_D3DCOLOR:
		return vkGlobals.bgraVertexColor ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
	case D3DDECLTYPE_UBYTE4:	return VK_FORMAT_R8G8B8A8_UINT;
	case D3DDECLTYPE_UBYTE4N:	return VK_FORMAT_R8G8B8A8_UNORM;
	case D3DDECLTYPE_SHORT2:	return VK_FORMAT_R16G16_SINT;
	case D3DDECLTYPE_SHORT4:	return VK_FORMAT_R16G16B16A16_SINT;
	case D3DDECLTYPE_SHORT2N:	return VK_FORMAT_R16G16_SNORM;
	case D3DDECLTYPE_SHORT4N:	return VK_FORMAT_R16G16B16A16_SNORM;
	case D3DDECLTYPE_USHORT2N:	return VK_FORMAT_R16G16_UNORM;
	case D3DDECLTYPE_USHORT4N:	return VK_FORMAT_R16G16B16A16_UNORM;
	case D3DDECLTYPE_FLOAT16_2:	return VK_FORMAT_R16G16_SFLOAT;
	case D3DDECLTYPE_FLOAT16_4:	return VK_FORMAT_R16G16B16A16_SFLOAT;
	}
	return VK_FORMAT_UNDEFINED;
}

static VkFormat
zeroInputFormat(uint8 type, uint8 components)
{
	static const VkFormat floats[] = { VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT,
		VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT };
	static const VkFormat uints[] = { VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT,
		VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT };
	static const VkFormat sints[] = { VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT,
		VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32A32_SINT };
	int i = components < 1 ? 0 : components > 4 ? 3 : components-1;
	switch(type){
	case INPUT_UINT:	return uints[i];
	case INPUT_SINT:	return sints[i];
	}
	return floats[i];
}

// The declaration and the vertex shader, as Vulkan's vertex input state. Stream
// 2 is the constant vertex: D3D binds it with a stride of zero so every vertex
// reads the same one, and Vulkan says that as one instance.
//
// What the shader reads and the geometry does not carry is fed zeroes from the
// constant vertex's texture coordinates, as d3d11shader.cpp's inputLayoutFor
// does and for the same reason.
static bool32
buildVertexInput(PipelineKey *key)
{
	VertexShader *vs = bound.vertexShader;
	d3d9::VertexElement *elements = (d3d9::VertexElement*)bound.declaration;
	if(vs == nil || elements == nil)
		return 0;

	uint32 provided = 0;
	uint32 streamsUsed = 0;
	int32 n = 0;
	for(int32 i = 0; elements[i].stream != 0xFF && i < 16; i++){
		int32 loc = locationForUsage(elements[i].usage, elements[i].usageIndex);
		if(loc < 0 || !(vs->inputMask & (1u << loc)) || (provided & (1u << loc)))
			continue;
		if(elements[i].stream > 2)
			continue;
		VkVertexInputAttributeDescription *a = &key->attributes[n++];
		a->location = loc;
		a->binding = elements[i].stream;
		a->format = declTypeFormat(elements[i].type);
		a->offset = elements[i].offset;
		provided |= 1u << loc;
		streamsUsed |= 1u << elements[i].stream;
	}
	uint32 missing = vs->inputMask & ~provided;
	for(int32 loc = 0; loc < 16; loc++){
		if(!(missing & (1u << loc)))
			continue;
		VkVertexInputAttributeDescription *a = &key->attributes[n++];
		a->location = loc;
		a->binding = 2;
		a->format = zeroInputFormat(vs->inputType[loc], vs->inputComponents[loc]);
		a->offset = offsetof(VertexConstantData, texCoors);
		streamsUsed |= 1u << 2;
	}
	key->numAttributes = n;

	int32 b = 0;
	for(uint32 s = 0; s < 3; s++){
		if(!(streamsUsed & (1u << s)))
			continue;
		if(bound.streams[s].buffer == nil)
			return 0;
		VkVertexInputBindingDescription *d = &key->bindings[b++];
		d->binding = s;
		if(s == 2){
			d->stride = sizeof(VertexConstantData);
			d->inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
		}else{
			d->stride = bound.streams[s].stride;
			d->inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
		}
	}
	key->numBindings = b;
	return 1;
}

// --- per command buffer -----------------------------------------------------

// What the command buffer being recorded has bound. A new command buffer has
// nothing, so all of this is forgotten with each one.
static struct {
	VkPipeline pipeline;
	VkDescriptorSet set;
	SetKey setKey;
	VkBuffer vertexBuffers[3];
	VkDeviceSize vertexOffsets[3];
	VkBuffer indexBuffer;
	VkDeviceSize indexOffset;
	bool32 dynamicValid;
	uint32 cullMode;
	bool32 depthTest, depthWrite;
	uint32 depthCompare;
	bool32 stencilTest;
	uint32 stencilFail, stencilPass, stencilZFail, stencilCompare;
	uint32 stencilMask, stencilWriteMask, stencilRef;
	// The constants as last uploaded this frame.
	VkBuffer constantBuffer;
	uint32 constantOffsets[3];
	uint32 constantSerial;
} cmdState;

void
commandBufferBegun(void)
{
	memset(&cmdState, 0, sizeof(cmdState));
	constantsDirty = 1;
}

void
invalidateBindings(void)
{
	cmdState.pipeline = VK_NULL_HANDLE;
	cmdState.set = VK_NULL_HANDLE;
	cmdState.dynamicValid = 0;
	for(int i = 0; i < 3; i++)
		cmdState.vertexBuffers[i] = VK_NULL_HANDLE;
	cmdState.indexBuffer = VK_NULL_HANDLE;
}

static VkCompareOp
compareOp(uint32 rwfunc)
{
	switch(rwfunc){
	case STENCILNEVER:		return VK_COMPARE_OP_NEVER;
	case STENCILLESS:		return VK_COMPARE_OP_LESS;
	case STENCILEQUAL:		return VK_COMPARE_OP_EQUAL;
	case STENCILLESSEQUAL:		return VK_COMPARE_OP_LESS_OR_EQUAL;
	case STENCILGREATER:		return VK_COMPARE_OP_GREATER;
	case STENCILNOTEQUAL:		return VK_COMPARE_OP_NOT_EQUAL;
	case STENCILGREATEREQUAL:	return VK_COMPARE_OP_GREATER_OR_EQUAL;
	}
	return VK_COMPARE_OP_ALWAYS;
}

static VkStencilOp
stencilOp(uint32 rwop)
{
	switch(rwop){
	case STENCILZERO:	return VK_STENCIL_OP_ZERO;
	case STENCILREPLACE:	return VK_STENCIL_OP_REPLACE;
	case STENCILINCSAT:	return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
	case STENCILDECSAT:	return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
	case STENCILINVERT:	return VK_STENCIL_OP_INVERT;
	case STENCILINC:	return VK_STENCIL_OP_INCREMENT_AND_WRAP;
	case STENCILDEC:	return VK_STENCIL_OP_DECREMENT_AND_WRAP;
	}
	return VK_STENCIL_OP_KEEP;
}

static void
setDynamicState(VkCommandBuffer cmd)
{
	uint32 cull = rwStateCache.cullmode == CULLBACK ? VK_CULL_MODE_BACK_BIT :
	              rwStateCache.cullmode == CULLFRONT ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE;
	bool32 depthTest = rwStateCache.ztest || rwStateCache.zwrite;
	uint32 depthCompare = rwStateCache.ztest ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_ALWAYS;
	bool valid = cmdState.dynamicValid != 0;

	if(!valid){
		// RenderWare's front face is counter-clockwise, as D3D11's rasterizer
		// is told. The viewport's negative height puts Vulkan's framebuffer in
		// D3D's orientation, so the winding reads the same way it does there.
		vkCmdSetFrontFace(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
	}
	if(!valid || cmdState.cullMode != cull){
		vkCmdSetCullMode(cmd, cull);
		cmdState.cullMode = cull;
	}
	if(!valid || cmdState.depthTest != depthTest){
		vkCmdSetDepthTestEnable(cmd, depthTest ? VK_TRUE : VK_FALSE);
		cmdState.depthTest = depthTest;
	}
	if(!valid || cmdState.depthWrite != rwStateCache.zwrite){
		vkCmdSetDepthWriteEnable(cmd, rwStateCache.zwrite ? VK_TRUE : VK_FALSE);
		cmdState.depthWrite = rwStateCache.zwrite;
	}
	if(!valid || cmdState.depthCompare != depthCompare){
		vkCmdSetDepthCompareOp(cmd, (VkCompareOp)depthCompare);
		cmdState.depthCompare = depthCompare;
	}
	if(!valid || cmdState.stencilTest != rwStateCache.stencilenable){
		vkCmdSetStencilTestEnable(cmd, rwStateCache.stencilenable ? VK_TRUE : VK_FALSE);
		cmdState.stencilTest = rwStateCache.stencilenable;
	}
	if(!valid || cmdState.stencilFail != rwStateCache.stencilfail ||
	   cmdState.stencilPass != rwStateCache.stencilpass ||
	   cmdState.stencilZFail != rwStateCache.stencilzfail ||
	   cmdState.stencilCompare != rwStateCache.stencilfunc){
		vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_FRONT_AND_BACK,
			stencilOp(rwStateCache.stencilfail), stencilOp(rwStateCache.stencilpass),
			stencilOp(rwStateCache.stencilzfail), compareOp(rwStateCache.stencilfunc));
		cmdState.stencilFail = rwStateCache.stencilfail;
		cmdState.stencilPass = rwStateCache.stencilpass;
		cmdState.stencilZFail = rwStateCache.stencilzfail;
		cmdState.stencilCompare = rwStateCache.stencilfunc;
	}
	if(!valid || cmdState.stencilMask != rwStateCache.stencilmask){
		vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, rwStateCache.stencilmask);
		cmdState.stencilMask = rwStateCache.stencilmask;
	}
	if(!valid || cmdState.stencilWriteMask != rwStateCache.stencilwritemask){
		vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, rwStateCache.stencilwritemask);
		cmdState.stencilWriteMask = rwStateCache.stencilwritemask;
	}
	if(!valid || cmdState.stencilRef != rwStateCache.stencilref){
		vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, rwStateCache.stencilref);
		cmdState.stencilRef = rwStateCache.stencilref;
	}
	cmdState.dynamicValid = 1;
}

// The constants, into this frame's arena. Laid out as one span so the three
// dynamic offsets share a buffer, which is all the descriptor set names.
static bool32
uploadConstants(void)
{
	if(!constantsDirty && cmdState.constantSerial == frameSerial() &&
	   cmdState.constantBuffer != VK_NULL_HANDLE)
		return 1;

	VkDeviceSize align = vkGlobals.properties.limits.minUniformBufferOffsetAlignment;
	if(align < 16)
		align = 16;
	VkDeviceSize o1 = (sizeof(vsConstants) + align-1) & ~(align-1);
	VkDeviceSize o2 = o1 + ((sizeof(vsIntConstants) + align-1) & ~(align-1));
	ArenaSpan span;
	if(!arenaAlloc(o2 + sizeof(psConstants), align, &span))
		return 0;
	memcpy(span.data, vsConstants, sizeof(vsConstants));
	memcpy(span.data + o1, vsIntConstants, sizeof(vsIntConstants));
	memcpy(span.data + o2, psConstants, sizeof(psConstants));
	cmdState.constantBuffer = span.buffer;
	cmdState.constantOffsets[0] = (uint32)span.offset;
	cmdState.constantOffsets[1] = (uint32)(span.offset + o1);
	cmdState.constantOffsets[2] = (uint32)(span.offset + o2);
	cmdState.constantSerial = frameSerial();
	constantsDirty = 0;
	return 1;
}

// Every stage's image in the layout a shader reads it in. A camera texture
// rendered into earlier in the frame is not, and moving it cannot be done
// inside a render pass -- so that closes the pass, which the draw reopens.
static void
prepareTextures(SetKey *key)
{
	Image *target = currentColorTarget();
	for(int i = 0; i < NUMTEXSTAGES; i++){
		Raster *raster = rwStateCache.texstage[i].raster;
		Image *img = raster ? rasterImage(raster) : nil;
		// A texture being drawn into cannot also be read. D3D11 answers that
		// by unbinding it; so does this.
		if(img == nil || img == target || img->depth)
			img = whiteTexture();
		if(img->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL){
			VkCommandBuffer cmd = frameCommands();
			endRendering();
			transitionImage(cmd, img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		}
		key->views[i] = img->sampleView;

		SamplerKey sk;
		sk.filter = rwStateCache.texstage[i].filter;
		sk.addressU = rwStateCache.texstage[i].addressingU;
		sk.addressV = rwStateCache.texstage[i].addressingV;
		sk.maxAnisotropy = rwStateCache.texstage[i].maxAnisotropy;
		key->samplers[i] = getSampler(&sk);
	}
}

static VkPrimitiveTopology
topology(uint32 primType)
{
	switch(primType){
	case D3DPT_POINTLIST:		return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
	case D3DPT_LINELIST:		return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
	case D3DPT_LINESTRIP:		return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
	case D3DPT_TRIANGLESTRIP:	return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
	}
	return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

bool32
prepareDraw(uint32 primType)
{
	if(bound.vertexShader == nil || bound.pixelShader == nil)
		return 0;

	SetKey setKey;
	memset(&setKey, 0, sizeof(setKey));
	prepareTextures(&setKey);
	ensureRendering();
	if(!renderingOpen())
		return 0;
	VkCommandBuffer cmd = frameCommands();

	PipelineKey key;
	memset(&key, 0, sizeof(key));
	if(!buildVertexInput(&key))
		return 0;
	TargetFormat target = currentTargetFormat();
	key.vertexShader = bound.vertexShader;
	key.pixelShader = bound.pixelShader;
	key.topology = topology(primType);
	key.colorFormat = target.color;
	key.depthFormat = target.depth;
	key.samples = target.samples;
	key.blendEnable = rwStateCache.blendenable;
	if(key.blendEnable){
		key.srcBlend = rwStateCache.srcblend;
		key.destBlend = rwStateCache.destblend;
	}
	key.writeMask = rwStateCache.colorwritemask;
	VkPipeline pipeline = getPipeline(&key, bound.vertexShader->module, bound.pixelShader->module);
	if(pipeline == VK_NULL_HANDLE)
		return 0;
	if(cmdState.pipeline != pipeline){
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		cmdState.pipeline = pipeline;
	}
	setDynamicState(cmd);

	for(uint32 i = 0; i < key.numBindings; i++){
		uint32 s = key.bindings[i].binding;
		VkBuffer buf;
		VkDeviceSize off;
		if(!bufferBinding(bound.streams[s].buffer, &buf, &off))
			return 0;
		off += bound.streams[s].offset;
		if(cmdState.vertexBuffers[s] != buf || cmdState.vertexOffsets[s] != off){
			vkCmdBindVertexBuffers(cmd, s, 1, &buf, &off);
			cmdState.vertexBuffers[s] = buf;
			cmdState.vertexOffsets[s] = off;
		}
	}

	if(!uploadConstants())
		return 0;
	setKey.constants = cmdState.constantBuffer;
	if(cmdState.set == VK_NULL_HANDLE || memcmp(&cmdState.setKey, &setKey, sizeof(SetKey)) != 0){
		cmdState.set = getSet(&setKey);
		cmdState.setKey = setKey;
		if(cmdState.set == VK_NULL_HANDLE)
			return 0;
	}
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1,
		&cmdState.set, 3, cmdState.constantOffsets);
	return 1;
}

void
bindIndexBuffer(void *buffer)
{
	VkBuffer buf;
	VkDeviceSize off;
	if(!bufferBinding(buffer, &buf, &off))
		return;
	if(cmdState.indexBuffer != buf || cmdState.indexOffset != off){
		vkCmdBindIndexBuffer(frameCommands(), buf, off, VK_INDEX_TYPE_UINT16);
		cmdState.indexBuffer = buf;
		cmdState.indexOffset = off;
	}
}

void
bindFanIndices(VkBuffer buffer)
{
	if(cmdState.indexBuffer != buffer || cmdState.indexOffset != 0){
		vkCmdBindIndexBuffer(frameCommands(), buffer, 0, VK_INDEX_TYPE_UINT16);
		cmdState.indexBuffer = buffer;
		cmdState.indexOffset = 0;
	}
}

// --- the blit ---------------------------------------------------------------

static VertexShader *blitVS;
static PixelShader *blitPS;
static VertexShader *overlayVS;
static PixelShader *overlayPS;

namespace spv {
#include "shadersvk/blit_VS.h"
#include "shadersvk/blit_PS.h"
// GLSL, not HLSL: see shaders/glsl_h.py.
#include "shadersvk/overlay_VS.h"
#include "shadersvk/overlay_PS.h"
}

void
drawBlit(VkFormat format, Image *source)
{
	if(blitVS == nil || blitPS == nil)
		return;
	VkCommandBuffer cmd = frameCommands();

	PipelineKey key;
	memset(&key, 0, sizeof(key));
	key.vertexShader = blitVS;
	key.pixelShader = blitPS;
	key.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	key.colorFormat = format;
	key.depthFormat = VK_FORMAT_UNDEFINED;
	key.samples = VK_SAMPLE_COUNT_1_BIT;
	key.writeMask = 0xF;
	VkPipeline pipeline = getPipeline(&key, blitVS->module, blitPS->module);
	if(pipeline == VK_NULL_HANDLE)
		return;
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	vkCmdSetFrontFace(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
	vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
	vkCmdSetDepthTestEnable(cmd, VK_FALSE);
	vkCmdSetDepthWriteEnable(cmd, VK_FALSE);
	vkCmdSetDepthCompareOp(cmd, VK_COMPARE_OP_ALWAYS);
	vkCmdSetStencilTestEnable(cmd, VK_FALSE);
	vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP,
		VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS);
	vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
	vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
	vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);

	if(!uploadConstants())
		return;
	SetKey setKey;
	memset(&setKey, 0, sizeof(setKey));
	for(int i = 0; i < NUMTEXSTAGES; i++){
		setKey.views[i] = i == 0 ? source->view : whiteTexture()->view;
		setKey.samplers[i] = blitSampler;
	}
	setKey.constants = cmdState.constantBuffer;
	VkDescriptorSet set = getSet(&setKey);
	if(set == VK_NULL_HANDLE)
		return;
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1,
		&set, 3, cmdState.constantOffsets);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	invalidateBindings();
}

// The present overlay's triangles. See drawPresentOverlay in rwd3d.h for the
// vertex; the position is turned from window pixels into clip space here, so
// the shaders need no constants and no descriptor set.
void
drawOverlay(VkFormat format, VkExtent2D extent, const float32 *vertices, int32 numVertices)
{
	enum { STRIDE = 12 };
	if(overlayVS == nil || overlayPS == nil || numVertices <= 0 ||
	   extent.width == 0 || extent.height == 0)
		return;
	VkCommandBuffer cmd = frameCommands();

	ArenaSpan span;
	if(!arenaAlloc((VkDeviceSize)numVertices*STRIDE*sizeof(float32), 4, &span))
		return;
	float32 *out = (float32*)span.data;
	memcpy(out, vertices, numVertices*STRIDE*sizeof(float32));
	for(int32 i = 0; i < numVertices; i++, out += STRIDE){
		out[0] = out[0]/extent.width*2.0f - 1.0f;
		out[1] = out[1]/extent.height*2.0f - 1.0f;
	}

	PipelineKey key;
	memset(&key, 0, sizeof(key));
	key.vertexShader = overlayVS;
	key.pixelShader = overlayPS;
	key.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	key.colorFormat = format;
	key.depthFormat = VK_FORMAT_UNDEFINED;
	key.samples = VK_SAMPLE_COUNT_1_BIT;
	key.blendEnable = 1;
	key.srcBlend = BLENDSRCALPHA;
	key.destBlend = BLENDINVSRCALPHA;
	key.writeMask = 0xF;
	key.numBindings = 1;
	key.bindings[0].binding = 0;
	key.bindings[0].stride = STRIDE*sizeof(float32);
	key.bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	static const VkFormat formats[5] = {
		VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32_SFLOAT,
		VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT
	};
	static const uint32 offsets[5] = { 0, 8, 16, 24, 32 };
	key.numAttributes = 5;
	for(int i = 0; i < 5; i++){
		key.attributes[i].location = i;
		key.attributes[i].binding = 0;
		key.attributes[i].format = formats[i];
		key.attributes[i].offset = offsets[i];
	}
	VkPipeline pipeline = getPipeline(&key, overlayVS->module, overlayPS->module);
	if(pipeline == VK_NULL_HANDLE)
		return;

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	vkCmdSetFrontFace(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
	vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
	vkCmdSetDepthTestEnable(cmd, VK_FALSE);
	vkCmdSetDepthWriteEnable(cmd, VK_FALSE);
	vkCmdSetDepthCompareOp(cmd, VK_COMPARE_OP_ALWAYS);
	vkCmdSetStencilTestEnable(cmd, VK_FALSE);
	vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP,
		VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS);
	vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
	vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
	vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);

	vkCmdBindVertexBuffers(cmd, 0, 1, &span.buffer, &span.offset);
	vkCmdDraw(cmd, numVertices, 1, 0, 0);
	invalidateBindings();
}

// --- open and close ---------------------------------------------------------

void
openPipelines(void)
{
	VkDescriptorSetLayoutBinding b[NUMBINDINGS];
	memset(b, 0, sizeof(b));
	for(int i = 0; i < NUMBINDINGS; i++){
		b[i].binding = i;
		b[i].descriptorCount = 1;
	}
	b[BINDING_VSGLOBALS].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	b[BINDING_VSGLOBALS].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	b[BINDING_VSINTS].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	b[BINDING_VSINTS].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	b[BINDING_PSGLOBALS].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	b[BINDING_PSGLOBALS].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	for(int i = 0; i < NUMTEXSTAGES; i++){
		b[BINDING_TEXTURE0+i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		b[BINDING_TEXTURE0+i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		b[BINDING_SAMPLER0+i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
		b[BINDING_SAMPLER0+i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	lci.bindingCount = NUMBINDINGS;
	lci.pBindings = b;
	vkCreateDescriptorSetLayout(vkGlobals.device, &lci, nil, &setLayout);

	VkPipelineLayoutCreateInfo pci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	pci.setLayoutCount = 1;
	pci.pSetLayouts = &setLayout;
	vkCreatePipelineLayout(vkGlobals.device, &pci, nil, &pipelineLayout);

	VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	sci.magFilter = VK_FILTER_LINEAR;
	sci.minFilter = VK_FILTER_LINEAR;
	sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	vkCreateSampler(vkGlobals.device, &sci, nil, &blitSampler);

	blitVS = (VertexShader*)createVertexShader((void*)spv::blit_VS);
	blitPS = (PixelShader*)createPixelShader((void*)spv::blit_PS);
	overlayVS = (VertexShader*)createVertexShader((void*)spv::overlay_VS);
	overlayPS = (PixelShader*)createPixelShader((void*)spv::overlay_PS);

	if(!vkGlobals.bgraVertexColor)
		fprintf(stderr, "librw: this Vulkan device cannot take B8G8R8A8 vertex colours; "
			"red and blue will be swapped\n");

	memset(&bound, 0, sizeof(bound));
	resetRenderState();
	commandBufferBegun();
}

void
closePipelines(void)
{
	destroyVertexShader(blitVS);
	blitVS = nil;
	destroyPixelShader(blitPS);
	blitPS = nil;
	destroyVertexShader(overlayVS);
	overlayVS = nil;
	destroyPixelShader(overlayPS);
	overlayPS = nil;
	for(int32 i = 0; i < numPipelines; i++)
		vkDestroyPipeline(vkGlobals.device, pipelines[i].pipeline, nil);
	rwFree(pipelines);
	pipelines = nil;
	numPipelines = maxPipelines = 0;
	for(int32 i = 0; i < numSamplers; i++)
		vkDestroySampler(vkGlobals.device, samplers[i].sampler, nil);
	numSamplers = 0;
	if(blitSampler != VK_NULL_HANDLE){
		vkDestroySampler(vkGlobals.device, blitSampler, nil);
		blitSampler = VK_NULL_HANDLE;
	}
	for(int32 j = 0; j < FRAMESINFLIGHT; j++){
		PoolSet *ps = &poolSets[j];
		for(int32 i = 0; i < ps->numPools; i++)
			vkDestroyDescriptorPool(vkGlobals.device, ps->pools[i], nil);
		rwFree(ps->pools);
		memset(ps, 0, sizeof(*ps));
	}
	vkDestroyPipelineLayout(vkGlobals.device, pipelineLayout, nil);
	pipelineLayout = VK_NULL_HANDLE;
	vkDestroyDescriptorSetLayout(vkGlobals.device, setLayout, nil);
	setLayout = VK_NULL_HANDLE;
}

#endif
}
}
}
