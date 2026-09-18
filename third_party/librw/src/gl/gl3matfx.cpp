#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwrender.h"
#include "../rwengine.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwanim.h"
#include "../rwplugins.h"

#include "rwgl3.h"
#include "rwgl3shader.h"
#include "rwgl3plg.h"

#include "rwgl3impl.h"

namespace rw {
namespace gl3 {

#ifdef RW_OPENGL

static Shader *envShader, *envShader_noAT;
static Shader *envShader_fullLight, *envShader_fullLight_noAT;
static int32 u_texMatrix;
static int32 u_fxparams;
static int32 u_colorClamp;
static int32 u_envColor;

void
matfxDefaultRender(InstanceDataHeader *header, InstanceData *inst, int32 vsBits, uint32 flags)
{
	Material *m;
	m = inst->material;

	setMaterial(flags, m->color, m->surfaceProps);

	setTexture(0, m->texture);

	setPipelineVertexAlpha(inst->vertexAlpha || m->color.alpha != 0xFF);

	// **The per-pixel path, which this pipeline never offered.**
	//
	// A material effect is a property of a surface, not a reason to light it
	// differently -- but matfx only ever reached for the plain shaders, so
	// anything with an environment map silently dropped out of per-pixel
	// lighting and, once it existed, out of the cel look. The robots are the
	// case that shows it: they are NPCs like any other, they were being tagged
	// for an outline like any other, and they came out smooth-shaded because
	// they are shiny.
	if((vsBits & VSLIGHT_MASK) == 0){
		if(getAlphaTest())
			defaultShader->use();
		else
			defaultShader_noAT->use();
	}else if(getToonShading() ||
	         (getPerPixelLighting() && (vsBits & VSLIGHT_MASK) == VSLIGHT_DIRECT)){
		if(getAlphaTest())
			defaultShader_pp->use();
		else
			defaultShader_pp_noAT->use();
	}else{
		if(getAlphaTest())
			defaultShader_fullLight->use();
		else
			defaultShader_fullLight_noAT->use();
	}

	drawInst(header, inst);
}

// WHERE the environment pass's uniforms go. WHAT they are is
// MatFX::setupEnv's, because none of it is this device's decision -- and while
// it was, envMapModulateByAlpha reached D3D9 and not here.
//
// Shared by both pipelines that draw an environment pass: this one and the
// skinning one in gl3skinmatfx.cpp. The texture on stage 0 and the material
// stay the caller's, because those are the same in its non-env path.
void
uploadEnvMapState(Texture *envTex, MatFXEnvState *es)
{
	setTexture(1, envTex);

	setUniform(u_texMatrix, &es->texMatrix);

	float fxparams[4];
	fxparams[0] = es->shininess;
	fxparams[1] = es->disableFBA;
	fxparams[2] = fxparams[3] = 0.0f;
	setUniform(u_fxparams, fxparams);

	setUniform(u_colorClamp, &es->colorClamp);
	setUniform(u_envColor, &es->color);
}

void
matfxEnvRender(InstanceDataHeader *header, InstanceData *inst, int32 vsBits, uint32 flags, MatFX::Env *env)
{
	Material *m;
	m = inst->material;

	MatFXEnvState es;
	if(!MatFX::setupEnv(&es, m, env)){
		matfxDefaultRender(header, inst, vsBits, flags);
		return;
	}

	setTexture(0, m->texture);
	uploadEnvMapState(env->tex, &es);

	setMaterial(flags, m->color, m->surfaceProps);

	setPipelineVertexAlpha(1);
	rw::SetRenderState(SRCBLEND, BLENDONE);

	if((vsBits & VSLIGHT_MASK) == 0){
		if(getAlphaTest())
			envShader->use();
		else
			envShader_noAT->use();
	}else{
		if(getAlphaTest())
			envShader_fullLight->use();
		else
			envShader_fullLight_noAT->use();
	}

	drawInst(header, inst);

	rw::SetRenderState(SRCBLEND, BLENDSRCALPHA);
}

void
matfxRenderCB(Atomic *atomic, InstanceDataHeader *header)
{
	uint32 flags = atomic->geometry->flags;
	setWorldMatrix(atomic->getFrame()->getLTM());
	int32 vsBits = lightingCB(atomic);

	setupVertexInput(header);

	// The hull, which this pipeline never drew. d3d9matfx.cpp says why an
	// environment map is no reason to go uninked, and gl3render.cpp has the
	// pass itself.
	if(getOutlineMode() != OUTLINE_NONE){
		uint32 outlineCull = GetRenderState(CULLMODE);

		SetRenderState(CULLMODE, getOutlineInverted() ? CULLBACK : CULLFRONT);
		outlineShader->use();

		InstanceData *oinst = header->inst;
		int32 on = header->numMeshes;

		while(on--){
			if(outlineTakesMesh(header, oinst)){
				// The material as well as its texture, so the ink is drawn at
				// the surface's alpha. See gl3render.cpp.
				setMaterial(flags, oinst->material->color, oinst->material->surfaceProps);
				setPipelineVertexAlpha(oinst->vertexAlpha ||
				                       oinst->material->color.alpha != 0xFF);
				setTexture(0, oinst->material->texture);
				drawInst(header, oinst);
			}

			oinst++;
		}

		SetRenderState(CULLMODE, outlineCull);
	}

	InstanceData *inst = header->inst;
	int32 n = header->numMeshes;

	while(n--){
		MatFX *matfx = MatFX::get(inst->material);

		if(matfx == nil)
			matfxDefaultRender(header, inst, vsBits, flags);
		else switch(matfx->type){
		case MatFX::ENVMAP:
			matfxEnvRender(header, inst, vsBits, flags, &matfx->fx[0].env);
			break;
		default:
			matfxDefaultRender(header, inst, vsBits, flags);
			break;
		}
		inst++;
	}
	teardownVertexInput(header);
}

ObjPipeline*
makeMatFXPipeline(void)
{
	ObjPipeline *pipe = ObjPipeline::create();
	pipe->instanceCB = defaultInstanceCB;
	pipe->uninstanceCB = defaultUninstanceCB;
	pipe->renderCB = matfxRenderCB;
	// An environment map contributes nothing to a depth value.
	pipe->depthRenderCB = defaultRenderDepthCB;
	pipe->pluginID = ID_MATFX;
	pipe->pluginData = 0;
	return pipe;
}

static void*
matfxOpen(void *o, int32, int32)
{
	// Only the platform that is RUNNING. Engine::start constructs every
	// platform's driver plugins, and a build may carry several backends -- so
	// without this a D3D9 run would build GL3's pipelines and compile their
	// shaders with no GL context, and a GL3 run would do the same to D3D's
	// with no device. See gl3.cpp's driverOpen.
	if(rw::platform != PLATFORM_GL3)
		return o;

	matFXGlobals.pipelines[PLATFORM_GL3] = makeMatFXPipeline();

#include "shaders/matfx_gl.inc"
	const char *vs[] = { shaderDecl, header_vert_src, matfx_env_vert_src, nil };
	const char *vs_fullLight[] = { shaderDecl, "#define DIRECTIONALS\n#define POINTLIGHTS\n#define SPOTLIGHTS\n", header_vert_src, matfx_env_vert_src, nil };
	const char *fs[] = { shaderDecl, header_frag_src, matfx_env_frag_src, nil };
	const char *fs_noAT[] = { shaderDecl, "#define NO_ALPHATEST\n", header_frag_src, matfx_env_frag_src, nil };

	envShader = Shader::create(vs, fs);
	assert(envShader);
	envShader_noAT = Shader::create(vs, fs_noAT);
	assert(envShader_noAT);

	envShader_fullLight = Shader::create(vs_fullLight, fs);
	assert(envShader_fullLight);
	envShader_fullLight_noAT = Shader::create(vs_fullLight, fs_noAT);
	assert(envShader_fullLight_noAT);

	return o;
}

static void*
matfxClose(void *o, int32, int32)
{
	// See this file's other half; the pipelines were never built.
	if(rw::platform != PLATFORM_GL3)
		return o;

	((ObjPipeline*)matFXGlobals.pipelines[PLATFORM_GL3])->destroy();
	matFXGlobals.pipelines[PLATFORM_GL3] = nil;

	envShader->destroy();
	envShader = nil;
	envShader_noAT->destroy();
	envShader_noAT = nil;
	envShader_fullLight->destroy();
	envShader_fullLight = nil;
	envShader_fullLight_noAT->destroy();
	envShader_fullLight_noAT = nil;

	return o;
}

// Called from here and from the skin plugin's init, because the pipeline that
// does both jobs lives with the skinning one and either plugin can be the
// first -- or the only -- one an application registers. registerUniform hands
// back the id it already made when it is asked twice.
void
registerEnvUniforms(void)
{
	u_texMatrix = registerUniform("u_texMatrix", UNIFORM_MAT4);
	u_fxparams = registerUniform("u_fxparams", UNIFORM_VEC4);
	u_colorClamp = registerUniform("u_colorClamp", UNIFORM_VEC4);
	u_envColor = registerUniform("u_envColor", UNIFORM_VEC4);
}

void
initMatFX(void)
{
	registerEnvUniforms();

	Driver::registerPlugin(PLATFORM_GL3, 0, ID_MATFX,
	                       matfxOpen, matfxClose);
}

#else

void initMatFX(void) { }

#endif

}
}

