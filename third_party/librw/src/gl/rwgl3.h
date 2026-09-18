#ifdef RW_GL3
#include "glad/glad.h"
#ifdef LIBRW_SDL2
#include <SDL.h>
#elif defined(LIBRW_SDL3)
#include <SDL3/SDL.h>
#elif defined(LIBRW_GLFW)
#include <GLFW/glfw3.h>
#else
// sane fallback
#define LIBRW_GLFW
#include <GLFW/glfw3.h>
#endif
#endif

namespace rw {

namespace gl3 {

#ifdef RW_GL3
struct EngineOpenParams : rw::EngineOpenParams
{
#ifdef LIBRW_SDL2
	SDL_Window **window;
	bool32 fullscreen;
#elif defined(LIBRW_SDL3)
	SDL_Window **window;
	bool32 fullscreen;
#elif defined(LIBRW_GLFW)
	GLFWwindow **window;
#else
    not implemented
#endif
	int width, height;
	const char *windowtitle;
};
#endif

void registerPlatformPlugins(void);

extern Device renderdevice;

// arguments to glVertexAttribPointer basically
struct AttribDesc
{
	uint32 index;
	int32  type;
	bool32 normalized;
	int32  size;
	uint32 stride;
	uint32 offset;
};

enum AttribIndices
{
	ATTRIB_POS = 0,
	ATTRIB_NORMAL,
	ATTRIB_COLOR,
	ATTRIB_WEIGHTS,
	ATTRIB_INDICES,
	ATTRIB_TEXCOORDS0,
	ATTRIB_TEXCOORDS1,
	ATTRIB_TEXCOORDS2,
	ATTRIB_TEXCOORDS3,
	ATTRIB_TEXCOORDS4,
	ATTRIB_TEXCOORDS5,
	ATTRIB_TEXCOORDS6,
	ATTRIB_TEXCOORDS7,
};

// default uniform indices
// The colour the vertices of a non-PRELIT geometry are given, for a platform
// that has no per-vertex colour to read. Zero -- black -- is the default and
// means lighting is the only thing that can brighten such a model. An
// application whose art expects an unlit vertex to start white rather than
// black sets this before the engine is started.
extern bool32 constantVertexColorWhite;

extern int32 u_matColor;
extern int32 u_surfProps;

struct InstanceData
{
	uint32    numIndex;
	uint32    minVert;	// not used for rendering
	int32     numVertices;	//
	Material *material;
	bool32    vertexAlpha;
	uint32    program;
	uint32    offset;
};

struct InstanceDataHeader : rw::InstanceDataHeader
{
	uint32      serialNumber;
	uint32      numMeshes;
	uint16     *indexBuffer;
	uint32      primType;
	uint8      *vertexBuffer;
	int32       numAttribs;
	AttribDesc *attribDesc;
	uint32      totalNumIndex;
	uint32      totalNumVertex;

	uint32      ibo;
	uint32      vbo;		// or 2?
#ifdef RW_GL_USE_VAOS
	uint32      vao;
#endif

	InstanceData *inst;
};

#ifdef RW_GL3

struct Shader;

extern Shader *defaultShader, *defaultShader_noAT;
extern Shader *defaultShader_fullLight, *defaultShader_fullLight_noAT;
extern Shader *uvXformShader, *uvXformShader_noAT;
extern Shader *uvXformShader_fullLight, *uvXformShader_fullLight_noAT;
// The per-pixel lighting path. One pair each and not two, because these do no
// lighting in the vertex shader and so have nothing for DIRECTIONALS to switch
// on. Their fragment shader is not simple.frag as the others' is -- it is
// simple.frag with PERPIXEL, which needs lighting.frag ahead of it.
extern Shader *defaultShader_pp, *defaultShader_pp_noAT;
extern Shader *uvXformShader_pp, *uvXformShader_pp_noAT;
// The caster pass: depth packed into an ordinary colour target. Paired with the
// plain vertex shader, and with skin.vert's in gl3skin.cpp.
extern Shader *depthShader, *depthShader_tex;
// The inverted hull, drawn around a model before the model itself.
extern Shader *outlineShader, *skinOutlineShader;

// Whether the hull is drawn round one mesh. Shared so the static pipeline and
// the two skinned ones cannot drift on what counts as see-through.
void setOutlineAlpha(bool32 allow);
bool32 outlineTakesMesh(InstanceDataHeader *header, InstanceData *inst);

// Draw atomics as depth rather than as a picture, for the shadow map's caster
// pass. While this is on, the default and skin pipelines ignore lighting,
// material colour and texture and write packed depth instead.
//
// Turn it off again before rendering anything the player sees.
void setDepthPassEnabled(bool32 enable);
bool32 getDepthPass(void);

// What a receiver is tested with, all of it already in the map's own units so
// the shader never has to know how deep the light volume is.
//
// `bias` is subtracted from the receiver's own depth before comparing, in the
// 0..1 the map stores. `slopeBias` is added to it once per unit of tan of the
// angle between the surface and the light, which is what one texel of the map
// costs in depth as a surface tilts away. `texel` is one texel of the map in
// texture coordinates, the spacing the filter taps at. `strength` is what a
// fully shadowed pixel is multiplied by: 1 is no shadow, 0 is black.
struct ShadowMapParams
{
	float32 bias;
	float32 slopeBias;
	float32 texel;
	float32 strength;
};

// Hand the receivers a shadow map to test against, and the transform that puts
// a world position into it. Set once a frame, after the caster pass; the
// uniform registry replays it onto every shader that reads it.
//
// `matrix` is the light camera's projection times its view, in the same layout
// the shaders take u_proj and u_view in -- so the receiver's lookup is built
// from exactly what rasterised the casters.
//
// `lightDir` is where the light travels, from it towards what it lights. It is
// what lets a receiver with a normal skip the test on a surface facing away
// from the light -- which is not an optimisation but the cure for the acne that
// storing back faces leaves behind -- and what the slope bias is measured
// against.
//
// nil clears the map, as does clearShadowMap.
void setShadowMap(Texture *tex, float32 *matrix, float32 *lightDir,
                  const ShadowMapParams *params);
void clearShadowMap(void);

extern int32 u_shadowMatrix;
extern int32 u_shadowParams;
extern int32 u_shadowParams2;
extern int32 u_shadowLightDir;
extern int32 u_toonParams;
extern int32 u_outlineColor;
extern int32 u_outlineColor2;
extern int32 u_toonLightDir;
extern int32 u_outlineFlags;
extern int32 u_toonRoomTint;
extern int32 u_toonExtra;

enum OutlineMode
{
	// No hull. The default, and what everything the application does not
	// speak up about gets.
	OUTLINE_NONE = 0,
	// One ink over the whole model.
	OUTLINE_PLAIN,
	// Two, split by height -- see setOutlineLower.
	OUTLINE_TWOTONE
};

// Light what is drawn next from a fixed direction of the application's
// choosing rather than from the scene's lights, keeping their colour. For
// characters, whose shading in a cartoon describes their shape and not the room
// -- see u_toonLightDir in header.vert.
// How many shades a character's colours are cut down to, keeping their hue. 0
// leaves them alone.
//
// A character in the show is drawn flat and bounded; the world is not touched,
// because a painted background does not want its colours rounded.
void setToonFlatten(float32 colors);

// The rest of the look, none of which is lighting.
//
//   wrap       how far the light term is carried round the far side, 0 for the
//              plain lambert that collapses all of it into one value.
//   rim        how bright an edge of light runs along the silhouette.
//   rimEdge    how far round the silhouette that edge starts.
//   occlusion  how far the colour baked into a model darkens its own shading.
//   hardness   how far the shading normal is pulled back towards the face's
//              own, undoing what welding the outline normals softened.
//
// Characters only, all of it, apart from the wrap.
void setToonLook(float32 wrap, float32 rim, float32 rimEdge, float32 occlusion,
                 float32 hardness);

// A hard highlight on a surface the light bounces off rather than scatters into.
//
//   amount  how far the pixel is carried towards white where the glint lands.
//   edge    how far round the half vector the glint starts.
//
// 0 is off, and off is where every draw starts: this is asked for by the few
// surfaces that are wet, not by characters. See ToonGlossAmount in header.frag.
void setToonGloss(float32 amount, float32 edge);

// How the rim light is put on.
//
//   0  towards the room's colour. A replacement: at full amount the band IS
//      the room, so whatever the surface was doing stops at its edge.
//   1  screened over the surface. Brightens by what is left of the range
//      rather than by a fixed amount, so the texture reads through the band,
//      and it cannot leave the range however bright either side is.
//   2  added, and clamped. The brightest of the three and the only one that
//      clips.
void setToonRimBlend(int32 mode);

// Whether the next draw takes the cel look although no light is on it.
//
// The look reads no light array -- the key direction and the room colour are
// resolved before the draw -- but a draw with no lights is usually art that
// wants nothing done to it, and the light count stood in for that. The goo is
// where the two come apart: a wide surface of liquid, drawn with no light kit,
// that wants bands more than anything else in a scene.
void setToonUnlit(bool32 on);
bool32 getToonUnlit(void);

// What colour it is in here, without also saying the draw is a character. A draw
// with no lights has nothing to resolve a room colour from.
void setToonRoomColor(float32 r, float32 g, float32 b);

// Which of the stacked ramps the next draw is shaded with. Skin does not band
// like sheet metal, and the strip holds a row for each.
void setToonRampRow(int32 row);

// Paint what is drawn next in the colour of the room, rather than in the
// colour of the lights that happen to reach it. A level lights its world and
// its objects with different rigs; a cartoon does not.
//
// This also says the next draw IS a character -- nothing else is ever given a
// room -- which is what gates the flattening, the rim and the rest.
void setToonRoomTint(float32 r, float32 g, float32 b);
void clearToonRoomTint(void);

void setToonLightDir(float32 x, float32 y, float32 z);
void clearToonLightDir(void);

// Whether what is drawn next gets an inverted hull around it, and with how many
// inks. Set per draw by the application and cleared after; there is no way to
// tell a character from a prop by looking at its geometry.
void setOutlineMode(int32 mode);
int32 getOutlineMode(void);

// The ink and how far out the hull is pushed, in world units. Thickness 0 turns
// the whole thing off whatever the mode says.
void setOutline(float32 r, float32 g, float32 b, float32 thickness);

// A second ink for the lower part of a model, and the object-space height
// where the two meet. The renderer sets the height per atomic, because it
// belongs to the model rather than to the setting; a height below every vertex
// means one ink everywhere, which is the default.
void setOutlineLower(float32 r, float32 g, float32 b);

// Whether each ink is a colour in its own right or a scale applied to the
// surface it surrounds. Scaled suits a character whose ink is a darker version
// of himself, which is most of them; flat suits one whose is not.
void setOutlineFlat(bool32 upper, bool32 lower);
// How much of the shade the application traced from its own models to take.
void setToonModelShade(float32 amount);

// The stretch on an ink that is a scale on the surface. outline.frag says what
// each of the two does.
void setOutlineInk(float32 saturation, float32 gamma);

// How bright the room is, as a scale on the colour the lights resolve to. 1 is
// the lights' own answer. gl3device.cpp says why they cannot answer it alone.
void setToonRoomScale(float32 scale);

void setOutlineSplit(float32 y);

// Whether the next hull is drawn for a model wound inside out: inflated
// along the negated normal, with back faces culled instead of front ones.
// Together those treat the mesh as wound the other way, which it is.
void setOutlineInverted(bool32 on);
void setOutlineDepthBias(float32 widths);
bool32 getOutlineInverted(void);

// A floor under the hull's width, in world units per unit of view depth, so a
// distant character keeps a line instead of losing it below a pixel. The
// application works the number out; it needs the camera and the render size.
void setOutlineMinWidth(float32 perDepth);
void setOutlineMaxWidth(float32 perDepth);

// The strip of colour the light term looks up in place of being multiplied in
// directly -- band count, widths and colours all live in the texture. nil
// leaves the stage as it was.
void setToonRamp(Texture *tex);

// How much brighter than authored every light from a light kit burns. 1 is as
// the level says. Applied on the way to the uniform, so nothing the
// application owns is modified. Safe before the device exists.
void setLightIntensity(float32 scale);
float32 getLightIntensity(void);

// Draw in the stylised look: the light cut into `bands` steps instead of a
// smooth ramp, and colour pushed away from grey by `saturation` -- 1 leaves it
// alone, above 1 pushes outward. Off until this is called.
//
// The banding is applied to the LIGHT and the saturation to the final colour,
// which is the difference between a drawing and a posterised photograph.
// `strength` dials the whole stylised shading against the plain lighting: 0 is
// the game as it was, 1 is the full cartoon.
void setToonShading(bool32 enable, float32 bands, float32 saturation, float32 strength);
bool32 getToonShading(void);

// Evaluate lighting per fragment rather than per vertex, in the default,
// uvxform and skin pipelines. Directional lights only: an atomic reached by a
// point or spot light keeps the per-vertex path for that draw, and one lit by
// ambient alone has nothing to gain. Safe to call before the device exists.
void setPerPixelLightingEnabled(bool32 enable);
bool32 getPerPixelLighting(void);
extern int32 u_uvXform;

struct Im3DVertex
{
	V3d     position;
	V3d     normal;		// librw extension
	// Blue first. D3D packs the colour as one D3DCOLOR word, which is ARGB and
	// so BGRA from the low byte up, and the vertex declaration it builds from
	// offsetof is what an application's own mirrored vertex has to match. One
	// order for every backend means one struct on that side rather than a
	// #ifdef per colour field; the attribute below reads it back as GL_BGRA.
	uint8   b, g, r, a;
	float32 u, v;

	void setX(float32 x) { this->position.x = x; }
	void setY(float32 y) { this->position.y = y; }
	void setZ(float32 z) { this->position.z = z; }
	void setNormalX(float32 x) { this->normal.x = x; }
	void setNormalY(float32 y) { this->normal.y = y; }
	void setNormalZ(float32 z) { this->normal.z = z; }
	void setColor(uint8 r, uint8 g, uint8 b, uint8 a) {
		this->r = r; this->g = g; this->b = b; this->a = a; }
	void setU(float32 u) { this->u = u; }
	void setV(float32 v) { this->v = v; }

	float getX(void) { return this->position.x; }
	float getY(void) { return this->position.y; }
	float getZ(void) { return this->position.z; }
	float getNormalX(void) { return this->normal.x; }
	float getNormalY(void) { return this->normal.y; }
	float getNormalZ(void) { return this->normal.z; }
	RGBA getColor(void) { return makeRGBA(this->r, this->g, this->b, this->a); }
	float getU(void) { return this->u; }
	float getV(void) { return this->v; }
};
extern RGBA im3dMaterialColor;
extern SurfaceProperties im3dSurfaceProps;

struct Im2DVertex
{
	float32 x, y, z, w;
	// Blue first. D3D packs the colour as one D3DCOLOR word, which is ARGB and
	// so BGRA from the low byte up, and the vertex declaration it builds from
	// offsetof is what an application's own mirrored vertex has to match. One
	// order for every backend means one struct on that side rather than a
	// #ifdef per colour field; the attribute below reads it back as GL_BGRA.
	uint8   b, g, r, a;
	float32 u, v;

	void setScreenX(float32 x) { this->x = x; }
	void setScreenY(float32 y) { this->y = y; }
	void setScreenZ(float32 z) { this->z = z; }
	// This is a bit unefficient but we have to counteract GL's divide, so multiply
	void setCameraZ(float32 z) { this->w = z; }
	void setRecipCameraZ(float32 recipz) { this->w = 1.0f/recipz; }
	void setColor(uint8 r, uint8 g, uint8 b, uint8 a) {
		this->r = r; this->g = g; this->b = b; this->a = a; }
	void setU(float32 u, float recipz) { this->u = u; }
	void setV(float32 v, float recipz) { this->v = v; }

	float getScreenX(void) { return this->x; }
	float getScreenY(void) { return this->y; }
	float getScreenZ(void) { return this->z; }
	float getCameraZ(void) { return this->w; }
	float getRecipCameraZ(void) { return 1.0f/this->w; }
	RGBA getColor(void) { return makeRGBA(this->r, this->g, this->b, this->a); }
	float getU(void) { return this->u; }
	float getV(void) { return this->v; }
};

void setAttribPointers(AttribDesc *attribDescs, int32 numAttribs);
void disableAttribPointers(AttribDesc *attribDescs, int32 numAttribs);
void setupVertexInput(InstanceDataHeader *header);
void teardownVertexInput(InstanceDataHeader *header);

// Render state

// Vertex shader bits
enum
{
	// These should be low so they could be used as indices
	VSLIGHT_DIRECT	= 1,
	VSLIGHT_POINT	= 2,
	VSLIGHT_SPOT	= 4,
	VSLIGHT_MASK	= 7,	// all the above
	// less critical
	VSLIGHT_AMBIENT = 8,
};

// A fixed-size screen, scaled onto the window at present time.
//
// Without one, a Raster::CAMERA IS the default framebuffer: the viewport
// follows the window and the camera's projection does not, so resizing the
// window stretches the picture. With one, the camera raster gets an FBO of its
// own at its own size, everything draws into that, and showRaster blits it into
// the window as the largest rectangle of its shape that fits -- centred, with
// the rest black. Same contract as the D3D9 device's setVirtualScreen.
//
// Call it before the game creates its camera raster, and never with a size that
// disagrees with the raster: the raster's own width and height are what the FBO
// is built at, so a mismatch would silently render at the wrong size rather
// than fail.
//
// Zero, the default, means no virtual screen and the behaviour above it.
void setVirtualScreen(int32 width, int32 height);
extern int32 virtualScreenWidth, virtualScreenHeight;

// The ONE framebuffer every Raster::CAMERA draws into when there is a virtual
// screen, and the texture behind it. Created on first use, because
// setVirtualScreen is called before there is a GL context to create it in.
// Zero when there is no virtual screen.
//
// Device-wide and shared, exactly as the D3D9 virtual screen is one surface
// that every camera raster points at rather than one per raster. An
// application makes several cameras -- a movie player, an offscreen instancer,
// a dummy for bucket sorting -- and they all draw onto the same picture.
// Copy the frame as it stands into a Raster::CAMERATEXTURE, which is how an
// effect gets the picture it is about to distort, blur or hold on screen. The
// source is the virtual screen, NOT whatever framebuffer happens to be bound:
// the last thing drawn before a present is often a camera texture -- a shadow
// buffer, say -- and reading that would capture the shadow buffer.
//
// False when there is no virtual screen to read, when the destination is not a
// camera texture, or when the two disagree about size. A caller that gets
// false has no picture and must fall back, not draw whatever was there before.
bool32 copyVirtualScreen(Raster *dst);

// The virtual screen's depth, copied into a texture and bound to a texture
// stage so a shader can read it. For looking at what the depth test did;
// nothing in a game needs it. False when there is no virtual screen, or on
// GLES. Every call re-copies, so it is one blit per use.
bool32 bindVirtualScreenDepth(int32 stage);
void unbindVirtualScreenDepth(int32 stage);

uint32 virtualScreenFramebuffer(void);
uint32 virtualScreenTexture(void);
// Where to read the frame, as against virtualScreenFramebuffer, which is where
// to draw it. They differ only when the scene is drawn multisampled.
uint32 virtualScreenResolvedFramebuffer(void);
void destroyVirtualScreen(void);

// How many samples the scene is drawn with. Set before the engine opens; the
// count the driver granted is what getVirtualScreenSamples answers, and it is
// 1 when there is no multisampling.
void setVirtualScreenSamples(int32 samples);
int32 getVirtualScreenSamples(void);
// Fold the samples into the single-sample picture every reader of the frame
// sees. Called for you by showRaster and by copyVirtualScreen.
void resolveVirtualScreen(void);

// Bracket a 2D primitive, which has no place in the depth buffer and whose
// edges are placed rather than found. See setIm2DActive's own comment.
void setIm2DActive(bool32 active);

extern const char *shaderDecl;	// #version stuff
extern const char *header_vert_src;
extern const char *header_frag_src;
// The im2d vertex stage. An application's own full-screen pass has to link
// against this one; see the note above it in gl3immed.cpp.
extern const char *im2d_vert_src;

extern Shader *im2dOverrideShader;

// per Scene
void setProjectionMatrix(float32*);
void setViewMatrix(float32*);

// per Object
void setWorldMatrix(Matrix*);
int32 setLights(WorldLights *lightData);

// per Mesh
void setTexture(int32 n, Texture *tex);
// Object pipelines announce what the instanced geometry needs here; it is
// combined with the application's own VERTEXALPHA request, never replaces it.
void setPipelineVertexAlpha(bool32 enable);
void setMaterial(const RGBA &color, const SurfaceProperties &surfaceprops, float extraSurfProp = 0.0f);
inline void setMaterial(uint32 flags, const RGBA &color, const SurfaceProperties &surfaceprops, float extraSurfProp = 0.0f)
{
	static RGBA white = { 255, 255, 255, 255 };
	if(flags & Geometry::MODULATE)
		setMaterial(color, surfaceprops, extraSurfProp);
	else
		setMaterial(white, surfaceprops, extraSurfProp);
}

void setAlphaBlend(bool32 enable);
bool32 getAlphaBlend(void);

bool32 getAlphaTest(void);

uint32 bindFramebuffer(uint32 fbo);
uint32 bindTexture(uint32 texid);
// Put GL_FRAMEBUFFER back to the cached binding after binding the read and draw
// targets separately.
void rebindFramebuffer(void);
// Delete through these rather than glDelete*: GL reverts a binding to 0 when
// the bound object goes, a later object can be handed the same name, and a
// cache still holding the name would then skip binding it.
void deleteFramebuffer(uint32 fbo);
void deleteTexture(uint32 texid);

void flushCache(void);

#endif

class ObjPipeline : public rw::ObjPipeline
{
public:
	void init(void);
	static ObjPipeline *create(void);

	void (*instanceCB)(Geometry *geo, InstanceDataHeader *header, bool32 reinstance);
	void (*uninstanceCB)(Geometry *geo, InstanceDataHeader *header);
	void (*renderCB)(Atomic *atomic, InstanceDataHeader *header);
	// What to draw with instead while setDepthPassEnabled is on.
	//
	// A separate callback and not a branch inside renderCB, because the answer
	// belongs to the pipeline: a skinned one has to move its vertices first, an
	// env-mapped one has nothing to add to a depth value and uses the plain
	// one. nil means this pipeline does not cast, which is a safe default -- a
	// pipeline added later is left out of the shadow map rather than
	// dereferencing a world the caster camera does not have.
	void (*depthRenderCB)(Atomic *atomic, InstanceDataHeader *header);
};

void defaultInstanceCB(Geometry *geo, InstanceDataHeader *header, bool32 reinstance);
void defaultUninstanceCB(Geometry *geo, InstanceDataHeader *header);
void defaultRenderCB(Atomic *atomic, InstanceDataHeader *header);
void uvTransformRenderCB(Atomic *atomic, InstanceDataHeader *header);
// The caster pass for anything whose vertices are already where they belong.
// The skin pipeline has its own; everything else uses this, matfx included.
void defaultRenderDepthCB(Atomic *atomic, InstanceDataHeader *header);
int32 lightingCB(Atomic *atomic);
int32 lightingCB(void);

void drawInst_simple(InstanceDataHeader *header, InstanceData *inst);
// Emulate PS2 GS alpha test FB_ONLY case: failed alpha writes to frame- but not to depth buffer
void drawInst_GSemu(InstanceDataHeader *header, InstanceData *inst);
// This one switches between the above two depending on render state;
void drawInst(InstanceDataHeader *header, InstanceData *inst);


void *destroyNativeData(void *object, int32, int32);

ObjPipeline *makeDefaultPipeline(void);
ObjPipeline *makeUVTransformPipeline(void);

// Native Texture and Raster

struct Gl3Raster
{
	// arguments to glTexImage2D
	int32 internalFormat;
	int32 type;
	int32 format;
	int32 bpp;	// bytes per pixel
	// texture object
	uint32 texid;

	bool isCompressed;
	bool hasAlpha;
	// One of rw::AlphaKind: what the texels actually hold, as opposed to what
	// the pixel format has room for. ALPHAGRADED is the safe default -- it is
	// how every raster with an alpha channel behaved before this existed.
	uint8 alphaKind;
	bool autogenMipmap;
	int8 numLevels;
	// The GL_TEXTURE_MAX_LEVEL set on the texture: the highest level uploaded.
	int8 maxLevel;
	// cached filtermode and addressing
	uint8 filterMode;
	uint8 addressU;
	uint8 addressV;
	int32 maxAnisotropy;

	uint32 fbo;		// used for camera texture only!
	Raster *fboMate;	// color or zbuffer raster mate of this one
	// Whether the fbo above has been checked for completeness. Once is enough
	// and once is all it can afford -- see setFrameBuffer, which explains why
	// the check exists at all.
	uint8 fboChecked;
	RasterLevels *backingStore;	// if we can't read back GPU memory but have to
};

struct Gl3Caps
{
	int gles;
	int glversion;
	bool dxtSupported;
	bool astcSupported;	// not used yet
	float maxAnisotropy;
};
extern Gl3Caps gl3Caps;
// GLES can't read back textures very nicely.
// In most cases that's not an issue, but when it is,
// this has to be set before the texture is filled:
extern bool32 needToReadBackTextures;

void allocateDXT(Raster *raster, int32 dxt, int32 numLevels, bool32 hasAlpha);
void setRasterAlphaKind(Raster *raster, int32 kind);

Texture *readNativeTexture(Stream *stream);
void writeNativeTexture(Texture *tex, Stream *stream);
uint32 getSizeNativeTexture(Texture *tex);

extern int32 nativeRasterOffset;
void registerNativeRaster(void);
#define GETGL3RASTEREXT(raster) PLUGINOFFSET(Gl3Raster, raster, rw::gl3::nativeRasterOffset)

}
}
