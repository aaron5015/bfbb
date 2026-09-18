#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define WITH_D3D
#include "../rwbase.h"
#include "../rwplg.h"
#include "../rwrender.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "rwd3d.h"
#include "rwd3d9.h"
#include "rwd3dimpl.h"

namespace rw {
namespace d3d {

#if defined(RW_D3D_ANY)

// might want to tweak this
#define NUMINDICES 10000
#define NUMVERTICES 10000

static int primTypeMap[] = {
	D3DPT_POINTLIST,	// invalid
	D3DPT_LINELIST,
	D3DPT_LINESTRIP,
	D3DPT_TRIANGLELIST,
	D3DPT_TRIANGLESTRIP,
	D3DPT_TRIANGLEFAN,
	D3DPT_POINTLIST,	// actually not supported!
};

static void *im2ddecl;
static void *im2dvertbuf;
static void *im2dindbuf;

void *im2dOverridePS;

void
openIm2D(void)
{
	d3d9::VertexElement elements[4] = {
// can't get proper fog with this :(
//		{ 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITIONT, 0 },
		{ 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
		{ 0, offsetof(Im2DVertex, color), D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
		{ 0, offsetof(Im2DVertex, u), D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
		{ 0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0 }
	};
	assert(im2ddecl == nil);
	im2ddecl = d3d9::createVertexDeclaration((d3d9::VertexElement*)elements);
	assert(im2ddecl);

	assert(im2dvertbuf == nil);
	im2dvertbuf = createVertexBuffer(NUMVERTICES*sizeof(Im2DVertex), 0, true);
	assert(im2dvertbuf);

	assert(im2dindbuf == nil);
	im2dindbuf = createIndexBuffer(NUMINDICES*sizeof(uint16), true);
	assert(im2dindbuf);

#ifdef RW_D3D9
	// The pool a lost device drops. D3D11 has no lost device and no pool.
	addDynamicVB(NUMVERTICES*sizeof(Im2DVertex), 0, (IDirect3DVertexBuffer9**)&im2dvertbuf);
	addDynamicIB(NUMINDICES*sizeof(uint16), (IDirect3DIndexBuffer9**)&im2dindbuf);
#endif

	// The same buffers, drawn through a POSITIONT declaration instead. Only
	// the declaration differs, so the fixed-function path shares the storage.
	if(getFixedFunction())
		ffOpenIm2D();
}

void
closeIm2D(void)
{
	if(getFixedFunction())
		ffCloseIm2D();
#ifdef RW_D3D9
	removeDynamicVB((IDirect3DVertexBuffer9**)&im2dvertbuf);
	removeDynamicIB((IDirect3DIndexBuffer9**)&im2dindbuf);
#endif
	d3d9::destroyVertexDeclaration(im2ddecl);
	im2ddecl = nil;

	destroyVertexBuffer(im2dvertbuf);
	im2dvertbuf = nil;

	destroyIndexBuffer(im2dindbuf);
	im2dindbuf = nil;
}

static Im2DVertex tmpprimbuf[3];

void
im2DRenderLine(void *vertices, int32 numVertices, int32 vert1, int32 vert2)
{
	Im2DVertex *verts = (Im2DVertex*)vertices;
	tmpprimbuf[0] = verts[vert1];
	tmpprimbuf[1] = verts[vert2];
	im2DRenderPrimitive(PRIMTYPELINELIST, tmpprimbuf, 2);
}

void
im2DRenderTriangle(void *vertices, int32 numVertices, int32 vert1, int32 vert2, int32 vert3)
{
	Im2DVertex *verts = (Im2DVertex*)vertices;
	tmpprimbuf[0] = verts[vert1];
	tmpprimbuf[1] = verts[vert2];
	tmpprimbuf[2] = verts[vert3];
	im2DRenderPrimitive(PRIMTYPETRILIST, tmpprimbuf, 3);
}

void
im2DSetXform(void)
{
	float xform[4];
	Camera *cam;
	cam = (Camera*)engine->currentCamera;
	xform[0] = 2.0f/cam->frameBuffer->width;
	xform[1] = -2.0f/cam->frameBuffer->height;
	xform[2] = -1.0f;
	xform[3] = 1.0f;
	// TODO: should cache this...
	setVertexShaderConstantF(VSLOC_afterLights, xform, 1);
}

void
im2DRenderPrimitive(PrimitiveType primType, void *vertices, int32 numVertices)
{
	if(numVertices > NUMVERTICES){
		// TODO: error
		return;
	}
	uint8 *lockedvertices = lockVertices(im2dvertbuf, 0, numVertices*sizeof(Im2DVertex), D3DLOCK_DISCARD);
	if(getFixedFunction())
		ffCopyIm2DVertices(lockedvertices, vertices, numVertices);
	else
		memcpy(lockedvertices, vertices, numVertices*sizeof(Im2DVertex));
	unlockVertices(im2dvertbuf);

	setStreamSource(0, im2dvertbuf, 0, sizeof(Im2DVertex));

	if(getFixedFunction()){
		setVertexDeclaration(ffIm2DDeclaration());
		ffSetupIm2D();
	}else{
		setVertexDeclaration(im2ddecl);
		im2DSetXform();
		setVertexShader(im2d_VS);
		if(im2dOverridePS)
			setPixelShader(im2dOverridePS);
		else if(engine->device.getRenderState(TEXTURERASTER))
			setPixelShader(im2d_tex_PS);
		else
			setPixelShader(im2d_PS);
	}

	// 2D for the length of the draw. Alpha to coverage is a statement about a
	// surface's place in the depth buffer, and a screen-space quad has none:
	// the interface and the font ask for a blend and have to get one.
	setIm2DActive(1);
	d3d::flushCache();

	uint32 primCount = 0;
	switch(primType){
	case PRIMTYPENONE:
		// Not a primitive. The count stays zero and the draw below draws
		// nothing, which is what this did before the case was written down.
		break;
	case PRIMTYPELINELIST:
		primCount = numVertices/2;
		break;
	case PRIMTYPEPOLYLINE:
		primCount = numVertices-1;
		break;
	case PRIMTYPETRILIST:
		primCount = numVertices/3;
		break;
	case PRIMTYPETRISTRIP:
		primCount = numVertices-2;
		break;
	case PRIMTYPETRIFAN:
		primCount = numVertices-2;
		break;
	case PRIMTYPEPOINTLIST:
		primCount = numVertices;
		break;
	}
	drawPrimitive(primTypeMap[primType], 0, primCount);
	setIm2DActive(0);
}

void
im2DRenderIndexedPrimitive(PrimitiveType primType,
   void *vertices, int32 numVertices, void *indices, int32 numIndices)
{
	if(numVertices > NUMVERTICES ||
	   numIndices > NUMINDICES){
		// TODO: error
		return;
	}
	uint16 *lockedindices = lockIndices(im2dindbuf, 0, numIndices*sizeof(uint16), D3DLOCK_DISCARD);
	memcpy(lockedindices, indices, numIndices*sizeof(uint16));
	unlockIndices(im2dindbuf);

	uint8 *lockedvertices = lockVertices(im2dvertbuf, 0, numVertices*sizeof(Im2DVertex), D3DLOCK_DISCARD);
	if(getFixedFunction())
		ffCopyIm2DVertices(lockedvertices, vertices, numVertices);
	else
		memcpy(lockedvertices, vertices, numVertices*sizeof(Im2DVertex));
	unlockVertices(im2dvertbuf);

	setStreamSource(0, im2dvertbuf, 0, sizeof(Im2DVertex));
	setIndices(im2dindbuf);

	if(getFixedFunction()){
		setVertexDeclaration(ffIm2DDeclaration());
		ffSetupIm2D();
	}else{
		setVertexDeclaration(im2ddecl);
		im2DSetXform();
		setVertexShader(im2d_VS);
		if(im2dOverridePS)
			setPixelShader(im2dOverridePS);
		else if(engine->device.getRenderState(TEXTURERASTER))
			setPixelShader(im2d_tex_PS);
		else
			setPixelShader(im2d_PS);
	}

	// 2D for the length of the draw. Alpha to coverage is a statement about a
	// surface's place in the depth buffer, and a screen-space quad has none:
	// the interface and the font ask for a blend and have to get one.
	setIm2DActive(1);
	d3d::flushCache();

	uint32 primCount = 0;
	switch(primType){
	case PRIMTYPENONE:
		// Not a primitive. The count stays zero and the draw below draws
		// nothing, which is what this did before the case was written down.
		break;
	case PRIMTYPELINELIST:
		primCount = numIndices/2;
		break;
	case PRIMTYPEPOLYLINE:
		primCount = numIndices-1;
		break;
	case PRIMTYPETRILIST:
		primCount = numIndices/3;
		break;
	case PRIMTYPETRISTRIP:
		primCount = numIndices-2;
		break;
	case PRIMTYPETRIFAN:
		primCount = numIndices-2;
		break;
	case PRIMTYPEPOINTLIST:
		primCount = numIndices;
		break;
	}
	drawIndexedPrimitive(primTypeMap[primType], 0,
	                     0, numVertices,
	                     0, primCount);
	setIm2DActive(0);
}


// Im3D


static void *im3ddecl;
static void *im3dvertbuf;
static void *im3dindbuf;
static int32 num3DVertices;

void
openIm3D(void)
{
	d3d9::VertexElement elements[5] = {
		{ 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
		{ 0, offsetof(Im3DVertex, normal), D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0 },
		{ 0, offsetof(Im3DVertex, color), D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
		{ 0, offsetof(Im3DVertex, u), D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
		{ 0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0 }
	};

	assert(im3ddecl == nil);
	im3ddecl = d3d9::createVertexDeclaration((d3d9::VertexElement*)elements);
	assert(im3ddecl);

	assert(im3dvertbuf == nil);
	im3dvertbuf = createVertexBuffer(NUMVERTICES*sizeof(Im3DVertex), 0, true);
	assert(im3dvertbuf);

	assert(im3dindbuf == nil);
	im3dindbuf = createIndexBuffer(NUMINDICES*sizeof(uint16), true);
	assert(im3dindbuf);

#ifdef RW_D3D9
	addDynamicVB(NUMVERTICES*sizeof(Im3DVertex), 0, (IDirect3DVertexBuffer9**)&im3dvertbuf);
	addDynamicIB(NUMINDICES*sizeof(uint16), (IDirect3DIndexBuffer9**)&im3dindbuf);
#endif
}

void
closeIm3D(void)
{
#ifdef RW_D3D9
	removeDynamicVB((IDirect3DVertexBuffer9**)&im3dvertbuf);
	removeDynamicIB((IDirect3DIndexBuffer9**)&im3dindbuf);
#endif
	d3d9::destroyVertexDeclaration(im3ddecl);
	im3ddecl = nil;

	destroyVertexBuffer(im3dvertbuf);
	im3dvertbuf = nil;

	destroyIndexBuffer(im3dindbuf);
	im3dindbuf = nil;
}

// settable by user - TOOD: make this less shit
RGBA im3dMaterialColor = { 255, 255, 255, 255 };
SurfaceProperties im3dSurfaceProps = { 1.0f, 1.0f, 1.0f };

void
im3DTransform(void *vertices, int32 numVertices, Matrix *world, uint32 flags)
{
	if(getFixedFunction()){
		if(world == nil)
			ffSetWorldTransform();
		else
			ffSetWorldTransform(world);
	}else if(world == nil)
		uploadMatrices();
	else
		uploadMatrices(world);

	if((flags & im3d::VERTEXUV) == 0)
		SetRenderStatePtr(TEXTURERASTER, nil);

	if(getFixedFunction()){
		ffSetupIm3D(flags);

		uint8 *ffverts = lockVertices(im3dvertbuf, 0, numVertices*sizeof(Im3DVertex), D3DLOCK_DISCARD);
		memcpy(ffverts, vertices, numVertices*sizeof(Im3DVertex));
		unlockVertices(im3dvertbuf);

		setStreamSource(0, im3dvertbuf, 0, sizeof(Im3DVertex));
		setVertexDeclaration(im3ddecl);

		num3DVertices = numVertices;
		return;
	}

	void *shader = default_amb_VS;
	if(flags & im3d::LIGHTING){
		setMaterial(im3dMaterialColor, im3dSurfaceProps);
		int32 vsBits = lightingCB_Shader();
		// Pick a shader
		if((vsBits & VSLIGHT_MASK) == 0)
			shader = default_amb_VS;
		else if((vsBits & VSLIGHT_MASK) == VSLIGHT_DIRECT)
			shader = default_amb_dir_VS;
		else
			shader = default_all_VS;
	}else{
		static RGBA white = { 255, 255, 255, 255 };
		static SurfaceProperties surfprops = { 0.0f, 0.0f, 0.0f };
		setMaterial(white, surfprops);
	}

	uint8 *lockedvertices = lockVertices(im3dvertbuf, 0, numVertices*sizeof(Im3DVertex), D3DLOCK_DISCARD);
	memcpy(lockedvertices, vertices, numVertices*sizeof(Im3DVertex));
	unlockVertices(im3dvertbuf);

	setStreamSource(0, im3dvertbuf, 0, sizeof(Im3DVertex));
	setVertexDeclaration(im3ddecl);

	setVertexShader(shader);

	num3DVertices = numVertices;
}

void
im3DRenderPrimitive(PrimitiveType primType)
{
	if(getFixedFunction())
		ffSetupIm3DDraw();
	else if(engine->device.getRenderState(TEXTURERASTER))
		setPixelShader(default_tex_PS);
	else
		setPixelShader(default_PS);

	d3d::flushCache();

	uint32 primCount = 0;
	switch(primType){
	case PRIMTYPENONE:
		// Not a primitive. The count stays zero and the draw below draws
		// nothing, which is what this did before the case was written down.
		break;
	case PRIMTYPELINELIST:
		primCount = num3DVertices/2;
		break;
	case PRIMTYPEPOLYLINE:
		primCount = num3DVertices-1;
		break;
	case PRIMTYPETRILIST:
		primCount = num3DVertices/3;
		break;
	case PRIMTYPETRISTRIP:
		primCount = num3DVertices-2;
		break;
	case PRIMTYPETRIFAN:
		primCount = num3DVertices-2;
		break;
	case PRIMTYPEPOINTLIST:
		primCount = num3DVertices;
		break;
	}
	drawPrimitive(primTypeMap[primType], 0, primCount);
}

void
im3DRenderIndexedPrimitive(PrimitiveType primType, void *indices, int32 numIndices)
{
	uint16 *lockedindices = lockIndices(im3dindbuf, 0, numIndices*sizeof(uint16), D3DLOCK_DISCARD);
	memcpy(lockedindices, indices, numIndices*sizeof(uint16));
	unlockIndices(im3dindbuf);

	setIndices(im3dindbuf);

	if(getFixedFunction())
		ffSetupIm3DDraw();
	else if(engine->device.getRenderState(TEXTURERASTER))
		setPixelShader(default_tex_PS);
	else
		setPixelShader(default_PS);

	d3d::flushCache();

	uint32 primCount = 0;
	switch(primType){
	case PRIMTYPENONE:
		// Not a primitive. The count stays zero and the draw below draws
		// nothing, which is what this did before the case was written down.
		break;
	case PRIMTYPELINELIST:
		primCount = numIndices/2;
		break;
	case PRIMTYPEPOLYLINE:
		primCount = numIndices-1;
		break;
	case PRIMTYPETRILIST:
		primCount = numIndices/3;
		break;
	case PRIMTYPETRISTRIP:
		primCount = numIndices-2;
		break;
	case PRIMTYPETRIFAN:
		primCount = numIndices-2;
		break;
	case PRIMTYPEPOINTLIST:
		primCount = numIndices;
		break;
	}
	drawIndexedPrimitive(primTypeMap[primType], 0,
	                     0, num3DVertices,
	                     0, primCount);
}

void
im3DEnd(void)
{
}

#endif
}
}
