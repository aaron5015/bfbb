// The fixed-function pipeline.
//
// The same picture as default_VS.hlsl and default_PS.hlsl, drawn without a
// shader: D3D9's own transform, lighting, texture stages and fog. What it is
// for is hardware older than Shader Model 2.0 -- DX7-class hardware T&L, which
// is the generation the game shipped on -- and, because it is a second
// implementation of the same lighting, alpha and blend semantics, a way to
// find the places where the shader path and the render states disagree.
//
// Everything here is dead unless getFixedFunction(). The flag is read once at
// driverOpen, which picks these render callbacks instead of the shader ones,
// and by the handful of seams that would otherwise name a shader. Nothing
// switches per draw.
//
// **What this is not.** It is a look-alike, not a match:
//
//   - Fixed-function lighting normalises with D3DRS_NORMALIZENORMALS rather
//     than with the inverse-transpose normal matrix the vertex shader is
//     handed, so an atomic under a non-uniform scale lights differently.
//   - Point and spot attenuation is D3D's 1/(a + bd + cd^2), not the PS2's
//     linear 1 - d/radius that lighting.h reproduces.
//   - The material colour is applied in a texture stage rather than in the
//     lighting, because the shaders multiply by it AFTER the lighting has been
//     clamped and a D3DMATERIAL9 cannot say that.
//   - Table fog is per pixel here and per vertex there, which is the one place
//     the fixed-function mode is the better picture.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define WITH_D3D
#include "../rwbase.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwanim.h"
#include "../rwengine.h"
#include "../rwrender.h"
#include "../rwplugins.h"
#include "rwd3d.h"
#include "rwd3d9.h"
#include "rwd3dimpl.h"

namespace rw {
namespace d3d {

#ifndef RW_D3D9

void ffSetWorldTransform(Matrix*) {}
void ffSetWorldTransform(void) {}
void ffBeginUpdate(Camera*) {}
bool32 ffSetLighting(Atomic*) { return 0; }
void ffSetVertexColorSource(bool32, uint32, bool32) {}
void ffSetColorStages(Texture*, const RGBA&) {}
void ffSetUVTransform(bool32) {}
void ffOpenIm2D(void) {}
void ffCloseIm2D(void) {}
void *ffIm2DDeclaration(void) { return nil; }
void ffCopyIm2DVertices(void*, const void*, int32) {}
void ffSetupIm2D(void) {}
void ffSetupIm3D(uint32) {}
void ffSetupIm3DDraw(void) {}

#else

static const RGBA ffWhite = { 255, 255, 255, 255 };

// A float render state. The cache stores the bit pattern, which is what
// SetRenderState takes for the fog planes and nothing else here.
static void
setRenderStateF(uint32 state, float32 value)
{
	setRenderState(state, *(uint32*)&value);
}

// ---------------------------------------------------------------------------
// Transforms

static RawMatrix worldTransform;
static bool32 worldTransformSet;

void
ffSetWorldTransform(Matrix *worldMat)
{
	RawMatrix world;
	convMatrix(&world, worldMat);
	if(worldTransformSet && memcmp(&world, &worldTransform, sizeof(world)) == 0)
		return;
	worldTransform = world;
	worldTransformSet = 1;
	d3ddevice->SetTransform(D3DTS_WORLD, (D3DMATRIX*)&world);
}

void
ffSetWorldTransform(void)
{
	static const RawMatrix identity = {
		{ 1.0f, 0.0f, 0.0f }, 0.0f,
		{ 0.0f, 1.0f, 0.0f }, 0.0f,
		{ 0.0f, 0.0f, 1.0f }, 0.0f,
		{ 0.0f, 0.0f, 0.0f }, 1.0f
	};
	if(worldTransformSet && memcmp(&identity, &worldTransform, sizeof(identity)) == 0)
		return;
	worldTransform = identity;
	worldTransformSet = 1;
	d3ddevice->SetTransform(D3DTS_WORLD, (D3DMATRIX*)&identity);
}

void
ffBeginUpdate(Camera *cam)
{
	// devView and devProj are what beginUpdate already computed for the shader
	// constants. The same two matrices, handed to the device instead.
	d3ddevice->SetTransform(D3DTS_VIEW, (D3DMATRIX*)&cam->devView);
	d3ddevice->SetTransform(D3DTS_PROJECTION, (D3DMATRIX*)&cam->devProj);
	worldTransformSet = 0;

	// The shader's fog factor is clamp((w - farPlane)/(fogPlane - farPlane)),
	// and the pixel shader lerps from the fog colour towards the surface with
	// it: 1 at the fog plane, 0 at the far plane. D3D's linear fog factor is
	// (end - d)/(end - start) with the same sense, so start is the fog plane
	// and end is the far plane.
	//
	// D3DFOG_LINEAR in the TABLE, set once when the device was made, which
	// evaluates it per pixel from eye-relative depth. The shader path
	// interpolates a per-vertex factor.
	setRenderStateF(D3DRS_FOGSTART, cam->fogPlane);
	setRenderStateF(D3DRS_FOGEND, cam->farPlane);
}

// ---------------------------------------------------------------------------
// Lighting

bool32
ffSetLighting(Atomic *atomic)
{
	uint32 flags = atomic->geometry->flags;

	// No normals means no lighting can be computed, and no LIGHT flag means
	// lightingCB_Shader uploads a black ambient and no lights -- both of which
	// leave the vertex colours alone. Fixed function says that by switching
	// the lighting stage off, which also leaves the vertex colours alone and
	// costs nothing.
	bool32 lighting = (flags & Geometry::LIGHT) && (flags & Geometry::NORMALS);
	setRenderState(D3DRS_LIGHTING, lighting ? TRUE : FALSE);
	if(!lighting)
		return 0;

	lightingCB_Fix(atomic);

	// The vertex shader is handed a normal matrix that already took the scale
	// out. Fixed function transforms the normal by the world matrix and has
	// only this to undo it with.
	setRenderState(D3DRS_NORMALIZENORMALS, TRUE);
	// Nothing in the shaders writes a specular term.
	setRenderState(D3DRS_SPECULARENABLE, FALSE);
	setRenderState(D3DRS_COLORVERTEX, TRUE);
	return 1;
}

void
ffSetVertexColorSource(bool32 lighting, uint32 geoFlags, bool32 vertexAlpha)
{
	if(!lighting)
		return;

	// output.Color = input.Prelight, then the lights are added to it. The
	// emissive term is the one that is added unlit and unscaled, so that is
	// where the prelight goes.
	setRenderState(D3DRS_EMISSIVEMATERIALSOURCE,
	               (geoFlags & Geometry::PRELIT) ? D3DMCS_COLOR1 : D3DMCS_MATERIAL);
	setRenderState(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
	setRenderState(D3DRS_SPECULARMATERIALSOURCE, D3DMCS_MATERIAL);

	// The alpha of the lit vertex is the alpha of whatever the DIFFUSE term
	// reads, and there is no way to take the colour from one source and the
	// alpha from another. So a mesh whose vertices carry alpha reads its
	// diffuse from them -- which tints its dynamic light by the baked one, and
	// is the lesser of the two errors -- and every other mesh reads the white
	// material, which lights correctly.
	setRenderState(D3DRS_DIFFUSEMATERIALSOURCE,
	               vertexAlpha ? D3DMCS_COLOR1 : D3DMCS_MATERIAL);
}

// ---------------------------------------------------------------------------
// Texture stages
//
// default_PS.hlsl is `vertexColor * matCol * texture`, and the material colour
// is a whole-colour multiply that the lighting stage cannot express, so it
// arrives here as D3DRS_TEXTUREFACTOR. TEXTUREFACTOR rather than
// D3DTSS_CONSTANT: the per-stage constant is a D3D9 capability bit that the
// hardware this mode exists for does not have.

void
ffSetColorStages(Texture *texture, const RGBA &matColor)
{
	if(texture){
		setTexture(0, texture);
		setTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
		setTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		setTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
		setTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
		setTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
		setTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
	}else{
		// The stale texture stays bound, exactly as it does on the shader
		// path, where the pixel shader simply does not sample it.
		setTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
		setTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
		setTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
		setTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
	}

	if(matColor.red == 255 && matColor.green == 255 &&
	   matColor.blue == 255 && matColor.alpha == 255){
		setTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
		setTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
		return;
	}

	setRenderState(D3DRS_TEXTUREFACTOR,
	               D3DCOLOR_ARGB(matColor.alpha, matColor.red, matColor.green, matColor.blue));
	setTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE);
	setTextureStageState(1, D3DTSS_COLORARG1, D3DTA_CURRENT);
	setTextureStageState(1, D3DTSS_COLORARG2, D3DTA_TFACTOR);
	setTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
	setTextureStageState(1, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
	setTextureStageState(1, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
}

void
ffSetUVTransform(bool32 enable)
{
	if(!enable){
		setTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
		return;
	}

	// rw::uvTransform is two rows of a 2x4 that multiplies (u, v, 1, 1), so
	// both constant columns add to the same output -- see rwrender.h. D3D
	// multiplies the row vector (u, v, 1, 1) by the matrix, so the rows here
	// are that matrix's columns and the two constants are summed into one.
	D3DMATRIX m;
	memset(&m, 0, sizeof(m));
	m._11 = uvTransform[0];
	m._21 = uvTransform[1];
	m._31 = uvTransform[2] + uvTransform[3];
	m._12 = uvTransform[4];
	m._22 = uvTransform[5];
	m._32 = uvTransform[6] + uvTransform[7];
	m._33 = 1.0f;
	m._44 = 1.0f;
	d3ddevice->SetTransform(D3DTS_TEXTURE0, &m);
	setTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
}

// ---------------------------------------------------------------------------
// im2d
//
// The shader path declares POSITION and lets im2d_VS map pixels to clip space
// and multiply the result back up by w so the divide undoes it. Fixed function
// takes the same thing directly as POSITIONT, with 1/w in the fourth component
// as an RHW, so only the declaration and the w change.

static void *ffim2ddecl;

void
ffOpenIm2D(void)
{
	d3d9::VertexElement elements[4] = {
		{ 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITIONT, 0 },
		{ 0, offsetof(Im2DVertex, color), D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
		{ 0, offsetof(Im2DVertex, u), D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
		{ 0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0 }
	};
	assert(ffim2ddecl == nil);
	ffim2ddecl = d3d9::createVertexDeclaration((d3d9::VertexElement*)elements);
	assert(ffim2ddecl);
}

void
ffCloseIm2D(void)
{
	d3d9::destroyVertexDeclaration(ffim2ddecl);
	ffim2ddecl = nil;
}

void*
ffIm2DDeclaration(void)
{
	return ffim2ddecl;
}

void
ffCopyIm2DVertices(void *dst, const void *src, int32 numVertices)
{
	const Im2DVertex *in = (const Im2DVertex*)src;
	Im2DVertex *out = (Im2DVertex*)dst;
	for(int32 i = 0; i < numVertices; i++){
		out[i] = in[i];
		// w holds the camera-space z. A transformed vertex wants its
		// reciprocal, which is what makes the texture coordinates perspective
		// correct on a primitive the game has already projected itself.
		out[i].w = in[i].w != 0.0f ? 1.0f/in[i].w : 0.0f;
	}
}

void
ffSetupIm2D(void)
{
	setRenderState(D3DRS_LIGHTING, FALSE);
	ffSetUVTransform(0);

	// Table fog reads eye-relative depth, which a pre-transformed vertex does
	// not have. The shader path does fog a 2D primitive; nothing in the game
	// asks it to with fog enabled, and a wrong depth would fog the interface.
	setRenderState(D3DRS_FOGENABLE, FALSE);

	if(engine->device.getRenderState(TEXTURERASTER)){
		setTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
		setTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		setTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
		setTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
		setTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
		setTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
	}else{
		setTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
		setTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
		setTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
		setTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
	}
	setTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
	setTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
}

// ---------------------------------------------------------------------------
// im3d

void
ffSetupIm3D(uint32 flags)
{
	ffSetUVTransform(0);

	if(flags & im3d::LIGHTING){
		setRenderState(D3DRS_LIGHTING, TRUE);
		setRenderState(D3DRS_NORMALIZENORMALS, TRUE);
		setRenderState(D3DRS_SPECULARENABLE, FALSE);
		setRenderState(D3DRS_COLORVERTEX, TRUE);
		setRenderState(D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_COLOR1);
		setRenderState(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
		setRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL);
		setRenderState(D3DRS_SPECULARMATERIALSOURCE, D3DMCS_MATERIAL);
		lightingCB_Fix();
		setMaterial_fix(ffWhite, im3dSurfaceProps);
	}else{
		setRenderState(D3DRS_LIGHTING, FALSE);
	}
}

void
ffSetupIm3DDraw(void)
{
	// The material colour is the one im3dMaterialColor names; there is no
	// Material to read it from here, and the raster is bound by whatever set
	// TEXTURERASTER rather than by a Texture this could be handed.
	Raster *raster = (Raster*)engine->device.getRenderState(TEXTURERASTER);
	ffSetColorStages(nil, im3dMaterialColor);
	if(raster){
		setTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
		setTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		setTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
		setTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
		setTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
		setTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
	}
}

#endif

}

namespace d3d9 {
using namespace d3d;

#ifndef RW_D3D9

void defaultRenderCB_Fix(Atomic*, InstanceDataHeader*) {}
void uvTransformRenderCB_Fix(Atomic*, InstanceDataHeader*) {}
void skinRenderCB_Fix(Atomic*, InstanceDataHeader*) {}
void ffOpenSkin(void) {}
void ffCloseSkin(void) {}

#else

static const RGBA ffMatWhite = { 255, 255, 255, 255 };

// The default pipeline's render and the UV-transforming one's, as one function
// for the same reason the shader pair is: they differ by one texture matrix.
static void
renderCB_Fix(Atomic *atomic, InstanceDataHeader *header, bool32 uvXform)
{
	Geometry *geo = atomic->geometry;
	uint32 flags = geo->flags;

	setStreamSource(0, header->vertexStream[0].vertexBuffer, 0, header->vertexStream[0].stride);
	setIndices(header->indexBuffer);
	setVertexDeclaration(header->vertexDeclaration);

	ffSetWorldTransform(atomic->getFrame()->getLTM());
	bool32 lighting = ffSetLighting(atomic);
	ffSetUVTransform(uvXform);

	InstanceData *inst = header->inst;
	for(uint32 i = 0; i < header->numMeshes; i++){
		Material *m = inst->material;

		setPipelineVertexAlpha(inst->vertexAlpha || m->color.alpha != 255);

		// White, not the material's colour: the shaders clamp the lighting
		// before they multiply by the material, so that multiply happens in a
		// texture stage instead. What the lighting stage wants from the
		// material is only its ambient and diffuse coefficients.
		if(lighting){
			setMaterial_fix(ffMatWhite, m->surfaceProps);
			ffSetVertexColorSource(lighting, flags, inst->vertexAlpha);
		}

		ffSetColorStages(m->texture, (flags & Geometry::MODULATE) ? m->color : ffMatWhite);

		drawInst(header, inst);
		inst++;
	}
}

void
defaultRenderCB_Fix(Atomic *atomic, InstanceDataHeader *header)
{
	renderCB_Fix(atomic, header, 0);
}

void
uvTransformRenderCB_Fix(Atomic *atomic, InstanceDataHeader *header)
{
	renderCB_Fix(atomic, header, 1);
}

// ---------------------------------------------------------------------------
// Skinning, on the CPU
//
// Fixed-function vertex blending exists, and it is capped at
// MaxVertexBlendMatrixIndex matrices per draw -- typically four on hardware of
// this generation. The characters have far more bones than that, so the choice
// is to bone-partition every mesh into four-bone batches or to blend the
// vertices before they reach the device. This does the second, which is what
// games of that generation did: on a part with hardware T&L and no shaders the
// CPU is frequently the better skinner anyway.
//
// The inputs are the geometry's PORTABLE arrays, not the instanced vertex
// buffer: skin->indices and skin->weights, morph target 0's vertices and
// normals, and the colours and texture coordinates beside them. That is the
// same data skinInstanceCB reads, and reading it here avoids locking a managed
// buffer every frame to get at what is already in system memory.

struct SkinVertex
{
	V3d      position;
	V3d      normal;
	uint32   color;
	float32  u, v;
};

static void *ffskindecl;
static void *ffskinvertbuf;
static uint32 ffskinvertcap;	// in vertices, not bytes

void
ffOpenSkin(void)
{
	VertexElement elements[5] = {
		{ 0, offsetof(SkinVertex, position), D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
		{ 0, offsetof(SkinVertex, normal), D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0 },
		{ 0, offsetof(SkinVertex, color), D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
		{ 0, offsetof(SkinVertex, u), D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
		{ 0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0 }
	};
	assert(ffskindecl == nil);
	ffskindecl = createVertexDeclaration(elements);
	assert(ffskindecl);
}

void
ffCloseSkin(void)
{
	destroyVertexDeclaration(ffskindecl);
	ffskindecl = nil;

	if(ffskinvertbuf){
		removeDynamicVB((IDirect3DVertexBuffer9**)&ffskinvertbuf);
		destroyVertexBuffer(ffskinvertbuf);
		ffskinvertbuf = nil;
	}
	ffskinvertcap = 0;
}

// One buffer, grown to the largest skinned geometry the scene has drawn. It is
// registered as a dynamic VB so that a lost device gets it back; growing means
// dropping the old registration first, because the recreate remembers a size.
static bool32
reserveSkinVertices(uint32 numVertices)
{
	if(ffskinvertcap >= numVertices)
		return 1;

	if(ffskinvertbuf){
		removeDynamicVB((IDirect3DVertexBuffer9**)&ffskinvertbuf);
		destroyVertexBuffer(ffskinvertbuf);
		ffskinvertbuf = nil;
	}

	uint32 cap = 1024;
	while(cap < numVertices)
		cap *= 2;

	ffskinvertbuf = createVertexBuffer(cap*sizeof(SkinVertex), 0, true);
	if(ffskinvertbuf == nil){
		ffskinvertcap = 0;
		return 0;
	}
	addDynamicVB(cap*sizeof(SkinVertex), 0, (IDirect3DVertexBuffer9**)&ffskinvertbuf);
	ffskinvertcap = cap;
	return 1;
}

static void
skinVertices(Geometry *geo, Skin *skin, const Matrix *bones, int32 numBones,
             SkinVertex *out)
{
	V3d *verts = geo->morphTargets[0].vertices;
	V3d *norms = geo->morphTargets[0].normals;
	RGBA *colors = geo->colors;
	TexCoords *uvs = geo->numTexCoordSets > 0 ? geo->texCoords[0] : nil;
	const uint8 *indices = skin->indices;
	const float32 *weights = skin->weights;

	// What the constant vertex stream would have supplied to a shader for the
	// attributes this geometry does not carry. Same values, so that turning
	// the pipeline over does not also change what an unlit, uncoloured model
	// starts from.
	uint32 flatColor = constantVertexColorWhite ? 0xFFFFFFFF : 0xFF000000;

	for(int32 i = 0; i < geo->numVertices; i++){
		V3d p = { 0.0f, 0.0f, 0.0f };
		V3d n = { 0.0f, 0.0f, 0.0f };

		for(int j = 0; j < 4; j++){
			float32 w = weights[i*4 + j];
			if(w == 0.0f)
				continue;
			int32 b = indices[i*4 + j];
			if(b >= numBones)
				continue;

			V3d tp, tn;
			V3d::transformPoints(&tp, &verts[i], 1, &bones[b]);
			p = add(p, scale(tp, w));
			if(norms){
				V3d::transformVectors(&tn, &norms[i], 1, &bones[b]);
				n = add(n, scale(tn, w));
			}
		}

		// A vertex every weight missed stays where it was authored, which is
		// visibly wrong in one place rather than collapsed onto the origin.
		if(weights[i*4] == 0.0f && weights[i*4+1] == 0.0f &&
		   weights[i*4+2] == 0.0f && weights[i*4+3] == 0.0f){
			p = verts[i];
			if(norms)
				n = norms[i];
		}

		out[i].position = p;
		out[i].normal = norms ? n : makeV3d(0.0f, 0.0f, 0.0f);
		out[i].color = colors ?
			D3DCOLOR_ARGB(colors[i].alpha, colors[i].red, colors[i].green, colors[i].blue) :
			flatColor;
		out[i].u = uvs ? uvs[i].u : 0.0f;
		out[i].v = uvs ? uvs[i].v : 0.0f;
	}
}

void
skinRenderCB_Fix(Atomic *atomic, InstanceDataHeader *header)
{
	Geometry *geo = atomic->geometry;
	Skin *skin = Skin::get(geo);

	// Nothing to skin from. The portable arrays are what the Xbox conversion
	// in the port's rw/convert.cpp fills; a geometry that arrived without them
	// draws where it stands rather than not at all.
	if(skin == nil || skin->indices == nil || skin->weights == nil ||
	   geo->morphTargets[0].vertices == nil ||
	   !reserveSkinVertices(header->totalNumVertex)){
		defaultRenderCB_Fix(atomic, header);
		return;
	}

	Matrix bones[MAXNUMSKINBONES];
	int32 numBones = computeSkinMatrices(atomic, bones);

	SkinVertex *out = (SkinVertex*)lockVertices(ffskinvertbuf, 0,
	                                            header->totalNumVertex*sizeof(SkinVertex),
	                                            D3DLOCK_DISCARD);
	if(out == nil){
		defaultRenderCB_Fix(atomic, header);
		return;
	}
	skinVertices(geo, skin, bones, numBones, out);
	unlockVertices(ffskinvertbuf);

	// The index buffer is the header's: skinInstanceCB writes the vertices in
	// geometry order, and so does skinVertices, so the meshes' baseIndex and
	// startIndex mean the same thing against either buffer.
	setStreamSource(0, ffskinvertbuf, 0, sizeof(SkinVertex));
	setIndices(header->indexBuffer);
	setVertexDeclaration(ffskindecl);

	uint32 flags = geo->flags;
	ffSetWorldTransform(atomic->getFrame()->getLTM());
	bool32 lighting = ffSetLighting(atomic);
	ffSetUVTransform(0);

	InstanceData *inst = header->inst;
	for(uint32 i = 0; i < header->numMeshes; i++){
		Material *m = inst->material;

		setPipelineVertexAlpha(inst->vertexAlpha || m->color.alpha != 255);

		if(lighting){
			setMaterial_fix(ffMatWhite, m->surfaceProps);
			ffSetVertexColorSource(lighting, flags, inst->vertexAlpha);
		}

		ffSetColorStages(m->texture, (flags & Geometry::MODULATE) ? m->color : ffMatWhite);

		drawInst(header, inst);
		inst++;
	}
}

#endif

}
}
