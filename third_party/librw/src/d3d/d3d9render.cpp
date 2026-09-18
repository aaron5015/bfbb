#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define WITH_D3D
#include "../rwbase.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "../rwrender.h"
#include "rwd3d.h"
#include "rwd3d9.h"

namespace rw {
namespace d3d9 {
using namespace d3d;

#if !defined(RW_D3D_ANY)
void defaultRenderCB(Atomic*, InstanceDataHeader*) {}
void defaultRenderCB_Shader(Atomic *atomic, InstanceDataHeader *header) {}
void uvTransformRenderCB_Shader(Atomic *atomic, InstanceDataHeader *header) {}
#else

void
drawInst_simple(d3d9::InstanceDataHeader *header, d3d9::InstanceData *inst)
{
	d3d::flushCache();
	d3d::drawIndexedPrimitive(header->primType, inst->baseIndex,
	                          0, inst->numVertices,
	                          inst->startIndex, inst->numPrimitives);
}

// Emulate PS2 GS alpha test FB_ONLY case: failed alpha writes to frame- but not to depth buffer
void
drawInst_GSemu(d3d9::InstanceDataHeader *header, InstanceData *inst)
{
	int alphafunc, alpharef, gsalpharef;
	int zwrite;
	if(d3d::getBlendEnabled()){
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
drawInst(d3d9::InstanceDataHeader *header, d3d9::InstanceData *inst)
{
	if(rw::GetRenderState(rw::GSALPHATEST))
		drawInst_GSemu(header, inst);
	else
		drawInst_simple(header, inst);
}

/*
void
defaultRenderCB_Fix(Atomic *atomic, InstanceDataHeader *header)
{
	RawMatrix world;
	Geometry *geo = atomic->geometry;

	int lighting = !!(geo->flags & rw::Geometry::LIGHT);
	if(lighting)
		d3d::lightingCB_Fix(atomic);

	d3d::setRenderState(D3DRS_LIGHTING, lighting);

	Frame *f = atomic->getFrame();
	convMatrix(&world, f->getLTM());
	d3ddevice->SetTransform(D3DTS_WORLD, (D3DMATRIX*)&world);

	setStreamSource(0, header->vertexStream[0].vertexBuffer, 0, header->vertexStream[0].stride);
	setIndices(header->indexBuffer);
	setVertexDeclaration(header->vertexDeclaration);

	InstanceData *inst = header->inst;
	for(uint32 i = 0; i < header->numMeshes; i++){
		d3d::setPipelineVertexAlpha(inst->vertexAlpha || inst->material->color.alpha != 255);
		const static rw::RGBA white = { 255, 255, 255, 255 };
		d3d::setMaterial(white, inst->material->surfaceProps);

		d3d::setRenderState(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
		if(geo->flags & Geometry::PRELIT)
			d3d::setRenderState(D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_COLOR1);
		else
			d3d::setRenderState(D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
		d3d::setRenderState(D3DRS_DIFFUSEMATERIALSOURCE, inst->vertexAlpha ? D3DMCS_COLOR1 : D3DMCS_MATERIAL);

		if(inst->material->texture){
			// Texture
			d3d::setTexture(0, inst->material->texture);
			d3d::setTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
			d3d::setTextureStageState(0, D3DTSS_COLORARG1, D3DTA_CURRENT);
			d3d::setTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TEXTURE);
			d3d::setTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
			d3d::setTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
			d3d::setTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_TEXTURE);
		}else{
			d3d::setTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
			d3d::setTextureStageState(0, D3DTSS_COLORARG1, D3DTA_CURRENT);
			d3d::setTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
			d3d::setTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
		}

		// Material colour
		const rw::RGBA *col = &inst->material->color;
		d3d::setTextureStageState(1, D3DTSS_CONSTANT, D3DCOLOR_ARGB(col->alpha,col->red,col->green,col->blue));
		d3d::setTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE);
		d3d::setTextureStageState(1, D3DTSS_COLORARG1, D3DTA_CURRENT);
		d3d::setTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CONSTANT);
		d3d::setTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
		d3d::setTextureStageState(1, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
		d3d::setTextureStageState(1, D3DTSS_ALPHAARG2, D3DTA_CONSTANT);

		drawInst(header, inst);
		inst++;
	}
	d3d::setTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
	d3d::setTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
}
*/

// The default pipeline's render, and the UV-transforming one's. They differ by
// two shader blobs and one constant upload, so they are one function rather
// than a copy that will drift.
static void
renderCB_Shader(Atomic *atomic, InstanceDataHeader *header, bool32 uvXform)
{
	int vsBits;
	uint32 flags = atomic->geometry->flags;
	setStreamSource(0, header->vertexStream[0].vertexBuffer, 0, header->vertexStream[0].stride);
	setIndices(header->indexBuffer);
	setVertexDeclaration(header->vertexDeclaration);

	vsBits = lightingCB_Shader(atomic);
	uploadMatrices(atomic->getFrame()->getLTM());

	// Uploaded per atomic and not cached, because the transform is state the
	// application changes between draws -- that is what makes a surface
	// animate -- so there is nothing here that stays the same long enough to
	// be worth comparing against.
	if(uvXform)
		d3d::setVertexShaderConstantF(VSLOC_uvXform, uvTransform,
		                              NUMUVTRANSFORMELEMENTS/4);

	// Pick a shader.
	//
	// Per-pixel lighting replaces exactly one of the three cases: directional
	// lights and nothing else. Ambient alone is the same colour at every pixel
	// and has nothing to gain, and the per-pixel shaders do not do point or
	// spot lights, so anything reached by one keeps the per-vertex path.
	bool32 perPixel = getPerPixelLighting() &&
	                  (vsBits & VSLIGHT_MASK) == VSLIGHT_DIRECT;

	// The cel look stands where the per-pixel path does and needs the same
	// vertex shader -- that is the one carrying a normal across. Unlike
	// per-pixel it does not care how many lights there are, because it uses
	// none of them: the direction and the room colour were resolved on the way
	// to the uniform.
	// A draw with no lights is usually art that wants nothing done to it, which
	// is why the count is asked at all. getToonUnlit is a draw saying otherwise.
	bool32 toon = getToonShading() &&
	              ((vsBits & VSLIGHT_MASK) != 0 || getToonUnlit());

	if(toon)
		perPixel = 1;

	uploadToonConstants();

	// The hull, before the model, so the model is drawn over the middle of it
	// and only the band past the silhouette survives. Front faces culled: what
	// is left of an inflated copy once the faces pointing at the camera are
	// gone is its far side, which the real model then covers except at the
	// edge. The same pass the skin pipeline draws, for everything that is not
	// a character -- a tree is a static atomic and came through here.
	int32 outline = getOutlineMode();

	if(outline != OUTLINE_NONE){

		uploadOutlineConstants();
		setVertexShader(outline_VS);
		setPixelShader(outline_PS);
		// Or back faces, for a model wound inside out: its near side is the
		// one pointing away, and the copy is pushed inward to match.
		// **Put back what was standing, not CULLBACK.** The application decides
		// whether a model is drawn two-sided, and a hull that restores CULLBACK
		// takes that away from the model's own pass: half of a shiny pickup, which
		// the game draws with no culling at all, simply disappeared.
		uint32 outlineCull = GetRenderState(CULLMODE);

		SetRenderState(CULLMODE, getOutlineInverted() ? CULLBACK : CULLFRONT);

		InstanceData *oinst = header->inst;

		for(uint32 i = 0; i < header->numMeshes; i++){
			if(outlineTakesMesh(header, oinst)){
				// **The surface's own material, which nothing here uploaded.**
				// The hull runs before the mesh loop, so the colour standing
				// was the last mesh of whatever was drawn before this atomic --
				// and the ink takes its alpha from that colour, so a model
				// fading out was inked at a stranger's opacity. See the
				// outline pixel shader, where the ink is the surface darkened.
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

	// **The per-pixel case is asked first, and it has to be.** It used to come
	// second, behind a test for no lights at all, which was harmless while
	// per-pixel and the cel look both needed a light: the first arm never took a
	// draw the second wanted. An unlit cel draw is exactly that draw, and the
	// shader it needs is the one that carries a normal and a view vector across.
	if(perPixel)
		setVertexShader(uvXform ? uvxform_pp_VS : default_pp_VS);
	else if((vsBits & VSLIGHT_MASK) == 0)
		setVertexShader(uvXform ? uvxform_amb_VS : default_amb_VS);
	else if((vsBits & VSLIGHT_MASK) == VSLIGHT_DIRECT)
		setVertexShader(uvXform ? uvxform_amb_dir_VS : default_amb_dir_VS);
	else
		setVertexShader(uvXform ? uvxform_all_VS : default_all_VS);

	InstanceData *inst = header->inst;
	for(uint32 i = 0; i < header->numMeshes; i++){
		Material *m = inst->material;

		d3d::setPipelineVertexAlpha(inst->vertexAlpha || m->color.alpha != 255);

		setMaterial(flags, m->color, m->surfaceProps);

		if(m->texture){
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

// Whether the hull is drawn round this mesh.
//
// **A hull is geometry, so it traces the shape a mesh is CUT from and not the
// shape its texture leaves behind.** Round a plant's alpha card that is a
// rectangle of ink with a plant inside it, which is the one way this effect
// looks like a bug rather than a style. So a mesh whose shape is cut by
// transparency is left alone, whether that is a texture with holes in it or
// alpha painted per vertex.
//
// And nothing small enough to be a detail: the eyebrows and the teeth are
// scraps laid over a face, and a hull around a scrap is an ink border around
// the scrap.
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
		   GETD3DRASTEREXT(m->texture->raster)->hasAlpha)
			return 0;
	}

	return inst->numVertices*20 >= (int32)header->totalNumVertex;
}

void
defaultRenderCB_Shader(Atomic *atomic, InstanceDataHeader *header)
{
	renderCB_Shader(atomic, header, 0);
}

void
uvTransformRenderCB_Shader(Atomic *atomic, InstanceDataHeader *header)
{
	renderCB_Shader(atomic, header, 1);
}

#endif
}
}
