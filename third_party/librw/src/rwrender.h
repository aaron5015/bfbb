namespace rw {

// Render states

enum RenderState
{
	TEXTURERASTER,
	TEXTUREADDRESS,
	TEXTUREADDRESSU,
	TEXTUREADDRESSV,
	TEXTUREFILTER,
	VERTEXALPHA,
	SRCBLEND,
	DESTBLEND,
	ZTESTENABLE,
	ZWRITEENABLE,
	FOGENABLE,
	FOGCOLOR,
	CULLMODE,
	// TODO:
	// fog type, density ?
	// ? shademode

	STENCILENABLE,
	STENCILFAIL,
	STENCILZFAIL,
	STENCILPASS,
	STENCILFUNCTION,
	STENCILFUNCTIONREF,
	STENCILFUNCTIONMASK,
	STENCILFUNCTIONWRITEMASK,

	// platform specific or opaque?
	ALPHATESTFUNC,
	ALPHATESTREF,

	// emulation of PS2 GS alpha test
	//  in the mode where it still writes color but nor depth
	GSALPHATEST,
	GSALPHATESTREF,

	// Which colour channels a draw is allowed to write, as a mask of
	// COLORWRITE*. Added for the GameCube/PS2 games' depth-priming passes: they
	// draw once with colour writes off to fill the z-buffer, then again with
	// them on, and without this the first pass paints.
	COLORWRITEMASK
};

enum ColorWriteMask
{
	COLORWRITERED   = 1,
	COLORWRITEGREEN = 2,
	COLORWRITEBLUE  = 4,
	COLORWRITEALPHA = 8,
	COLORWRITEALL   = 15
};

enum AlphaTestFunc
{
	ALPHAALWAYS,
	ALPHAGREATEREQUAL,
	ALPHALESS
};

enum StencilOp
{
	STENCILKEEP = 1,
	STENCILZERO,
	STENCILREPLACE,
	STENCILINCSAT,
	STENCILDECSAT,
	STENCILINVERT,
	STENCILINC,
	STENCILDEC
};

enum StencilFunc
{
	STENCILNEVER = 1,
	STENCILLESS,
	STENCILEQUAL,
	STENCILLESSEQUAL,
	STENCILGREATER,
	STENCILNOTEQUAL,
	STENCILGREATEREQUAL,
	STENCILALWAYS
};

enum CullMode
{
	CULLNONE = 1,
	CULLBACK,
	CULLFRONT
};

enum BlendFunction
{
	BLENDZERO = 1,
	BLENDONE,
	BLENDSRCCOLOR,
	BLENDINVSRCCOLOR,
	BLENDSRCALPHA,
	BLENDINVSRCALPHA,
	BLENDDESTALPHA,
	BLENDINVDESTALPHA,
	BLENDDESTCOLOR,
	BLENDINVDESTCOLOR,
	BLENDSRCALPHASAT
	// TODO: add more perhaps
};

void SetRenderState(int32 state, uint32 value);
void SetRenderStatePtr(int32 state, void *value);
uint32 GetRenderState(int32 state);
void *GetRenderStatePtr(int32 state);

// Texture coordinate transform
//
// A 2x4 matrix applied to a vertex's texture coordinates before they are used,
// which is what the GameCube and PS2 call a texgen matrix and what animates a
// scrolling, rotating or scaling surface without touching the geometry. The
// geometry is the point: an instanced model shares one Geometry between every
// Atomic that draws it, so anything written into the vertices animates every
// copy of the model at once. A transform applied at draw time animates the one
// atomic being drawn.
//
// The matrix multiplies the vector (u, v, 1, 1), so row 0 is
// (uu, uv, uconst, uconst2) and the two constant columns BOTH add to u. That is
// deliberately the GameCube's GX_TG_MTX2x4 convention rather than a tidier 2x3,
// because the games that need this were authored against it -- see
// UVTRANSFORM_IDENTITY for what "no transform" is.
//
// This is state, not a material property: it is set for a draw and cleared
// after, in the same way a render state is. Only the pipeline
// GetUVTransformPipeline() returns reads it; every other pipeline ignores it,
// so setting it does not disturb anything else that is drawing.
struct ObjPipeline;

enum { NUMUVTRANSFORMELEMENTS = 8 };

extern float32 uvTransform[NUMUVTRANSFORMELEMENTS];

extern const float32 UVTRANSFORM_IDENTITY[NUMUVTRANSFORMELEMENTS];

// nil resets the transform to the identity.
void SetUVTransform(const float32 *xform);

// The atomic pipeline that applies it, for the platform in use, or nil where
// the platform has no implementation. A caller that gets nil must keep drawing
// with whatever pipeline it already had -- an unanimated surface is the correct
// fallback, and the only other option is not drawing at all.
ObjPipeline *GetUVTransformPipeline(void);

// Each platform's, filled in by that platform's driverOpen and cleared by its
// driverClose, exactly as the default pipeline is. A platform that leaves its
// entry nil is a platform where the transform is not implemented, and that is
// how GetUVTransformPipeline answers nil rather than by knowing which
// platforms those are.
extern ObjPipeline *uvTransformPipelines[NUM_PLATFORMS];

// Im2D

namespace im2d {

float32 GetNearZ(void);
float32 GetFarZ(void);
void RenderLine(void *verts, int32 numVerts, int32 vert1, int32 vert2);
void RenderTriangle(void *verts, int32 numVerts, int32 vert1, int32 vert2, int32 vert3);
void RenderIndexedPrimitive(PrimitiveType, void *verts, int32 numVerts, void *indices, int32 numIndices);
void RenderPrimitive(PrimitiveType type, void *verts, int32 numVerts);

}

// Im3D

namespace im3d {

enum TransformFlags
{
	VERTEXUV      = 1,	// has tex Coords
	ALLOPAQUE     = 2,	// no vertex alpha
	NOCLIP        = 4,	// don't frustum clip
	VERTEXXYZ     = 8,	// has position
	VERTEXRGBA    = 16,	// has color
	LIGHTING      = 32,	// do lighting, assumes normals (librw extension)
	EVERYTHING = VERTEXUV|VERTEXXYZ|VERTEXRGBA
};

void Transform(void *vertices, int32 numVertices, Matrix *world, uint32 flags);
void RenderLine(int32 vert1, int32 vert2);
void RenderTriangle(int32 vert1, int32 vert2, int32 vert3);
void RenderPrimitive(PrimitiveType primType);
void RenderIndexedPrimitive(PrimitiveType primType, void *indices, int32 numIndices);
void End(void);

}

}

