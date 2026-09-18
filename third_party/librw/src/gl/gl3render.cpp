#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwrender.h"
#include "../rwengine.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#ifdef RW_OPENGL
#include "rwgl3.h"
#include "rwgl3shader.h"

#include "rwgl3impl.h"

namespace rw {
namespace gl3 {

#define MAX_LIGHTS 

void
drawInst_simple(InstanceDataHeader *header, InstanceData *inst)
{
	flushCache();
	glDrawElements(header->primType, inst->numIndex,
	               GL_UNSIGNED_SHORT, (void*)(uintptr)inst->offset);
}

// Emulate PS2 GS alpha test FB_ONLY case: failed alpha writes to frame- but not to depth buffer
void
drawInst_GSemu(InstanceDataHeader *header, InstanceData *inst)
{
	uint32 hasAlpha;
	int alphafunc, alpharef, gsalpharef;
	int zwrite;
	hasAlpha = getAlphaBlend();
	if(hasAlpha){
		zwrite = rw::GetRenderState(rw::ZWRITEENABLE);
		alphafunc = rw::GetRenderState(rw::ALPHATESTFUNC);
		if(zwrite){
			alpharef = rw::GetRenderState(rw::ALPHATESTREF);
			gsalpharef = rw::GetRenderState(rw::GSALPHATESTREF);

			SetRenderState(rw::ALPHATESTFUNC, rw::ALPHAGREATEREQUAL);
			SetRenderState(rw::ALPHATESTREF, gsalpharef);
			drawInst_simple(header, inst);
			SetRenderState(rw::ALPHATESTFUNC, rw::ALPHALESS);
			SetRenderState(rw::ZWRITEENABLE, 0);
			drawInst_simple(header, inst);
			SetRenderState(rw::ZWRITEENABLE, 1);
			SetRenderState(rw::ALPHATESTFUNC, alphafunc);
			SetRenderState(rw::ALPHATESTREF, alpharef);
		}else{
			SetRenderState(rw::ALPHATESTFUNC, rw::ALPHAALWAYS);
			drawInst_simple(header, inst);
			SetRenderState(rw::ALPHATESTFUNC, alphafunc);
		}
	}else
		drawInst_simple(header, inst);
}

void
drawInst(InstanceDataHeader *header, InstanceData *inst)
{
	if(rw::GetRenderState(rw::GSALPHATEST))
		drawInst_GSemu(header, inst);
	else
		drawInst_simple(header, inst);
}


void
setAttribPointers(AttribDesc *attribDescs, int32 numAttribs)
{
	AttribDesc *a;
	for(a = attribDescs; a != &attribDescs[numAttribs]; a++){
		glEnableVertexAttribArray(a->index);
		glVertexAttribPointer(a->index, a->size, a->type, a->normalized,
		                      a->stride, (void*)(uint64)a->offset);
	}
}

void
disableAttribPointers(AttribDesc *attribDescs, int32 numAttribs)
{
	AttribDesc *a;
	for(a = attribDescs; a != &attribDescs[numAttribs]; a++)
		glDisableVertexAttribArray(a->index);
}

void
setupVertexInput(InstanceDataHeader *header)
{
#ifdef RW_GL_USE_VAOS
	glBindVertexArray(header->vao);
#else
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, header->ibo);
	glBindBuffer(GL_ARRAY_BUFFER, header->vbo);
	setAttribPointers(header->attribDesc, header->numAttribs);
#endif
}

void
teardownVertexInput(InstanceDataHeader *header)
{
#ifndef RW_GL_USE_VAOS
	disableAttribPointers(header->attribDesc, header->numAttribs);
#endif
}

int32
lightingCB(Atomic *atomic)
{
	WorldLights lightData;
	Light *directionals[8];
	Light *locals[8];
	lightData.directionals = directionals;
	lightData.numDirectionals = 8;
	lightData.locals = locals;
	lightData.numLocals = 8;

	if(atomic->geometry->flags & rw::Geometry::LIGHT)
		((World*)engine->currentWorld)->enumerateLights(atomic, &lightData);
	else
		memset(&lightData, 0, sizeof(lightData));
	return setLights(&lightData);
}

int32
lightingCB(void)
{
	WorldLights lightData;
	Light *directionals[8];
	Light *locals[8];
	lightData.directionals = directionals;
	lightData.numDirectionals = 8;
	lightData.locals = locals;
	lightData.numLocals = 8;

	((World*)engine->currentWorld)->enumerateLights(&lightData);
	return setLights(&lightData);
}


// Whether the hull is drawn round this mesh.
//
// **A hull is geometry, so it traces the shape a mesh is CUT from and not the
// shape its texture leaves behind.** Round a plant's alpha card that is a
// rectangle of ink with a plant inside it, which is the one way this effect
// looks like a bug rather than a style. So a mesh whose shape is cut by
// transparency is left alone, whether that is a texture with holes in it or
// alpha painted per vertex. An ink line is a statement that a surface ends
// here, and an edge that only a texture puts there is not one.
//
// **And nothing small enough to be a detail.** The eyebrows and the teeth are
// separate scraps laid over the face, so a hull around one is an ink border
// around the scrap -- SpongeBob with outlined eyebrows, which no drawing of him
// has. A face is thousands of vertices and an eyebrow is a handful, and a
// twentieth of the model is well clear of a hand or a shoe.
// Whether a see-through mesh may still be inked. Off by default, so a plant
// card and a floor decal are left alone; the application turns it on around a
// model it NAMED as a character, because a jellyfish and Bubble Buddy are
// see-through by nature and are still the thing a line goes round.
static bool32 outlineAlphaOK;

void
setOutlineAlpha(bool32 allow)
{
	outlineAlphaOK = !!allow;
}

bool32
outlineTakesMesh(InstanceDataHeader *header, InstanceData *inst)
{
	Material *m = inst->material;

	// **A material's own alpha is a fade and not a cutout.** It is one number
	// over the whole mesh, so it says how solid the surface is and nothing about
	// what shape it is -- the hull still traces the same silhouette. The two
	// that DO change the shape are a texture with holes in it and alpha painted
	// per vertex, and those are what this refuses. Before, any model on its way
	// out lost its line the moment its alpha left 255: the HUD models pop off
	// as the interface fades, and a tiki fades as the camera closes on it.
	if(!outlineAlphaOK){
		if(inst->vertexAlpha)
			return 0;

		if(m->texture && m->texture->raster &&
		   GETGL3RASTEREXT(m->texture->raster)->hasAlpha)
			return 0;
	}

	return inst->numVertices*20 >= (int32)header->totalNumVertex;
}

// The default pipeline's render, and the UV-transforming one's. They differ by
// four shader programs and one uniform upload, so they are one function rather
// than a copy that will drift.
static void
renderCB(Atomic *atomic, InstanceDataHeader *header, bool32 uvXform)
{
	Material *m;

	uint32 flags = atomic->geometry->flags;
	setWorldMatrix(atomic->getFrame()->getLTM());
	int32 vsBits = lightingCB(atomic);

	setupVertexInput(header);

	// Uploaded per atomic and not cached, because the transform is state the
	// application changes between draws -- that is what makes a surface
	// animate -- so there is nothing here that stays the same long enough to
	// be worth comparing against.
	if(uvXform)
		setUniform(u_uvXform, uvTransform);

	// The hull, before the model, so the model covers the middle of it and only
	// the band past the silhouette survives. Front faces culled: what is left of
	// an inflated copy once the faces pointing at the camera are gone is its far
	// side. The same pass gl3skin.cpp draws, for everything that is not a
	// character -- a tree is a static atomic and came through here.
	if(getOutlineMode() != OUTLINE_NONE){
		// Or back faces, for a model wound inside out: its near side is the
		// one pointing away, and the copy is pushed inward to match.
		// **Put back what was standing, not CULLBACK.** The application decides
		// whether a model is drawn two-sided, and a hull that restores CULLBACK
		// takes that away from the model's own pass: half of a shiny pickup, which
		// the game draws with no culling at all, simply disappeared.
		uint32 outlineCull = GetRenderState(CULLMODE);

		SetRenderState(CULLMODE, getOutlineInverted() ? CULLBACK : CULLFRONT);
		outlineShader->use();

		InstanceData *oinst = header->inst;
		int32 on = header->numMeshes;

		while(on--){
			if(outlineTakesMesh(header, oinst)){
				// The hull reads the material's texture to tint its own ink
				// -- see outline.frag -- so it is bound here as well, and the
				// material with it: the ink is drawn at the surface's alpha
				// and nothing else uploads one before the mesh loop.
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
		m = inst->material;

		setMaterial(flags, m->color, m->surfaceProps);

		setTexture(0, m->texture);

		setPipelineVertexAlpha(inst->vertexAlpha || m->color.alpha != 0xFF);

		// Per-pixel lighting replaces exactly one of the light cases:
		// directional and nothing else. Ambient alone is the same colour at
		// every fragment and has nothing to gain, and the per-pixel fragment
		// shader does not do point or spot lights, so anything reached by one
		// keeps the per-vertex path.
		//
		// The cel look takes the per-pixel shader whatever the lights are and
		// whether or not per-pixel lighting is on: that is the one carrying a
		// normal and a view vector across, and the fragment shader's toon block
		// is inside its PERPIXEL guard. A draw with no lights takes it only
		// where it asked to with setToonUnlit. See the D3D9 twin in
		// d3d9render.cpp.
		bool32 toon = getToonShading() &&
		              ((vsBits & VSLIGHT_MASK) != 0 || getToonUnlit());
		bool32 perPixel = toon ||
		                  (getPerPixelLighting() && (vsBits & VSLIGHT_MASK) == VSLIGHT_DIRECT);

		if(perPixel){
			if(getAlphaTest())
				(uvXform ? uvXformShader_pp : defaultShader_pp)->use();
			else
				(uvXform ? uvXformShader_pp_noAT : defaultShader_pp_noAT)->use();
		}else if((vsBits & VSLIGHT_MASK) == 0){
			if(getAlphaTest())
				(uvXform ? uvXformShader : defaultShader)->use();
			else
				(uvXform ? uvXformShader_noAT : defaultShader_noAT)->use();
		}else{
			if(getAlphaTest())
				(uvXform ? uvXformShader_fullLight : defaultShader_fullLight)->use();
			else
				(uvXform ? uvXformShader_fullLight_noAT : defaultShader_fullLight_noAT)->use();
		}

		drawInst(header, inst);
		inst++;
	}
	teardownVertexInput(header);
}

// The caster pass for a pipeline whose vertices need no moving. matfx shares
// it: an environment map has nothing to contribute to a depth value.
//
// Deliberately does NOT call lightingCB. That reads engine->currentWorld, which
// Camera::beginUpdate takes from the camera's own world, and a camera rendering
// an offscreen target need not belong to one -- the shadow map's does not.
// setWorldMatrix marks the uniform block dirty by itself, so nothing here
// depends on having been through the lighting.
void
defaultRenderDepthCB(Atomic *atomic, InstanceDataHeader *header)
{
	setWorldMatrix(atomic->getFrame()->getLTM());
	setupVertexInput(header);

	InstanceData *inst = header->inst;
	int32 n = header->numMeshes;
	while(n--){
		Material *m = inst->material;

		// A caster that cuts its shape out of a texture has to cast that
		// shape and not the rectangle it was cut from.
		//
		// Whether to actually cut is left to setTexture and the shader rather
		// than decided here. setTexture reads the raster's own alpha kind and
		// turns the test on or off from it; where it turns it off, u_alphaRef
		// opens to a range nothing can fall outside and DoAlphaTest discards
		// nothing. So an opaque texture costs one fetch and changes no pixel,
		// and asking getAlphaTest() first would only ever read the state of
		// the PREVIOUS mesh.
		if(m->texture){
			setTexture(0, m->texture);
			depthShader_tex->use();
		}else
			depthShader->use();

		drawInst(header, inst);
		inst++;
	}

	teardownVertexInput(header);
}

void
defaultRenderCB(Atomic *atomic, InstanceDataHeader *header)
{
	renderCB(atomic, header, 0);
}

void
uvTransformRenderCB(Atomic *atomic, InstanceDataHeader *header)
{
	renderCB(atomic, header, 1);
}


}
}

#endif

