#include <stdio.h>
#include <string.h>

#include "rwbase.h"
#include "rwplg.h"
#include "rwengine.h"
#include "rwrender.h"

namespace rw {

void SetRenderState(int32 state, uint32 value){
	engine->device.setRenderState(state, (void*)(uintptr)value); }

void SetRenderStatePtr(int32 state, void *value){
	engine->device.setRenderState(state, value); }

uint32 GetRenderState(int32 state){
	return (uint32)(uintptr)engine->device.getRenderState(state); }

void *GetRenderStatePtr(int32 state){
	return engine->device.getRenderState(state); }

// Texture coordinate transform. See rwrender.h for what the matrix means.

const float32 UVTRANSFORM_IDENTITY[NUMUVTRANSFORMELEMENTS] = {
	1.0f, 0.0f, 0.0f, 0.0f,
	0.0f, 1.0f, 0.0f, 0.0f
};

float32 uvTransform[NUMUVTRANSFORMELEMENTS] = {
	1.0f, 0.0f, 0.0f, 0.0f,
	0.0f, 1.0f, 0.0f, 0.0f
};

ObjPipeline *uvTransformPipelines[NUM_PLATFORMS];

void
SetUVTransform(const float32 *xform)
{
	if(xform == nil)
		xform = UVTRANSFORM_IDENTITY;
	memcpy(uvTransform, xform, sizeof(uvTransform));
}

ObjPipeline*
GetUVTransformPipeline(void)
{
	// rw::platform is whatever the engine was started with. Bounds-checked
	// rather than indexed blind so that asking before Engine::start, or from
	// a build with no backend at all, answers nil instead of reading past
	// the array.
	if(platform < 0 || platform >= NUM_PLATFORMS)
		return nil;
	return uvTransformPipelines[platform];
}

// Im2D

namespace im2d {

float32 GetNearZ(void) { return engine->device.zNear; }
float32 GetFarZ(void) { return engine->device.zFar; }
void
RenderLine(void *verts, int32 numVerts, int32 vert1, int32 vert2)
{
	engine->device.im2DRenderLine(verts, numVerts, vert1, vert2);
}
void
RenderTriangle(void *verts, int32 numVerts, int32 vert1, int32 vert2, int32 vert3)
{
	engine->device.im2DRenderTriangle(verts, numVerts, vert1, vert2, vert3);
}
void
RenderIndexedPrimitive(PrimitiveType type, void *verts, int32 numVerts, void *indices, int32 numIndices)
{
	engine->device.im2DRenderIndexedPrimitive(type, verts, numVerts, indices, numIndices);
}
void
RenderPrimitive(PrimitiveType type, void *verts, int32 numVerts)
{
	engine->device.im2DRenderPrimitive(type, verts, numVerts);
}

}

// Im3D

namespace im3d {

void
Transform(void *vertices, int32 numVertices, Matrix *world, uint32 flags)
{
	engine->device.im3DTransform(vertices, numVertices, world, flags);
}
void
RenderLine(int32 vert1, int32 vert2)
{
	int16 indices[2];
	indices[0] = vert1;
	indices[1] = vert2;
	RenderIndexedPrimitive(rw::PRIMTYPELINELIST, indices, 2);
}
void
RenderTriangle(int32 vert1, int32 vert2, int32 vert3)
{
	int16 indices[3];
	indices[0] = vert1;
	indices[1] = vert2;
	indices[2] = vert3;
	RenderIndexedPrimitive(rw::PRIMTYPETRILIST, indices, 3);
}
void
RenderPrimitive(PrimitiveType primType)
{
	engine->device.im3DRenderPrimitive(primType);
}
void
RenderIndexedPrimitive(PrimitiveType primType, void *indices, int32 numIndices)
{
	engine->device.im3DRenderIndexedPrimitive(primType, indices, numIndices);
}
void
End(void)
{
	engine->device.im3DEnd();
}

}

}

