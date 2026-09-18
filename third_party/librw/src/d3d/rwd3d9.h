namespace rw {

// Defined in rwplugins.h, which several translation units that include this
// header do not. Forward-declared at rw scope, not inside d3d9: a declaration
// in the inner namespace would name a different type of the same name, and the
// error that follows says "incomplete type" rather than anything about scope.
struct MatFXEnvState;

namespace d3d9 {

void registerPlatformPlugins(void);

struct VertexElement
{
	uint16   stream;
	uint16   offset;
	uint8    type;
	uint8    method;
	uint8    usage;
	uint8    usageIndex;
};

// The terminator every vertex declaration ends with. d3d9.h supplies it in a
// D3D9 build; otherwise it is spelled here rather than taken from rwd3d.h's
// portable table, so that this header does not depend on which of the two a
// translation unit included first.
#ifndef RW_D3D9
enum { RWDECLTYPE_UNUSED = 17 };
static VertexElement _rwd3ddecl_end = { 0xFF, 0, RWDECLTYPE_UNUSED, 0, 0, 0 };
#define D3DDECL_END() _rwd3ddecl_end
#endif

struct VertexStream
{
	void  *vertexBuffer;
	uint32 offset;
	uint32 stride;
	uint16 geometryFlags;
	uint8  managed;
	uint8  dynamicLock;
};

struct InstanceData
{
	uint32    numIndex;
	uint32    minVert;
	Material *material;
	bool32    vertexAlpha;
	void     *vertexShader;
	uint32    baseIndex;
	uint32    numVertices;
	uint32    startIndex;
	uint32    numPrimitives;
};

struct InstanceDataHeader : rw::InstanceDataHeader
{
	uint32  serialNumber;
	uint32  numMeshes;
	void   *indexBuffer;
	uint32  primType;
	VertexStream vertexStream[2];
	bool32  useOffsets;
	void   *vertexDeclaration;
	uint32  totalNumIndex;
	uint32  totalNumVertex;

	InstanceData *inst;
};

void *createVertexDeclaration(VertexElement *elements);
void destroyVertexDeclaration(void *delaration);
uint32 getDeclaration(void *declaration, VertexElement *elements);

void drawInst_simple(d3d9::InstanceDataHeader *header, d3d9::InstanceData *inst);
// Emulate PS2 GS alpha test FB_ONLY case: failed alpha writes to frame- but not to depth buffer
void drawInst_GSemu(d3d9::InstanceDataHeader *header, InstanceData *inst);
// This one switches between the above two depending on render state;
void drawInst(d3d9::InstanceDataHeader *header, d3d9::InstanceData *inst);




void *destroyNativeData(void *object, int32, int32);
Stream *readNativeData(Stream *stream, int32 len, void *object, int32, int32);
Stream *writeNativeData(Stream *stream, int32 len, void *object, int32, int32);
int32 getSizeNativeData(void *object, int32, int32);
void registerNativeDataPlugin(void);

class ObjPipeline : public rw::ObjPipeline
{
public:
	void init(void);
	static ObjPipeline *create(void);

	void (*instanceCB)(Geometry *geo, InstanceDataHeader *header, bool32 reinstance);
	void (*uninstanceCB)(Geometry *geo, InstanceDataHeader *header);
	void (*renderCB)(Atomic *atomic, InstanceDataHeader *header);
};

void defaultInstanceCB(Geometry *geo, InstanceDataHeader *header, bool32 reinstance);
void defaultUninstanceCB(Geometry *geo, InstanceDataHeader *header);
void defaultRenderCB_Fix(Atomic *atomic, InstanceDataHeader *header);
// Whether the hull is drawn round one mesh. Shared so the static pipeline and
// the two skinned ones cannot drift on what counts as see-through.
void setOutlineAlpha(bool32 allow);
bool32 outlineTakesMesh(InstanceDataHeader *header, InstanceData *inst);
void defaultRenderCB_Shader(Atomic *atomic, InstanceDataHeader *header);

ObjPipeline *makeDefaultPipeline(void);

// The default pipeline with rw::uvTransform applied to the vertices' texture
// coordinates. Instancing and uninstancing are the default ones -- nothing
// about the vertex buffer changes, which is the whole reason the transform is
// worth doing in the shader.
void uvTransformRenderCB_Shader(Atomic *atomic, InstanceDataHeader *header);
void uvTransformRenderCB_Fix(Atomic *atomic, InstanceDataHeader *header);

ObjPipeline *makeUVTransformPipeline(void);


// Skin plugin

void initSkin(void);
// The bone count skin_VS.hlsl declares, and the most computeSkinMatrices will
// write. A skin with more bones than this was already drawn wrong by the
// shader path; the fixed-function one stops at the same place rather than at a
// different one.
enum { MAXNUMSKINBONES = 64 };
// The bone transforms in object space, as rw matrices. The shader path
// transposes these into its constants; the fixed-function path multiplies
// vertices by them. Writes at most MAXNUMSKINBONES and returns how many.
int32 computeSkinMatrices(Atomic *atomic, Matrix *out);
void uploadSkinMatrices(Atomic *atomic);
void skinInstanceCB(Geometry *geo, InstanceDataHeader *header, bool32 reinstance);
void skinRenderCB(Atomic *atomic, InstanceDataHeader *header);
// The same, skinned on the CPU into a dynamic vertex buffer. Fixed-function
// vertex blending is capped at four matrices per draw on the hardware this
// mode exists for, and the characters have far more bones than that, so the
// blend happens before the vertices reach the device.
void skinRenderCB_Fix(Atomic *atomic, InstanceDataHeader *header);
// The dynamic buffer and the declaration those draws use. Called from skinOpen
// and skinClose, and only under getFixedFunction().
void ffOpenSkin(void);
void ffCloseSkin(void);
ObjPipeline *makeSkinPipeline(void);
extern void *skin_amb_VS;
extern void *skin_amb_dir_VS;
extern void *skin_all_VS;
// Skinning with the lighting left to the pixel shader. Pairs with the _pp_
// pixel shaders in rwd3d.h, as the default pipeline's does.
extern void *skin_pp_VS;
// The skinned inverted hull. Beside its siblings, in this namespace, because
// that is where d3d9skin.cpp defines it.
extern void *skin_outline_VS;

// Skin plugin, combined with MatFX

// The pipeline RenderWare calls rpSKINTYPEMATFX: it skins the vertices AND
// applies the material effect, which neither the skin nor the matfx pipeline
// alone can do. Its shaders and its open/close live with the skin plugin
// because that is which globals hold it, but the env state it uploads is the
// matfx pipeline's, shared rather than copied.
void skinMatfxRenderCB(Atomic *atomic, InstanceDataHeader *header);
ObjPipeline *makeSkinMatFXPipeline(void);
void createSkinMatFXShaders(void);
void destroySkinMatFXShaders(void);

// MatFX plugin

void initMatFX(void);
ObjPipeline *makeMatFXPipeline(void);
void uploadEnvMapState(Texture *envTex, MatFXEnvState *es, int32 vslocBase);
extern void *matfx_env_PS;
extern void *matfx_env_tex_PS;

// Native Texture and Raster

Texture *readNativeTexture(Stream *stream);
void writeNativeTexture(Texture *tex, Stream *stream);
uint32 getSizeNativeTexture(Texture *tex);

}
}
