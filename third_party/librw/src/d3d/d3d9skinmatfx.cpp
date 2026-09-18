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

// The pipeline RenderWare selects with rpSKINTYPEMATFX: bone skinning AND a
// material effect on the same atomic. Neither of the two pipelines it is made
// of can stand in for it -- the skin pipeline draws the deformed model with no
// effect, and the matfx pipeline draws the effect on an undeformed one -- and
// every environment-mapped character in the game needs both. The env map in
// particular is meaningless without the first: its texture coordinate comes
// off the vertex normal, so a normal still in its bind pose reflects whatever
// the model was authored facing rather than what it is facing now.
//
// It is deliberately assembled out of the other two rather than rewritten:
// instancing is skinInstanceCB unchanged, the env state is d3d9matfx.cpp's
// uploadEnvMapState, the pixel shaders are matfx's, and the fallback for a
// mesh with no effect on it is d3d9skin.cpp's own vertex shaders. Only the
// env-mapped vertex shader is new, because only it has to do both jobs at once.

namespace rw {
namespace d3d9 {
using namespace d3d;

#if !defined(RW_D3D_ANY)
void skinMatfxRenderCB(Atomic *atomic, InstanceDataHeader *header) {}
void createSkinMatFXShaders(void) {}
void destroySkinMatFXShaders(void) {}
#else

static void *skin_matfx_env_amb_VS;
static void *skin_matfx_env_amb_dir_VS;
static void *skin_matfx_env_all_VS;

enum
{
	// 64 bones at three registers each leave nowhere after the lights for the
	// env constants, so they go where the plain skinning shader has them and
	// the env constants move to the end of the constant file. Keeping the
	// bones here is what lets uploadSkinMatrices be the same call for both
	// skinning pipelines, and what lets a mesh with no effect on it fall back
	// to a plain skinning shader without re-uploading anything.
	VSLOC_boneMatrices = VSLOC_afterLights,
	VSLOC_texMat = VSLOC_boneMatrices + 64*3,
	// uploadEnvMapState places the colour clamp and the env colour itself,
	// at VSLOC_texMat+4 and +5. Named here only to say what the shader's
	// c237 and c238 are.
	VSLOC_colorClamp = VSLOC_texMat + 4,
	VSLOC_envColor,
};

static void
skinMatfxRender_Default(InstanceDataHeader *header, InstanceData *inst, int32 lightBits)
{
	Material *m = inst->material;

	// Exactly what skinRenderCB draws. A skinned atomic stays on this pipeline
	// once MatFX::enableEffects has put it here, so most of the meshes it sees
	// have no effect on them at all -- including every mesh of every frame in
	// which the game has turned the effects off again.
	// The per-pixel and cel paths, which this pipeline never offered -- see
	// d3d9matfx.cpp, where the same gap let every robot out of the cel look
	// for being shiny.
	bool32 perPixel = getPerPixelLighting() &&
	                  (lightBits & VSLIGHT_MASK) == VSLIGHT_DIRECT;
	bool32 toon = getToonShading() && (lightBits & VSLIGHT_MASK) != 0;

	if(toon)
		perPixel = 1;

	uploadToonConstants();

	if((lightBits & VSLIGHT_MASK) == 0)
		setVertexShader(skin_amb_VS);
	else if(perPixel)
		setVertexShader(skin_pp_VS);
	else if((lightBits & VSLIGHT_MASK) == VSLIGHT_DIRECT)
		setVertexShader(skin_amb_dir_VS);
	else
		setVertexShader(skin_all_VS);

	d3d::setPipelineVertexAlpha(inst->vertexAlpha || m->color.alpha != 255);

	if(m->texture){
		d3d::setTexture(0, m->texture);
		setPixelShader(toon ? default_tex_toon_PS :
		               perPixel ? default_tex_pp_PS : default_tex_PS);
	}else
		setPixelShader(toon ? default_toon_PS :
		               perPixel ? default_pp_PS : default_PS);

	drawInst(header, inst);
}

static void
skinMatfxRender_EnvMap(InstanceDataHeader *header, InstanceData *inst, int32 lightBits, MatFX::Env *env)
{
	Material *m = inst->material;

	MatFXEnvState es;
	if(!MatFX::setupEnv(&es, m, env)){
		skinMatfxRender_Default(header, inst, lightBits);
		return;
	}

	uploadEnvMapState(env->tex, &es, VSLOC_texMat);

	SetRenderState(SRCBLEND, BLENDONE);

	if((lightBits & VSLIGHT_MASK) == 0)
		setVertexShader(skin_matfx_env_amb_VS);
	else if((lightBits & VSLIGHT_MASK) == VSLIGHT_DIRECT)
		setVertexShader(skin_matfx_env_amb_dir_VS);
	else
		setVertexShader(skin_matfx_env_all_VS);

	bool32 texAlpha = GETD3DRASTEREXT(env->tex->raster)->hasAlpha;

	if(m->texture){
		d3d::setTexture(0, m->texture);
		setPixelShader(matfx_env_tex_PS);
	}else
		setPixelShader(matfx_env_PS);

	d3d::setPipelineVertexAlpha(texAlpha || inst->vertexAlpha || m->color.alpha != 255);

	drawInst(header, inst);

	SetRenderState(SRCBLEND, BLENDSRCALPHA);
}

void
skinMatfxRenderCB(Atomic *atomic, InstanceDataHeader *header)
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

	// The hull, exactly as d3d9skin.cpp draws it. A character does not stop
	// being a character because one of his materials reflects.
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

	// Without normals there is nothing to reflect, so the env map cannot be
	// generated at all and the mesh falls back to plain skinning -- the same
	// decision matfxRenderCB_Shader makes for the same reason.
	bool normals = !!(flags & Geometry::NORMALS);

	InstanceData *inst = header->inst;
	for(uint32 i = 0; i < header->numMeshes; i++){
		Material *m = inst->material;

		setMaterial(flags, m->color, m->surfaceProps);

		MatFX *matfx = MatFX::get(m);
		if(matfx && matfx->type == MatFX::ENVMAP && normals)
			skinMatfxRender_EnvMap(header, inst, vsBits, &matfx->fx[0].env);
		else
			// Every other effect is unsupported here for the same reason it is
			// unsupported in the unskinned matfx pipeline: no shader for it.
			skinMatfxRender_Default(header, inst, vsBits);

		inst++;
	}
	d3d::setTexture(1, nil);
}


#ifdef RW_D3D9
namespace sm2 {
#include "shaders/skin_matfx_env_amb_VS.h"
#include "shaders/skin_matfx_env_amb_dir_VS.h"
#include "shaders/skin_matfx_env_all_VS.h"
}
#endif
#ifdef RW_D3D11
namespace sm4 {
#include "shaders11/skin_matfx_env_amb_VS.h"
#include "shaders11/skin_matfx_env_amb_dir_VS.h"
#include "shaders11/skin_matfx_env_all_VS.h"
}
#endif
#ifdef RW_VULKAN
namespace spv {
#include "shadersvk/skin_matfx_env_amb_VS.h"
#include "shadersvk/skin_matfx_env_amb_dir_VS.h"
#include "shadersvk/skin_matfx_env_all_VS.h"
}
#endif

void
createSkinMatFXShaders(void)
{
	skin_matfx_env_amb_VS = createVertexShader(RWD3D_SHADER(skin_matfx_env_amb_VS));
	assert(skin_matfx_env_amb_VS);
	skin_matfx_env_amb_dir_VS = createVertexShader(RWD3D_SHADER(skin_matfx_env_amb_dir_VS));
	assert(skin_matfx_env_amb_dir_VS);
	// As in d3d9skin.cpp: skinning is expensive enough in vertex shader
	// instructions that the fully lit variant is the one at risk of not
	// fitting, so it is not asserted. If it ever comes back nil the env pass
	// is skipped for that lighting setup rather than drawn wrong.
	skin_matfx_env_all_VS = createVertexShader(RWD3D_SHADER(skin_matfx_env_all_VS));
}

void
destroySkinMatFXShaders(void)
{
	destroyVertexShader(skin_matfx_env_amb_VS);
	skin_matfx_env_amb_VS = nil;

	destroyVertexShader(skin_matfx_env_amb_dir_VS);
	skin_matfx_env_amb_dir_VS = nil;

	if(skin_matfx_env_all_VS){
		destroyVertexShader(skin_matfx_env_all_VS);
		skin_matfx_env_all_VS = nil;
	}
}

#endif

ObjPipeline*
makeSkinMatFXPipeline(void)
{
	ObjPipeline *pipe = ObjPipeline::create();
	// The instancing callback is the skin one, and that is not an economy: the
	// instanced vertex buffer is cached on the GEOMETRY and is never rebuilt
	// because the atomic changed pipeline, so a skinned atomic that can be on
	// either skinning pipeline has to lay its vertices out the same way on
	// both or the one that did not instance them reads bones that are not there.
	pipe->instanceCB = skinInstanceCB;
	pipe->uninstanceCB = nil;
	// No environment map under fixed function: it needs a second texture stage
	// with a camera-space reflection texgen, which the plain skin path does not
	// set up. The base material draws, skinned; the shine does not.
	pipe->renderCB = getFixedFunction() ? skinRenderCB_Fix : skinMatfxRenderCB;
	// The plugin ID the pipeline is streamed out under. It is the skin plugin's
	// because this is a skinning pipeline; pluginData is the same 1 the plain
	// skinning pipeline writes, so a DFF written with an atomic on this reads
	// back as a skinned atomic on any RenderWare, which is what it is.
	pipe->pluginID = ID_SKIN;
	pipe->pluginData = 1;
	return pipe;
}

}
}
