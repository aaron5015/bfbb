#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define WITH_D3D
#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwrender.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwanim.h"
#include "../rwengine.h"
#include "../rwplugins.h"
#include "rwd3d.h"
#include "rwd3d9.h"

namespace rw {
namespace d3d9 {
using namespace d3d;

#if !defined(RW_D3D_ANY)
void skinInstanceCB(Geometry *geo, InstanceDataHeader *header, bool32 reinstance) {}
void skinRenderCB(Atomic *atomic, InstanceDataHeader *header) {}
#else


// Shared with the combined skin+matfx pipeline in d3d9skinmatfx.cpp, which
// falls back to these for the meshes of a skinned atomic whose material has no
// effect on it. They agree on where the bone matrices live, so the fallback is
// a shader swap and nothing else.
void *skin_amb_VS;
void *skin_amb_dir_VS;
void *skin_all_VS;
void *skin_pp_VS;
void *skin_outline_VS;

#define NUMDECLELT 14

void
skinInstanceCB(Geometry *geo, InstanceDataHeader *header, bool32 reinstance)
{
	int i = 0;
	VertexElement dcl[NUMDECLELT];
	VertexStream *s = &header->vertexStream[0];

	bool isPrelit = (geo->flags & Geometry::PRELIT) != 0;
	bool hasNormals = (geo->flags & Geometry::NORMALS) != 0;

	// TODO: support both vertex buffers

	if(!reinstance){
		// Create declarations and buffers only the first time

		assert(s->vertexBuffer == nil);
		s->offset = 0;
		s->managed = 1;
		s->geometryFlags = 0;
		s->dynamicLock = 0;

		dcl[i].stream = 0;
		dcl[i].offset = 0;
		dcl[i].type = D3DDECLTYPE_FLOAT3;
		dcl[i].method = D3DDECLMETHOD_DEFAULT;
		dcl[i].usage = D3DDECLUSAGE_POSITION;
		dcl[i].usageIndex = 0;
		i++;
		uint16 stride = 12;
		s->geometryFlags |= 0x2;

		if(isPrelit){
			dcl[i].stream = 0;
			dcl[i].offset = stride;
			dcl[i].type = D3DDECLTYPE_D3DCOLOR;
			dcl[i].method = D3DDECLMETHOD_DEFAULT;
			dcl[i].usage = D3DDECLUSAGE_COLOR;
			dcl[i].usageIndex = 0;
			i++;
			s->geometryFlags |= 0x8;
			stride += 4;
		}

		for(int32 n = 0; n < geo->numTexCoordSets; n++){
			dcl[i].stream = 0;
			dcl[i].offset = stride;
			dcl[i].type = D3DDECLTYPE_FLOAT2;
			dcl[i].method = D3DDECLMETHOD_DEFAULT;
			dcl[i].usage = D3DDECLUSAGE_TEXCOORD;
			dcl[i].usageIndex = (uint8)n;
			i++;
			s->geometryFlags |= 0x10 << n;
			stride += 8;
		}

		if(hasNormals){
			dcl[i].stream = 0;
			dcl[i].offset = stride;
			dcl[i].type = D3DDECLTYPE_FLOAT3;
			dcl[i].method = D3DDECLMETHOD_DEFAULT;
			dcl[i].usage = D3DDECLUSAGE_NORMAL;
			dcl[i].usageIndex = 0;
			i++;
			s->geometryFlags |= 0x4;
			stride += 12;
		}

		dcl[i].stream = 0;
		dcl[i].offset = stride;
		dcl[i].type = D3DDECLTYPE_FLOAT4;
		dcl[i].method = D3DDECLMETHOD_DEFAULT;
		dcl[i].usage = D3DDECLUSAGE_BLENDWEIGHT;
		dcl[i].usageIndex = 0;
		i++;
		stride += 16;

		dcl[i].stream = 0;
		dcl[i].offset = stride;
		dcl[i].type = D3DDECLTYPE_UBYTE4;
		dcl[i].method = D3DDECLMETHOD_DEFAULT;
		dcl[i].usage = D3DDECLUSAGE_BLENDINDICES;
		dcl[i].usageIndex = 0;
		i++;
		stride += 4;

		// We expect some attributes to always be there, use the constant buffer as fallback
		if(!hasNormals){
			// As in d3d9.cpp: a lighting vertex shader reads NORMAL whether
			// the geometry carries one or not.
			dcl[i].stream = 2;
			dcl[i].offset = offsetof(VertexConstantData, normal);
			dcl[i].type = D3DDECLTYPE_FLOAT3;
			dcl[i].method = D3DDECLMETHOD_DEFAULT;
			dcl[i].usage = D3DDECLUSAGE_NORMAL;
			dcl[i].usageIndex = 0;
			i++;
		}
		if(!isPrelit){
			dcl[i].stream = 2;
			dcl[i].offset = offsetof(VertexConstantData, color);
			dcl[i].type = D3DDECLTYPE_D3DCOLOR;
			dcl[i].method = D3DDECLMETHOD_DEFAULT;
			dcl[i].usage = D3DDECLUSAGE_COLOR;
			dcl[i].usageIndex = 0;
			i++;
		}
		if(geo->numTexCoordSets == 0){
			dcl[i].stream = 2;
			dcl[i].offset = offsetof(VertexConstantData, texCoors[0]);
			dcl[i].type = D3DDECLTYPE_FLOAT2;
			dcl[i].method = D3DDECLMETHOD_DEFAULT;
			dcl[i].usage = D3DDECLUSAGE_TEXCOORD;
			dcl[i].usageIndex = 0;
			i++;
		}

		dcl[i] = D3DDECL_END();
		s->stride = stride;

		assert(header->vertexDeclaration == nil);
		header->vertexDeclaration = createVertexDeclaration((VertexElement*)dcl);

		assert(s->vertexBuffer == nil);
		s->vertexBuffer = createVertexBuffer(header->totalNumVertex*s->stride, 0, false);
	}else
		getDeclaration(header->vertexDeclaration, dcl);

	Skin *skin = Skin::get(geo);
	uint8 *verts = lockVertices(s->vertexBuffer, 0, 0, D3DLOCK_NOSYSLOCK);

	// Instance vertices
	if(!reinstance || geo->lockedSinceInst&Geometry::LOCKVERTICES){
		for(i = 0; dcl[i].usage != D3DDECLUSAGE_POSITION || dcl[i].usageIndex != 0; i++)
			;
		instV3d(vertFormatMap[dcl[i].type], verts + dcl[i].offset,
			geo->morphTargets[0].vertices,
			header->totalNumVertex,
			header->vertexStream[dcl[i].stream].stride);
	}

	// Instance prelight colors
	if(isPrelit && (!reinstance || geo->lockedSinceInst&Geometry::LOCKPRELIGHT)){
		for(i = 0; dcl[i].usage != D3DDECLUSAGE_COLOR || dcl[i].usageIndex != 0; i++)
			;
		InstanceData *inst = header->inst;
		uint32 n = header->numMeshes;
		while(n--){
			uint32 stride = header->vertexStream[dcl[i].stream].stride;
			inst->vertexAlpha = instColor(vertFormatMap[dcl[i].type],
				verts + dcl[i].offset + stride*inst->minVert,
				geo->colors + inst->minVert,
				inst->numVertices,
				stride);
			inst++;
		}
	}

	// Instance tex coords
	for(int32 n = 0; n < geo->numTexCoordSets; n++){
		if(!reinstance || geo->lockedSinceInst&(Geometry::LOCKTEXCOORDS<<n)){
			for(i = 0; dcl[i].usage != D3DDECLUSAGE_TEXCOORD || dcl[i].usageIndex != n; i++)
				;
			instTexCoords(vertFormatMap[dcl[i].type], verts + dcl[i].offset,
				geo->texCoords[n],
				header->totalNumVertex,
				header->vertexStream[dcl[i].stream].stride);
		}
	}

	// Instance normals
	if(hasNormals && (!reinstance || geo->lockedSinceInst&Geometry::LOCKNORMALS)){
		for(i = 0; dcl[i].usage != D3DDECLUSAGE_NORMAL || dcl[i].usageIndex != 0; i++)
			;
		instV3d(vertFormatMap[dcl[i].type], verts + dcl[i].offset,
			geo->morphTargets[0].normals,
			header->totalNumVertex,
			header->vertexStream[dcl[i].stream].stride);
	}

	// Instance skin weights
	if(!reinstance){
		for(i = 0; dcl[i].usage != D3DDECLUSAGE_BLENDWEIGHT || dcl[i].usageIndex != 0; i++)
			;
		instV4d(vertFormatMap[dcl[i].type], verts + dcl[i].offset,
			(V4d*)skin->weights,
			header->totalNumVertex,
			header->vertexStream[dcl[i].stream].stride);
	}

	// Instance skin indices
	if(!reinstance){
		for(i = 0; dcl[i].usage != D3DDECLUSAGE_BLENDINDICES || dcl[i].usageIndex != 0; i++)
			;
		// not really colors of course but what the heck
		instColor(vertFormatMap[dcl[i].type], verts + dcl[i].offset,
			  (RGBA*)skin->indices,
			  header->totalNumVertex,
			  header->vertexStream[dcl[i].stream].stride);
	}

	unlockVertices(s->vertexBuffer);
}

enum
{
	VSLOC_boneMatrices = VSLOC_afterLights
};

static float skinMatrices[64*16];

// The bone transforms, in the atomic's object space, as ordinary rw matrices.
//
// Split out of uploadSkinMatrices because the fixed-function path skins on the
// CPU and needs the same matrices in a form it can multiply a vertex by --
// the constants that go to the vertex shader are these, transposed.
//
// Returns how many were written, which is skin->numBones and never more than
// MAXNUMSKINBONES.
int32
computeSkinMatrices(Atomic *a, Matrix *out)
{
	int i;
	Skin *skin = Skin::get(a->geometry);
	HAnimHierarchy *hier = Skin::getHierarchy(a);
	int32 numBones = skin->numBones;
	if(numBones > MAXNUMSKINBONES)
		numBones = MAXNUMSKINBONES;

	if(hier){
		Matrix *invMats = (Matrix*)skin->inverseMatrices;
		Matrix tmp;

		assert(skin->numBones == hier->numNodes);
		if(hier->flags & HAnimHierarchy::LOCALSPACEMATRICES){
			for(i = 0; i < numBones; i++){
				invMats[i].flags = 0;
				Matrix::mult(&out[i], &invMats[i], &hier->matrices[i]);
			}
		}else{
			Matrix invAtmMat;
			Matrix::invert(&invAtmMat, a->getFrame()->getLTM());
			for(i = 0; i < numBones; i++){
				invMats[i].flags = 0;
				Matrix::mult(&tmp, &hier->matrices[i], &invAtmMat);
				Matrix::mult(&out[i], &invMats[i], &tmp);
			}
		}
	}else{
		for(i = 0; i < numBones; i++)
			out[i].setIdentity();
	}
	return numBones;
}

void
uploadSkinMatrices(Atomic *a)
{
	int i;
	Skin *skin = Skin::get(a->geometry);
	float *m = skinMatrices;
	Matrix bones[MAXNUMSKINBONES];
	int32 numBones = computeSkinMatrices(a, bones);

	for(i = 0; i < numBones; i++){
		RawMatrix::transpose((RawMatrix*)m, (RawMatrix*)&bones[i]);
		m += 12;
	}

	// **The count that was written, not the count the model claims.**
	// computeSkinMatrices stops at MAXNUMSKINBONES; uploading skin->numBones
	// instead sends whatever follows in the array, and past 64 bones it runs off
	// the end of the register range the bones own -- straight over the outline
	// constants, the eye position and the hull's direction at c233 to c237.
	d3d::setVertexShaderConstantF(VSLOC_boneMatrices, skinMatrices, numBones*3);
}

void
skinRenderCB(Atomic *atomic, InstanceDataHeader *header)
{
	int vsBits;
	uint32 flags = atomic->geometry->flags;
	setStreamSource(0, header->vertexStream[0].vertexBuffer,
	                           0, header->vertexStream[0].stride);
	setIndices(header->indexBuffer);
	setVertexDeclaration(header->vertexDeclaration);

	vsBits = lightingCB_Shader(atomic);
	uploadMatrices(atomic->getFrame()->getLTM());

	uploadSkinMatrices(atomic);

	// Pick a shader. Same rule as the default pipeline in d3d9render.cpp:
	// per-pixel replaces the directional-only case and nothing else.
	bool32 perPixel = getPerPixelLighting() &&
	                  (vsBits & VSLIGHT_MASK) == VSLIGHT_DIRECT;

	// As in d3d9render.cpp: the cel look stands where per-pixel does and uses
	// the same vertex shader.
	bool32 toon = getToonShading() && (vsBits & VSLIGHT_MASK) != 0;

	if(toon)
		perPixel = 1;

	uploadToonConstants();

	// The hull, before the model, so the model is drawn over the middle of it
	// and only the band past the silhouette survives. Front faces culled:
	// what is left of an inflated copy once the faces pointing at the camera
	// are gone is its far side, which the real model then covers except at the
	// edge.
	int32 outline = getOutlineMode();

	if(outline != OUTLINE_NONE){
		uploadOutlineConstants();
		setVertexShader(skin_outline_VS);
		setPixelShader(outline_PS);
		// **Put back what was standing, not CULLBACK.** The application decides
		// whether a model is drawn two-sided, and a hull that restores CULLBACK
		// takes that away from the model's own pass: half of a shiny pickup, which
		// the game draws with no culling at all, simply disappeared.
		uint32 outlineCull = GetRenderState(CULLMODE);

		SetRenderState(CULLMODE, getOutlineInverted() ? CULLBACK : CULLFRONT);

		InstanceData *oinst = header->inst;

		for(uint32 i = 0; i < header->numMeshes; i++){
			if(outlineTakesMesh(header, oinst)){
				// The surface's own material, so the ink is drawn at the
				// surface's alpha. d3d9render.cpp says why it is not already
				// standing.
				setMaterial(flags, oinst->material->color, oinst->material->surfaceProps);
				d3d::setPipelineVertexAlpha(oinst->vertexAlpha ||
				                            oinst->material->color.alpha != 255);
				d3d::setTexture(0, oinst->material->texture);
				drawInst(header, oinst);
			}

			oinst++;
		}

		SetRenderState(CULLMODE, outlineCull);
	}

	if((vsBits & VSLIGHT_MASK) == 0)
		setVertexShader(skin_amb_VS);
	else if(perPixel)
		setVertexShader(skin_pp_VS);
	else if((vsBits & VSLIGHT_MASK) == VSLIGHT_DIRECT)
		setVertexShader(skin_amb_dir_VS);
	else
		setVertexShader(skin_all_VS);

	InstanceData *inst = header->inst;
	for(uint32 i = 0; i < header->numMeshes; i++){
		Material *m = inst->material;

		d3d::setPipelineVertexAlpha(inst->vertexAlpha || m->color.alpha != 255);

		setMaterial(flags, m->color, m->surfaceProps);

		if(inst->material->texture){
			d3d::setTexture(0, m->texture);
			setPixelShader(toon ? default_tex_toon_PS :
			               perPixel ? default_tex_pp_PS : default_tex_PS);
		}else
			setPixelShader(toon ? default_toon_PS :
			               perPixel ? default_pp_PS : default_PS);

		drawInst(header, inst);
		inst++;
	}
}


#ifdef RW_D3D9
namespace sm2 {
#include "shaders/skin_amb_VS.h"
#include "shaders/skin_amb_dir_VS.h"
#include "shaders/skin_all_VS.h"
#include "shaders/skin_pp_VS.h"
#include "shaders/skin_outline_VS.h"
}
#endif
#ifdef RW_D3D11
namespace sm4 {
#include "shaders11/skin_amb_VS.h"
#include "shaders11/skin_amb_dir_VS.h"
#include "shaders11/skin_all_VS.h"
#include "shaders11/skin_pp_VS.h"
#include "shaders11/skin_outline_VS.h"
}
#endif
#ifdef RW_VULKAN
namespace spv {
#include "shadersvk/skin_amb_VS.h"
#include "shadersvk/skin_amb_dir_VS.h"
#include "shadersvk/skin_all_VS.h"
#include "shadersvk/skin_pp_VS.h"
#include "shadersvk/skin_outline_VS.h"
}
#endif

void
createSkinShaders(void)
{
	skin_amb_VS = createVertexShader(RWD3D_SHADER(skin_amb_VS));
	assert(skin_amb_VS);
	skin_amb_dir_VS = createVertexShader(RWD3D_SHADER(skin_amb_dir_VS));
	assert(skin_amb_dir_VS);
	// Skinning takes a lot of instructions....lighting may be not possible
	// TODO: should do something about this
	skin_all_VS = createVertexShader(RWD3D_SHADER(skin_all_VS));
//	assert(skin_all_VS);
	// This one has room the note above worries about: it does no lighting at
	// all, only carrying the skinned normal out to the pixel shader.
	skin_pp_VS = createVertexShader(RWD3D_SHADER(skin_pp_VS));
	assert(skin_pp_VS);
	skin_outline_VS = createVertexShader(RWD3D_SHADER(skin_outline_VS));
	assert(skin_outline_VS);
}

void
destroySkinShaders(void)
{
	destroyVertexShader(skin_amb_VS);
	skin_amb_VS = nil;

	destroyVertexShader(skin_amb_dir_VS);
	skin_amb_dir_VS = nil;

	if(skin_all_VS){
		destroyVertexShader(skin_all_VS);
		skin_all_VS = nil;
	}

	destroyVertexShader(skin_pp_VS);
	skin_pp_VS = nil;

	destroyVertexShader(skin_outline_VS);
	skin_outline_VS = nil;
}

#endif

static void*
skinOpen(void *o, int32, int32)
{
	// Only the platform that is RUNNING. Engine::start constructs every
	// platform's driver plugins, and a build may carry several backends -- so
	// without this a D3D9 run would build GL3's pipelines and compile their
	// shaders with no GL context, and a GL3 run would do the same to D3D's
	// with no device. See gl3.cpp's driverOpen.
	if(rw::platform != PLATFORM_D3D9)
		return o;

#if defined(RW_D3D_ANY)
	// Not under fixed function: the CPU skinner needs a vertex declaration and
	// a dynamic buffer, and the device it runs on may have no shader unit to
	// compile these for.
	if(getFixedFunction())
		ffOpenSkin();
	else{
		createSkinShaders();
		createSkinMatFXShaders();
	}
#endif

	skinGlobals.pipelines[PLATFORM_D3D9] = makeSkinPipeline();
	// The combined skin+matfx pipeline belongs to the skin plugin, not the
	// matfx one: it is what SKINTYPEMATFX selects, and a platform that does not
	// register it here silently falls back to plain skinning.
	skinGlobals.matfxPipelines[PLATFORM_D3D9] = makeSkinMatFXPipeline();
	return o;
}

static void*
skinClose(void *o, int32, int32)
{
	// See this file's other half; the pipelines were never built.
	if(rw::platform != PLATFORM_D3D9)
		return o;

#if defined(RW_D3D_ANY)
	if(getFixedFunction())
		ffCloseSkin();
	else{
		destroySkinShaders();
		destroySkinMatFXShaders();
	}
#endif

	((ObjPipeline*)skinGlobals.pipelines[PLATFORM_D3D9])->destroy();
	skinGlobals.pipelines[PLATFORM_D3D9] = nil;
	((ObjPipeline*)skinGlobals.matfxPipelines[PLATFORM_D3D9])->destroy();
	skinGlobals.matfxPipelines[PLATFORM_D3D9] = nil;
	return o;
}

void
initSkin(void)
{
	Driver::registerPlugin(PLATFORM_D3D9, 0, ID_SKIN,
	                       skinOpen, skinClose);
}

ObjPipeline*
makeSkinPipeline(void)
{
	ObjPipeline *pipe = ObjPipeline::create();
	pipe->instanceCB = skinInstanceCB;
	pipe->uninstanceCB = nil;
	pipe->renderCB = getFixedFunction() ? skinRenderCB_Fix : skinRenderCB;
	pipe->pluginID = ID_SKIN;
	pipe->pluginData = 1;
	return pipe;
}

}
}
