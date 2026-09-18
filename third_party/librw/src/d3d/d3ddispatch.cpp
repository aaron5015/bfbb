// rw::d3d's entry points, forwarded to whichever implementation is running.
//
// The device implementations -- Direct3D 9, Direct3D 11 and Vulkan -- implement
// one interface. The raster layer, the immediate mode, the pipelines and
// everything above them are written against rw::d3d and are compiled once, and
// the implementations used to BE rw::d3d, one at a time, which is why a build
// could only ever carry one. Their definitions now sit in rw::d3d::impl9,
// rw::d3d::impl11 and rw::d3d::implvk, and this file is rw::d3d.
//
// The cost is one call per device operation. That is the price of the choice
// being a setting rather than a build, and these are state setters that already
// end in a driver call.

#include <stddef.h>

#define WITH_D3D
#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwrender.h"
#include "../rwengine.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "rwd3d.h"

#define PLUGIN_ID 0

namespace rw {
namespace d3d {

// Each arm is the implementation's call when it is built in and a call that is
// never reached when it is not, so one definition serves every combination.
// RWD3D_IS9 and friends are constants when there is only one.
#if defined(RW_VULKAN)
#define RWD3D_ARMANY(f, args) implvk::f args
#elif defined(RW_D3D11)
#define RWD3D_ARMANY(f, args) impl11::f args
#else
#define RWD3D_ARMANY(f, args) impl9::f args
#endif
#ifdef RW_D3D9
#define RWD3D_ARM9(f, args) impl9::f args
#else
#define RWD3D_ARM9(f, args) RWD3D_ARMANY(f, args)
#endif
#ifdef RW_D3D11
#define RWD3D_ARM11(f, args) impl11::f args
#else
#define RWD3D_ARM11(f, args) RWD3D_ARMANY(f, args)
#endif
#ifdef RW_VULKAN
#define RWD3D_ARMVK(f, args) implvk::f args
#else
#define RWD3D_ARMVK(f, args) RWD3D_ARMANY(f, args)
#endif

#define RWD3D_CALL(f, args) \
	(RWD3D_ISVK ? RWD3D_ARMVK(f, args) : RWD3D_IS11 ? RWD3D_ARM11(f, args) : RWD3D_ARM9(f, args))
#define RWD3D_VOID(f, args)                                             \
	do {                                                            \
		if(RWD3D_ISVK) RWD3D_ARMVK(f, args);                    \
		else if(RWD3D_IS11) RWD3D_ARM11(f, args);               \
		else RWD3D_ARM9(f, args);                               \
	} while(0)

#ifdef RW_D3D_ANY

Device &
renderDevice(void)
{
	return RWD3D_ISVK ? RWD3D_ARMVK(renderdevice, ) :
	       RWD3D_IS11 ? RWD3D_ARM11(renderdevice, ) : RWD3D_ARM9(renderdevice, );
}

bool32
captureFrame(Raster *dst)
{
	return RWD3D_CALL(captureFrame, (dst));
}

void *
createPixelShader(void *csosrc)
{
	return RWD3D_CALL(createPixelShader, (csosrc));
}

void *
createVertexShader(void *csosrc)
{
	return RWD3D_CALL(createVertexShader, (csosrc));
}

void
destroyPixelShader(void *shader)
{
	RWD3D_VOID(destroyPixelShader, (shader));
}

void
destroyVertexShader(void *shader)
{
	RWD3D_VOID(destroyVertexShader, (shader));
}

bool32
deviceOpen(void)
{
	return RWD3D_CALL(deviceOpen, ());
}

void
drawIndexedPrimitive(uint32 primType, int32 baseVertex, uint32 minVertex,
	uint32 numVertices, uint32 startIndex, uint32 numPrimitives)
{
	RWD3D_VOID(drawIndexedPrimitive, (primType, baseVertex, minVertex, numVertices, startIndex, numPrimitives));
}

void
drawPrimitive(uint32 primType, uint32 startVertex, uint32 numPrimitives)
{
	RWD3D_VOID(drawPrimitive, (primType, startVertex, numPrimitives));
}

void
flushCache(void)
{
	RWD3D_VOID(flushCache, ());
}

bool32
getBlendEnabled(void)
{
	return RWD3D_CALL(getBlendEnabled, ());
}

void
getRenderState(uint32 state, uint32 *value)
{
	RWD3D_VOID(getRenderState, (state, value));
}

void
getSamplerState(uint32 stage, uint32 type, uint32 *value)
{
	RWD3D_VOID(getSamplerState, (stage, type, value));
}

void
getScreenExtent(int32 *width, int32 *height)
{
	RWD3D_VOID(getScreenExtent, (width, height));
}

void
getTextureStageState(uint32 stage, uint32 type, uint32 *value)
{
	RWD3D_VOID(getTextureStageState, (stage, type, value));
}

void
getVirtualScreen(int32 *width, int32 *height)
{
	RWD3D_VOID(getVirtualScreen, (width, height));
}

int32
getVirtualScreenSamples(void)
{
	return RWD3D_CALL(getVirtualScreenSamples, ());
}

void
setIm2DActive(bool32 active)
{
	RWD3D_VOID(setIm2DActive, (active));
}

void
setIndices(void *indexBuffer)
{
	RWD3D_VOID(setIndices, (indexBuffer));
}

void
setMaterial(const RGBA &color, const SurfaceProperties &surfaceprops, float extraSurfProp)
{
	RWD3D_VOID(setMaterial, (color, surfaceprops, extraSurfProp));
}

void
setPipelineVertexAlpha(bool32 enable)
{
	RWD3D_VOID(setPipelineVertexAlpha, (enable));
}

void
setPixelShader(void *ps)
{
	RWD3D_VOID(setPixelShader, (ps));
}

void
setPixelShaderConstantF(uint32 reg, const float32 *data, int32 numRegs)
{
	RWD3D_VOID(setPixelShaderConstantF, (reg, data, numRegs));
}

void
setRasterStage(uint32 stage, Raster *raster)
{
	RWD3D_VOID(setRasterStage, (stage, raster));
}

void
setRenderState(uint32 state, uint32 value)
{
	RWD3D_VOID(setRenderState, (state, value));
}

void
setSamplerState(uint32 stage, uint32 type, uint32 value)
{
	RWD3D_VOID(setSamplerState, (stage, type, value));
}

void
setStreamSource(int n, void *buffer, uint32 offset, uint32 stride)
{
	RWD3D_VOID(setStreamSource, (n, buffer, offset, stride));
}

void
setTexture(uint32 stage, Texture *tex)
{
	RWD3D_VOID(setTexture, (stage, tex));
}

void
setTextureStageState(uint32 stage, uint32 type, uint32 value)
{
	RWD3D_VOID(setTextureStageState, (stage, type, value));
}

void
setVertexDeclaration(void *declaration)
{
	RWD3D_VOID(setVertexDeclaration, (declaration));
}

void
setVertexShader(void *vs)
{
	RWD3D_VOID(setVertexShader, (vs));
}

void
setVertexShaderConstantF(uint32 reg, const float32 *data, int32 numRegs)
{
	RWD3D_VOID(setVertexShaderConstantF, (reg, data, numRegs));
}

void
setVertexShaderConstantI(uint32 reg, const int32 *data, int32 numRegs)
{
	RWD3D_VOID(setVertexShaderConstantI, (reg, data, numRegs));
}

void
setVirtualScreen(int32 width, int32 height)
{
	RWD3D_VOID(setVirtualScreen, (width, height));
}

void
setVirtualScreenSamples(int32 samples)
{
	RWD3D_VOID(setVirtualScreenSamples, (samples));
}
#endif

}
}
