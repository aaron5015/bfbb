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

// Not static: gl3skinmatfx.cpp draws a mesh with no effect on it with exactly
// these, which is what keeps the combined pipeline an assembly of the two it
// stands between rather than a copy of either.
Shader *skinShader, *skinShader_noAT;
Shader *skinShader_fullLight, *skinShader_fullLight_noAT;
// Skinning with the lighting left to the fragment shader.
Shader *skinShader_pp, *skinShader_pp_noAT;
// A skinned caster. Shares depth.frag with the unskinned one -- only the
// vertex stage differs, and only because the bones do.
Shader *skinDepthShader;
Shader *skinDepthShader_tex;
static int32 u_boneMatrices;

void
skinInstanceCB(Geometry *geo, InstanceDataHeader *header, bool32 reinstance)
{
	AttribDesc *attribs, *a;

	bool isPrelit = !!(geo->flags & Geometry::PRELIT);
	bool hasNormals = !!(geo->flags & Geometry::NORMALS);

	if(!reinstance){
		AttribDesc tmpAttribs[14];
		uint32 stride;

		//
		// Create attribute descriptions
		//
		a = tmpAttribs;
		stride = 0;

		// Positions
		a->index = ATTRIB_POS;
		a->size = 3;
		a->type = GL_FLOAT;
		a->normalized = GL_FALSE;
		a->offset = stride;
		stride += 12;
		a++;

		// Normals
		// TODO: compress
		if(hasNormals){
			a->index = ATTRIB_NORMAL;
			a->size = 3;
			a->type = GL_FLOAT;
			a->normalized = GL_FALSE;
			a->offset = stride;
			stride += 12;
			a++;
		}

		// Prelighting
		if(isPrelit){
			a->index = ATTRIB_COLOR;
			a->size = 4;
			a->type = GL_UNSIGNED_BYTE;
			a->normalized = GL_TRUE;
			a->offset = stride;
			stride += 4;
			a++;
		}

		// Texture coordinates
		for(int32 n = 0; n < geo->numTexCoordSets; n++){
			a->index = ATTRIB_TEXCOORDS0+n;
			a->size = 2;
			a->type = GL_FLOAT;
			a->normalized = GL_FALSE;
			a->offset = stride;
			stride += 8;
			a++;
		}

		// Weights
		a->index = ATTRIB_WEIGHTS;
		a->size = 4;
		a->type = GL_FLOAT;
		a->normalized = GL_FALSE;
		a->offset = stride;
		stride += 16;
		a++;

		// Indices
		a->index = ATTRIB_INDICES;
		a->size = 4;
		a->type = GL_UNSIGNED_BYTE;
		a->normalized = GL_FALSE;
		a->offset = stride;
		stride += 4;
		a++;

		header->numAttribs = a - tmpAttribs;
		for(a = tmpAttribs; a != &tmpAttribs[header->numAttribs]; a++)
			a->stride = stride;
		header->attribDesc = rwNewT(AttribDesc, header->numAttribs, MEMDUR_EVENT | ID_GEOMETRY);
		memcpy(header->attribDesc, tmpAttribs,
		       header->numAttribs*sizeof(AttribDesc));

		//
		// Allocate vertex buffer
		//
		header->vertexBuffer = rwNewT(uint8, header->totalNumVertex*stride, MEMDUR_EVENT | ID_GEOMETRY);
		assert(header->vbo == 0);
		glGenBuffers(1, &header->vbo);
	}

	Skin *skin = Skin::get(geo);
	attribs = header->attribDesc;

	//
	// Fill vertex buffer
	//

	uint8 *verts = header->vertexBuffer;

	// Positions
	if(!reinstance || geo->lockedSinceInst&Geometry::LOCKVERTICES){
		for(a = attribs; a->index != ATTRIB_POS; a++)
			;
		instV3d(VERT_FLOAT3, verts + a->offset,
			geo->morphTargets[0].vertices,
			header->totalNumVertex, a->stride);
	}

	// Normals
	if(hasNormals && (!reinstance || geo->lockedSinceInst&Geometry::LOCKNORMALS)){
		for(a = attribs; a->index != ATTRIB_NORMAL; a++)
			;
		instV3d(VERT_FLOAT3, verts + a->offset,
			geo->morphTargets[0].normals,
			header->totalNumVertex, a->stride);
	}

	// Prelighting
	if(isPrelit && (!reinstance || geo->lockedSinceInst&Geometry::LOCKPRELIGHT)){
		for(a = attribs; a->index != ATTRIB_COLOR; a++)
			;
		instColor(VERT_RGBA, verts + a->offset,
			  geo->colors,
			  header->totalNumVertex, a->stride);
	}

	// Texture coordinates
	for(int32 n = 0; n < geo->numTexCoordSets; n++){
		if(!reinstance || geo->lockedSinceInst&(Geometry::LOCKTEXCOORDS<<n)){
			for(a = attribs; a->index != ATTRIB_TEXCOORDS0+n; a++)
				;
			instTexCoords(VERT_FLOAT2, verts + a->offset,
				geo->texCoords[n],
				header->totalNumVertex, a->stride);
		}
	}

	// Weights
	if(!reinstance){
		for(a = attribs; a->index != ATTRIB_WEIGHTS; a++)
			;
		float *w = skin->weights;
		instV4d(VERT_FLOAT4, verts + a->offset,
			(V4d*)w,
			header->totalNumVertex, a->stride);
	}

	// Indices
	if(!reinstance){
		for(a = attribs; a->index != ATTRIB_INDICES; a++)
			;
		// not really colors of course but what the heck
		instColor(VERT_RGBA, verts + a->offset,
			  (RGBA*)skin->indices,
			  header->totalNumVertex, a->stride);
	}

#ifdef RW_GL_USE_VAOS
	glBindVertexArray(header->vao);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, header->ibo);
#endif
	glBindBuffer(GL_ARRAY_BUFFER, header->vbo);
	glBufferData(GL_ARRAY_BUFFER, header->totalNumVertex*attribs[0].stride,
	             header->vertexBuffer, GL_STATIC_DRAW);
#ifdef RW_GL_USE_VAOS
	setAttribPointers(header->attribDesc, header->numAttribs);
	glBindVertexArray(0);
#endif
}

void
skinUninstanceCB(Geometry *geo, InstanceDataHeader *header)
{
	assert(0 && "can't uninstance");
}

// Three rows of each bone matrix, which is all skin.vert reads.
static float skinMatrices[64*12];

void
uploadSkinMatrices(Atomic *a)
{
	int i;
	Skin *skin = Skin::get(a->geometry);
	HAnimHierarchy *hier = Skin::getHierarchy(a);
	Matrix bones[64];
	int32 numBones = skin->numBones;
	if(numBones > 64)
		numBones = 64;

	if(hier){
		Matrix *invMats = (Matrix*)skin->inverseMatrices;
		Matrix tmp;

		assert(skin->numBones == hier->numNodes);
		if(hier->flags & HAnimHierarchy::LOCALSPACEMATRICES){
			for(i = 0; i < numBones; i++){
				invMats[i].flags = 0;
				Matrix::mult(&bones[i], &invMats[i], &hier->matrices[i]);
			}
		}else{
			Matrix invAtmMat;
			Matrix::invert(&invAtmMat, a->getFrame()->getLTM());
			for(i = 0; i < numBones; i++){
				invMats[i].flags = 0;
				Matrix::mult(&tmp, &hier->matrices[i], &invAtmMat);
				Matrix::mult(&bones[i], &invMats[i], &tmp);
			}
		}
	}else{
		for(i = 0; i < numBones; i++)
			bones[i].setIdentity();
	}

	float *m = skinMatrices;
	for(i = 0; i < numBones; i++){
		RawMatrix rows;
		RawMatrix::transpose(&rows, (RawMatrix*)&bones[i]);
		memcpy(m, &rows, 12*sizeof(float));
		m += 12;
	}
	setUniform(u_boneMatrices, skinMatrices);
}

// The caster pass for a skinned atomic. The one thing it cannot share with
// gl3render.cpp's is the bones: without them every vertex casts from its bind
// pose, which is a shadow of a character standing still inside one that is not.
//
// No lightingCB here either -- see the comment on defaultRenderDepthCB.
void
skinRenderDepthCB(Atomic *atomic, InstanceDataHeader *header)
{
	setWorldMatrix(atomic->getFrame()->getLTM());
	setupVertexInput(header);
	uploadSkinMatrices(atomic);

	InstanceData *inst = header->inst;
	int32 n = header->numMeshes;
	while(n--){
		Material *m = inst->material;

		// As in defaultRenderDepthCB: cast the shape the texture cuts, not
		// the rectangle it was cut from, and let setTexture decide whether
		// there is anything to cut.
		if(m->texture){
			setTexture(0, m->texture);
			skinDepthShader_tex->use();
		}else
			skinDepthShader->use();

		drawInst(header, inst);
		inst++;
	}

	teardownVertexInput(header);
}

void
skinRenderCB(Atomic *atomic, InstanceDataHeader *header)
{
	Material *m;

	uint32 flags = atomic->geometry->flags;
	setWorldMatrix(atomic->getFrame()->getLTM());
	int32 vsBits = lightingCB(atomic);

	setupVertexInput(header);

	InstanceData *inst = header->inst;
	int32 n = header->numMeshes;

	uploadSkinMatrices(atomic);

	// The hull first, so the model is drawn over the middle of it and only the
	// band that sticks out past the silhouette survives.
	//
	// Front faces culled: what is left of an inflated copy after removing the
	// faces pointing at the camera is its far side, which the real model then
	// covers except around the edge. Depth still written, so the band sorts
	// against the scene like any other geometry.
	int32 outline = getOutlineMode();

	if(outline != OUTLINE_NONE){
		// **Put back what was standing, not CULLBACK.** The application decides
		// whether a model is drawn two-sided, and a hull that restores CULLBACK
		// takes that away from the model's own pass: half of a shiny pickup, which
		// the game draws with no culling at all, simply disappeared.
		uint32 outlineCull = GetRenderState(CULLMODE);

		SetRenderState(CULLMODE, getOutlineInverted() ? CULLBACK : CULLFRONT);
		skinOutlineShader->use();

		InstanceData *oinst = header->inst;
		int32 on = header->numMeshes;

		while(on--){
			if(!outlineTakesMesh(header, oinst)){
				oinst++;
				continue;
			}

			// The hull reads the material's texture to tint its own ink -- see
			// outline.frag -- so it has to be bound here as well as in the
			// pass that draws the model itself, and the material with it: the
			// ink is drawn at the surface's alpha. See gl3render.cpp.
			setMaterial(flags, oinst->material->color, oinst->material->surfaceProps);
			setPipelineVertexAlpha(oinst->vertexAlpha ||
			                       oinst->material->color.alpha != 0xFF);
			setTexture(0, oinst->material->texture);
			drawInst(header, oinst);
			oinst++;
		}

		SetRenderState(CULLMODE, outlineCull);
	}

	while(n--){
		m = inst->material;

		setMaterial(flags, m->color, m->surfaceProps);

		setTexture(0, m->texture);

		setPipelineVertexAlpha(inst->vertexAlpha || m->color.alpha != 0xFF);

		// Same rule as the default pipeline in gl3render.cpp: per-pixel
		// replaces the directional-only case, and the cel look any lit case.
		// A skinned draw with no lights never takes the cel look, as on D3D9.
		if((vsBits & VSLIGHT_MASK) == 0){
			if(getAlphaTest())
				skinShader->use();
			else
				skinShader_noAT->use();
		}else if(getToonShading() ||
		         (getPerPixelLighting() && (vsBits & VSLIGHT_MASK) == VSLIGHT_DIRECT)){
			if(getAlphaTest())
				skinShader_pp->use();
			else
				skinShader_pp_noAT->use();
		}else{
			if(getAlphaTest())
				skinShader_fullLight->use();
			else
				skinShader_fullLight_noAT->use();
		}

		drawInst(header, inst);
		inst++;
	}
	teardownVertexInput(header);
}

static void*
skinOpen(void *o, int32, int32)
{
	// Only the platform that is RUNNING. Engine::start constructs every
	// platform's driver plugins, and a build may carry several backends -- so
	// without this a D3D9 run would build GL3's pipelines and compile their
	// shaders with no GL context, and a GL3 run would do the same to D3D's
	// with no device. See gl3.cpp's driverOpen.
	if(rw::platform != PLATFORM_GL3)
		return o;

	skinGlobals.pipelines[PLATFORM_GL3] = makeSkinPipeline();
	skinGlobals.matfxPipelines[PLATFORM_GL3] = makeSkinMatFXPipeline();

#include "shaders/simple_fs_gl.inc"
#include "shaders/skin_gl.inc"
#include "shaders/lighting_fs.inc"
	const char *vs[] = { shaderDecl, header_vert_src, skin_vert_src, nil };
	const char *vs_fullLight[] = { shaderDecl, "#define DIRECTIONALS\n#define POINTLIGHTS\n#define SPOTLIGHTS\n", header_vert_src, skin_vert_src, nil };
	const char *fs[] = { shaderDecl, "#define SHADOWRECEIVER\n", header_frag_src, simple_frag_src, nil };
	const char *fs_noAT[] = { shaderDecl, "#define SHADOWRECEIVER\n", "#define NO_ALPHATEST\n", header_frag_src, simple_frag_src, nil };

	skinShader = Shader::create(vs, fs);
	assert(skinShader);
	skinShader_noAT = Shader::create(vs, fs_noAT);
	assert(skinShader_noAT);

	skinShader_fullLight = Shader::create(vs_fullLight, fs);
	assert(skinShader_fullLight);
	skinShader_fullLight_noAT = Shader::create(vs_fullLight, fs_noAT);
	assert(skinShader_fullLight_noAT);

	// Per-pixel. One vertex shader rather than two: it does no lighting, so
	// there is nothing for DIRECTIONALS to switch on.
	const char *vs_pp[] = { shaderDecl, "#define PERPIXEL\n", header_vert_src, skin_vert_src, nil };
	const char *fs_pp[] = { shaderDecl, "#define SHADOWRECEIVER\n", "#define PERPIXEL\n", header_frag_src, lighting_frag_src, simple_frag_src, nil };
	const char *fs_pp_noAT[] = { shaderDecl, "#define SHADOWRECEIVER\n", "#define PERPIXEL\n#define NO_ALPHATEST\n", header_frag_src, lighting_frag_src, simple_frag_src, nil };

	skinShader_pp = Shader::create(vs_pp, fs_pp);
	assert(skinShader_pp);
	skinShader_pp_noAT = Shader::create(vs_pp, fs_pp_noAT);
	assert(skinShader_pp_noAT);

	// The skinned caster: skin.vert as it is, plus the shared depth shader.
	{
#include "shaders/depth_fs.inc"
		const char *fs_depth[] = { shaderDecl, header_frag_src, depth_frag_src, nil };
		skinDepthShader = Shader::create(vs, fs_depth);
		assert(skinDepthShader);

		// And the one that reads the caster's texture, so a skinned caster
		// cuts its shape out of the alpha channel like anything else.
		const char *fs_depth_tex[] = { shaderDecl, "#define TEX\n", header_frag_src, depth_frag_src, nil };
		skinDepthShader_tex = Shader::create(vs, fs_depth_tex);
		assert(skinDepthShader_tex);

		// The skinned outline hull. Same fragment shader as the unskinned one
		// -- a flat colour does not care how the vertex got where it is.
#include "shaders/outline_fs.inc"
		const char *vs_outline[] = { shaderDecl, "#define OUTLINE\n", header_vert_src, skin_vert_src, nil };
		// lighting.frag between the two, because the ink is lit and
		// ToonRoomLight is where the light uniforms are declared.
		const char *fs_outline[] = { shaderDecl, header_frag_src, lighting_frag_src, outline_frag_src, nil };
		skinOutlineShader = Shader::create(vs_outline, fs_outline);
		assert(skinOutlineShader);
	}

	createSkinMatFXShaders();

	return o;
}

static void*
skinClose(void *o, int32, int32)
{
	// See this file's other half; the pipelines were never built.
	if(rw::platform != PLATFORM_GL3)
		return o;

	((ObjPipeline*)skinGlobals.pipelines[PLATFORM_GL3])->destroy();
	skinGlobals.pipelines[PLATFORM_GL3] = nil;

	((ObjPipeline*)skinGlobals.matfxPipelines[PLATFORM_GL3])->destroy();
	skinGlobals.matfxPipelines[PLATFORM_GL3] = nil;

	destroySkinMatFXShaders();

	skinShader->destroy();
	skinShader = nil;
	skinShader_noAT->destroy();
	skinShader_noAT = nil;
	skinShader_fullLight->destroy();
	skinShader_fullLight = nil;
	skinShader_fullLight_noAT->destroy();
	skinShader_fullLight_noAT = nil;
	skinShader_pp->destroy();
	skinShader_pp = nil;
	skinShader_pp_noAT->destroy();
	skinShader_pp_noAT = nil;
	skinDepthShader->destroy();
	skinDepthShader = nil;
	skinDepthShader_tex->destroy();
	skinDepthShader_tex = nil;

	return o;
}

void
initSkin(void)
{
	u_boneMatrices = registerUniform("u_boneMatrices", UNIFORM_VEC4, 64*3);
	// The combined pipeline's shader reads the environment uniforms, and this
	// plugin registers it whether or not the matfx plugin is in the build.
	registerEnvUniforms();

	Driver::registerPlugin(PLATFORM_GL3, 0, ID_SKIN,
	                       skinOpen, skinClose);
}

ObjPipeline*
makeSkinPipeline(void)
{
	ObjPipeline *pipe = ObjPipeline::create();
	pipe->instanceCB = skinInstanceCB;
	pipe->uninstanceCB = skinUninstanceCB;
	pipe->renderCB = skinRenderCB;
	pipe->depthRenderCB = skinRenderDepthCB;
	pipe->pluginID = ID_SKIN;
	pipe->pluginData = 1;
	return pipe;
}

#else

void initSkin(void) { }

#endif

}
}

