#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define WITH_D3D
#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwanim.h"
#include "../rwengine.h"
#include "../rwrender.h"
#include "../rwplugins.h"
#include "rwd3d.h"
#include "rwd3d9.h"

namespace rw {
namespace d3d9 {
using namespace d3d;

#if !defined(RW_D3D_ANY)
void matfxRenderCB_Shader(Atomic *atomic, InstanceDataHeader *header) {}
#else

static void *matfx_env_amb_VS;
static void *matfx_env_amb_dir_VS;
static void *matfx_env_all_VS;
// Shared with the combined skin+matfx pipeline in d3d9skinmatfx.cpp: the env
// map is resolved entirely in the pixel shader, so the two pipelines differ
// only in how their vertex shaders get to a normal and can use one of these.
void *matfx_env_PS;
void *matfx_env_tex_PS;

enum
{
	VSLOC_texMat = VSLOC_afterLights,
	VSLOC_colorClamp = VSLOC_texMat + 4,
	VSLOC_envColor,

	PSLOC_shininess = 1,
};

void
matfxRender_Default(InstanceDataHeader *header, InstanceData *inst, int32 lightBits)
{
	Material *m = inst->material;

	// **The per-pixel and cel paths, which this pipeline never offered.**
	//
	// A material effect is a property of a surface, not a reason to light it
	// differently -- but matfx only ever reached for the plain shaders, so
	// anything with an environment map silently dropped out of per-pixel
	// lighting and, once it existed, out of the cel look. The robots are the
	// case that shows it: NPCs like any other, tagged for an outline like any
	// other, and smooth-shaded because they are shiny.
	bool32 perPixel = getPerPixelLighting() &&
	                  (lightBits & VSLIGHT_MASK) == VSLIGHT_DIRECT;
	bool32 toon = getToonShading() && (lightBits & VSLIGHT_MASK) != 0;

	if(toon)
		perPixel = 1;

	uploadToonConstants();

	// Pick a shader
	if((lightBits & VSLIGHT_MASK) == 0)
		setVertexShader(default_amb_VS);
	else if(perPixel)
		setVertexShader(default_pp_VS);
	else if((lightBits & VSLIGHT_MASK) == VSLIGHT_DIRECT)
		setVertexShader(default_amb_dir_VS);
	else
		setVertexShader(default_all_VS);

	d3d::setPipelineVertexAlpha(inst->vertexAlpha || m->color.alpha != 255);

	if(inst->material->texture){
		d3d::setTexture(0, m->texture);
		setPixelShader(toon ? default_tex_toon_PS :
		               perPixel ? default_tex_pp_PS : default_tex_PS);
	}else
		setPixelShader(toon ? default_toon_PS :
		               perPixel ? default_pp_PS : default_PS);

	drawInst(header, inst);
}

// WHERE an env-map shader's constants go. WHAT they are is MatFX::setupEnv's,
// because none of it is this device's decision.
//
// vslocBase is the register the 4x4 texture matrix goes to; the clamp and the
// env colour follow it at the same fixed offsets in every env shader, so one
// base places all three. It is VSLOC_texMat for every shader in this file, but
// the combined skin+matfx shader needs its bone matrices at VSLOC_afterLights
// and moves the env constants aside.
void
uploadEnvMapState(Texture *envTex, MatFXEnvState *es, int32 vslocBase)
{
	d3d::setTexture(1, envTex);
	d3d::setVertexShaderConstantF(vslocBase, (float*)&es->texMatrix, 4);

	struct  {
		float shininess;
		float disableFBA;
		float unused[2];
	} fxparams;
	fxparams.shininess = es->shininess;
	fxparams.disableFBA = es->disableFBA;
	d3d::setPixelShaderConstantF(PSLOC_shininess, (float*)&fxparams, 1);

	d3d::setVertexShaderConstantF(vslocBase + 4, (float*)&es->colorClamp, 1);
	d3d::setVertexShaderConstantF(vslocBase + 5, (float*)&es->color, 1);
}

void
matfxRender_EnvMap(InstanceDataHeader *header, InstanceData *inst, int32 lightBits, MatFX::Env *env)
{
	Material *m = inst->material;

	MatFXEnvState es;
	if(!MatFX::setupEnv(&es, m, env)){
		matfxRender_Default(header, inst, lightBits);
		return;
	}

	uploadEnvMapState(env->tex, &es, VSLOC_texMat);

	SetRenderState(SRCBLEND, BLENDONE);

	// Pick a shader
	if((lightBits & VSLIGHT_MASK) == 0)
		setVertexShader(matfx_env_amb_VS);
	else if((lightBits & VSLIGHT_MASK) == VSLIGHT_DIRECT)
		setVertexShader(matfx_env_amb_dir_VS);
	else
		setVertexShader(matfx_env_all_VS);

	bool32 texAlpha = GETD3DRASTEREXT(env->tex->raster)->hasAlpha;

	if(inst->material->texture){
		d3d::setTexture(0, m->texture);
		setPixelShader(matfx_env_tex_PS);
	}else
		setPixelShader(matfx_env_PS);

	d3d::setPipelineVertexAlpha(texAlpha || inst->vertexAlpha || m->color.alpha != 255);

	drawInst(header, inst);

	SetRenderState(SRCBLEND, BLENDSRCALPHA);
}

void
matfxRenderCB_Shader(Atomic *atomic, InstanceDataHeader *header)
{
	int vsBits;
	uint32 flags = atomic->geometry->flags;
	setStreamSource(0, header->vertexStream[0].vertexBuffer,
	                           0, header->vertexStream[0].stride);
	setIndices(header->indexBuffer);
	setVertexDeclaration(header->vertexDeclaration);

	vsBits = lightingCB_Shader(atomic);
	uploadMatrices(atomic->getFrame()->getLTM());

	bool normals = !!(atomic->geometry->flags & Geometry::NORMALS);

	// **The hull, which this pipeline never drew.** A material effect says how
	// a surface is shaded and nothing about whether a line goes round it, but
	// only the plain and the skinned pipelines ever drew one, so anything with
	// an environment map came out uninked. The shiny pickups are the case that
	// shows it: solid objects, tagged for an outline like any other, and the
	// one class of thing in a level with no line on it.
	//
	// The same pass as d3d9render.cpp's, for the same reasons; that is where
	// the whole of it is explained.
	if(getOutlineMode() != OUTLINE_NONE){

		uploadOutlineConstants();
		setVertexShader(outline_VS);
		setPixelShader(outline_PS);

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

	InstanceData *inst = header->inst;
	for(uint32 i = 0; i < header->numMeshes; i++){
		Material *m = inst->material;

		setMaterial(flags, m->color, m->surfaceProps);

		MatFX *matfx = MatFX::get(m);
		if(matfx == nil)
			matfxRender_Default(header, inst, vsBits);
		else switch(matfx->type){
		case MatFX::ENVMAP:
			if(normals)
				matfxRender_EnvMap(header, inst, vsBits, &matfx->fx[0].env);
			else
				matfxRender_Default(header, inst, vsBits);
			break;
		case MatFX::NOTHING:
		case MatFX::BUMPMAP:
		case MatFX::BUMPENVMAP:
		case MatFX::DUAL:
		case MatFX::UVTRANSFORM:
		case MatFX::DUALUVTRANSFORM:
			// not supported yet
			matfxRender_Default(header, inst, vsBits);
			break;			
		}

		inst++;
	}
	d3d::setTexture(1, nil);
}


#ifdef RW_D3D9
namespace sm2 {
#include "shaders/matfx_env_amb_VS.h"
#include "shaders/matfx_env_amb_dir_VS.h"
#include "shaders/matfx_env_all_VS.h"
#include "shaders/matfx_env_PS.h"
#include "shaders/matfx_env_tex_PS.h"
}
#endif
#ifdef RW_D3D11
namespace sm4 {
#include "shaders11/matfx_env_amb_VS.h"
#include "shaders11/matfx_env_amb_dir_VS.h"
#include "shaders11/matfx_env_all_VS.h"
#include "shaders11/matfx_env_PS.h"
#include "shaders11/matfx_env_tex_PS.h"
}
#endif
#ifdef RW_VULKAN
namespace spv {
#include "shadersvk/matfx_env_amb_VS.h"
#include "shadersvk/matfx_env_amb_dir_VS.h"
#include "shadersvk/matfx_env_all_VS.h"
#include "shadersvk/matfx_env_PS.h"
#include "shadersvk/matfx_env_tex_PS.h"
}
#endif

void
createMatFXShaders(void)
{
	matfx_env_amb_VS = createVertexShader(RWD3D_SHADER(matfx_env_amb_VS));
	assert(matfx_env_amb_VS);
	matfx_env_amb_dir_VS = createVertexShader(RWD3D_SHADER(matfx_env_amb_dir_VS));
	assert(matfx_env_amb_dir_VS);
	matfx_env_all_VS = createVertexShader(RWD3D_SHADER(matfx_env_all_VS));
	assert(matfx_env_all_VS);

	matfx_env_PS = createPixelShader(RWD3D_SHADER(matfx_env_PS));
	assert(matfx_env_PS);
	matfx_env_tex_PS = createPixelShader(RWD3D_SHADER(matfx_env_tex_PS));
	assert(matfx_env_tex_PS);
}

void
destroyMatFXShaders(void)
{
	destroyVertexShader(matfx_env_amb_VS);
	matfx_env_amb_VS = nil;

	destroyVertexShader(matfx_env_amb_dir_VS);
	matfx_env_amb_dir_VS = nil;

	destroyVertexShader(matfx_env_all_VS);
	matfx_env_all_VS = nil;


	destroyPixelShader(matfx_env_PS);
	matfx_env_PS = nil;

	destroyPixelShader(matfx_env_tex_PS);
	matfx_env_tex_PS = nil;
}

#endif

static void*
matfxOpen(void *o, int32, int32)
{
	// Only the platform that is RUNNING. Engine::start constructs every
	// platform's driver plugins, and a build may carry several backends -- so
	// without this a D3D9 run would build GL3's pipelines and compile their
	// shaders with no GL context, and a GL3 run would do the same to D3D's
	// with no device. See gl3.cpp's driverOpen.
	if(rw::platform != PLATFORM_D3D9)
		return o;

#if defined(RW_D3D_ANY)
	if(!getFixedFunction())
		createMatFXShaders();
#endif

	matFXGlobals.pipelines[PLATFORM_D3D9] = makeMatFXPipeline();
	return o;
}

static void*
matfxClose(void *o, int32, int32)
{
	// See this file's other half; the pipelines were never built.
	if(rw::platform != PLATFORM_D3D9)
		return o;

#if defined(RW_D3D_ANY)
	if(!getFixedFunction())
		destroyMatFXShaders();
#endif

	((ObjPipeline*)matFXGlobals.pipelines[PLATFORM_D3D9])->destroy();
	matFXGlobals.pipelines[PLATFORM_D3D9] = nil;
	return o;
}

void
initMatFX(void)
{
	Driver::registerPlugin(PLATFORM_D3D9, 0, ID_MATFX,
	                       matfxOpen, matfxClose);
}

ObjPipeline*
makeMatFXPipeline(void)
{
	ObjPipeline *pipe = ObjPipeline::create();
	pipe->instanceCB = defaultInstanceCB;
	pipe->uninstanceCB = defaultUninstanceCB;
	// The default fixed-function render, which draws the base material and
	// nothing else. The environment map needs a second texture stage with
	// D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR; it is not written yet.
	pipe->renderCB = getFixedFunction() ? defaultRenderCB_Fix : matfxRenderCB_Shader;
	pipe->pluginID = ID_MATFX;
	pipe->pluginData = 0;
	return pipe;
}

}
}
