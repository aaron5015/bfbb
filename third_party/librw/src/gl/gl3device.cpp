#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

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

#define PLUGIN_ID 0

namespace rw {
namespace gl3 {

GlGlobals glGlobals;

Gl3Caps gl3Caps;
// terrible hack for GLES
bool32 needToReadBackTextures;

int32   alphaFunc;
float32 alphaRef;

struct UniformState
{
	float32 alphaRefLow;
	float32 alphaRefHigh;
	int32   pad[2];

	float32 fogStart;
	float32 fogEnd;
	float32 fogRange;
	float32 fogDisable;

	RGBAf   fogColor;
};

struct UniformScene
{
	float32 proj[16];
	float32 view[16];
};

#define MAX_LIGHTS 8

struct UniformObject
{
	RawMatrix    world;
	RGBAf        ambLight;
	struct {
		float type;
		float radius;
		float minusCosAngle;
		float hardSpot;
	} lightParams[MAX_LIGHTS];
	V4d lightPosition[MAX_LIGHTS];
	V4d lightDirection[MAX_LIGHTS];
	RGBAf lightColor[MAX_LIGHTS];
};

const char *shaderDecl120 =
"#version 120\n"
"#define GL2\n"
"#define texture texture2D\n"
"#define VSIN(index) attribute\n"
"#define VSOUT varying\n"
"#define FSIN varying\n"
"#define FRAGCOLOR(c) (gl_FragColor = c)\n";
const char *shaderDecl330 =
"#version 330\n"
"#define VSIN(index) layout(location = index) in\n"
"#define VSOUT out\n"
"#define FSIN in\n"
"#define FRAGCOLOR(c) (fragColor = c)\n";
const char *shaderDecl100es =
"#version 100\n"
// fwidth and dFdx are core from 3.0 and an extension before it. A vertex
// shader has no derivatives, and "enable" on an unsupported extension is a
// warning rather than an error, so one preamble still serves both stages.
"#extension GL_OES_standard_derivatives : enable\n"
"#define GL2\n"
"#define texture texture2D\n"
"#define VSIN(index) attribute\n"
"#define VSOUT varying\n"
"#define FSIN varying\n"
"#define FRAGCOLOR(c) (gl_FragColor = c)\n"
"precision highp float;\n"
"precision highp int;\n";
const char *shaderDecl300es =
"#version 300 es\n"
"#define VSIN(index) layout(location = index) in\n"
"#define VSOUT out\n"
"#define FSIN in\n"
"#define FRAGCOLOR(c) (fragColor = c)\n"
"precision highp float;\n"
"precision highp int;\n";
const char *shaderDecl310es =
"#version 310 es\n"
"#define VSIN(index) layout(location = index) in\n"
"#define VSOUT out\n"
"#define FSIN in\n"
"#define FRAGCOLOR(c) (fragColor = c)\n"
"precision highp float;\n"
"precision highp int;\n";

const char *shaderDecl;

// this needs a define in the shaders as well!
//#define RW_GL_USE_UBOS

static GLuint vao;
#ifdef RW_GL_USE_UBOS
static GLuint ubo_state, ubo_scene, ubo_object;
#endif
static GLuint whitetex;
static UniformState uniformState;
static UniformScene uniformScene;
static UniformObject uniformObject;

#ifndef RW_GL_USE_UBOS
// State
int32 u_alphaRef;
int32 u_fogData;
int32 u_fogColor;

// Scene
int32 u_proj;
int32 u_view;

// Object
int32 u_world;
// Outside the Object block on purpose: it is a plain uniform in both the UBO
// and the non-UBO build, so adding it did not have to change the block layout.
int32 u_normal;
int32 u_ambLight;
int32 u_lightParams;
int32 u_lightPosition;
int32 u_lightDirection;
int32 u_lightColor;
#endif

int32 u_matColor;
int32 u_surfProps;
int32 u_shadowMatrix;
int32 u_shadowParams;
int32 u_shadowParams2;
int32 u_shadowLightDir;
int32 u_toonParams;
int32 u_outlineColor;
int32 u_outlineColor2;
int32 u_toonLightDir;
int32 u_outlineFlags;
int32 u_outlineSign;
int32 u_outlineInk;
int32 u_toonRoomTint;
int32 u_toonExtra;
int32 u_toonExtra2;
int32 u_toonExtra3;

bool32 constantVertexColorWhite;

int32 virtualScreenWidth;
int32 virtualScreenHeight;

// The single-sample picture, and the framebuffer that owns it. This is what
// every reader of the frame sees -- the blit to the window, the loading-screen
// still -- whether or not the scene was drawn multisampled.
static uint32 virtualScreenFbo;
static uint32 virtualScreenTex;

// The multisampled pair the scene is actually drawn into, when there is one,
// resolved into virtualScreenFbo on the way out. Zero at one sample per pixel,
// and then the scene is drawn straight into virtualScreenFbo.
static uint32 virtualScreenDepth;
static uint32 virtualScreenMSFbo;
static uint32 virtualScreenMSColor;
static uint32 virtualScreenMSDepth;

// What was asked for, and what the driver granted. They differ when the card
// will not take the count; falling back is not a failure, it is the same
// picture drawn with fewer samples.
static int32 virtualScreenSamplesWanted = 1;
static int32 virtualScreenSamples = 1;

void
setVirtualScreen(int32 width, int32 height)
{
	virtualScreenWidth = width;
	virtualScreenHeight = height;
}

// How many samples the scene is drawn with. Takes effect the next time the
// framebuffer is built, which is why the port calls it before the engine opens.
void
setVirtualScreenSamples(int32 samples)
{
	virtualScreenSamplesWanted = samples < 1 ? 1 : samples;
}

int32
getVirtualScreenSamples(void)
{
	return virtualScreenSamples;
}

Shader *defaultShader, *defaultShader_noAT;
Shader *defaultShader_fullLight, *defaultShader_fullLight_noAT;
Shader *uvXformShader, *uvXformShader_noAT;
Shader *uvXformShader_fullLight, *uvXformShader_fullLight_noAT;
Shader *defaultShader_pp, *defaultShader_pp_noAT;
Shader *uvXformShader_pp, *uvXformShader_pp_noAT;
Shader *depthShader;
Shader *outlineShader, *skinOutlineShader;
Shader *depthShader_tex;

static bool32 perPixelLighting;
static bool32 depthPass;

void
setDepthPassEnabled(bool32 enable)
{
	depthPass = !!enable;
}

// The shadow map every receiver tests itself against, and the transform that
// takes a world position into it.
//
// Set once a frame, not per draw. setUniform stores into a registry that
// flushUniforms replays onto whichever shader is next used, so one call reaches
// every program that reads it.
//
// The map goes to texture unit 2 because Shader::create binds tex0..tex3 to
// units 0..3 by name, unit 0 is the material's own texture and unit 1 is
// matfx's environment map.
//
// **Filtering must be nearest.** The map holds depth packed across three bytes,
// and a linear filter would average the bytes of two unrelated depths and
// produce a value that is neither. That is the price of packing rather than
// using a depth texture, and it is why header.frag compares nine taps by hand
// instead of asking the hardware for a filtered comparison.
void
setShadowMap(Texture *tex, float32 *matrix, float32 *lightDir, const ShadowMapParams *params)
{
	setTexture(2, tex);

	setUniform(u_shadowMatrix, matrix);

	float32 dir[4];
	dir[0] = lightDir[0];
	dir[1] = lightDir[1];
	dir[2] = lightDir[2];
	dir[3] = 0.0f;
	setUniform(u_shadowLightDir, dir);

	float32 p[4];
	p[0] = tex ? 1.0f : 0.0f;
	p[1] = params->bias;
	p[2] = params->strength;
	p[3] = params->slopeBias;
	setUniform(u_shadowParams, p);

	float32 p2[4];
	p2[0] = params->texel;
	p2[1] = 0.0f;
	p2[2] = 0.0f;
	p2[3] = 0.0f;
	setUniform(u_shadowParams2, p2);
}

// The stylised look. Set once, and read by every shader that draws a lit
// surface -- the world and the characters alike, because a cartoon that only
// applied to one of them would look like a bug.
//
// **Held, not pushed, until the uniform exists.** The application sets this
// beside setPerPixelLightingEnabled, which is before Engine::open, and until
// open runs there is no uniform registry to write into -- u_toonParams is
// still zero, and setUniform would happily write these four floats over
// whichever uniform registered first. So the values are kept here and the
// registration site pushes them; a call after the device is up pushes
// immediately, as a caller would expect.
// How much brighter than authored every light burns.
//
// The kits were lit for a television in 2003 and read dark on a modern
// display, and there is nowhere else to put this: the colours come out of the
// level's own assets, and scaling them there would mean writing to the asset.
// Scaled on the way to the uniform instead, so nothing the game owns changes.
//
// The shader clamps after summing, so a scale that overshoots flattens the
// brightest surfaces to white rather than wrapping.
static float32 lightIntensity = 1.0f;

void
setLightIntensity(float32 scale)
{
	if(scale < 0.0f)
		scale = 0.0f;

	lightIntensity = scale;
}

float32
getLightIntensity(void)
{
	return lightIntensity;
}

// Whether the uniform registry exists yet. Both setters below are called before
// Engine::open, and until it runs there is nothing to write into -- see
// setToonShading, which explains what happens if you try.
static bool32 toonRegistered;

// Whether each ink is a colour outright or a scale on the surface. See
// u_outlineFlags.
static float32 outlineFlags[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

// x is +1 for a hull pushed out of the surface, -1 for one pushed into it.
static float32 outlineSign[4] = { 1.0f, 0.0f, 0.0f, 0.0f };

// The stretch on a scaled ink: saturation, then a curve. 1 and 1 is the plain
// multiply this had before either existed.
static float32 outlineInk[4] = { 1.0f, 1.0f, 0.0f, 0.0f };

// How bright the room is, which the lights cannot say. See the D3D9 twin in
// d3drender.cpp.
static float32 toonRoomScale = 1.0f;

// What colour it is in here. Either the room the scene handed over for a
// character or, failing that, what setLights worked out from the lights
// themselves -- so it always holds an answer and the shader never has to ask.
//
// **w is not the switch any more.** It was, back when an unset tint meant the
// shader should go and sum the lights itself; now that nothing is unset, which
// draws are characters is a separate fact and lives in u_toonExtra.z.
static float32 toonRoomTint[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

// Whether the scene named the room for this draw. setLights leaves a named one
// alone and fills in the rest.
static bool32 toonRoomSet;

// x how flat a character's colours are cut, y which ramp row he is drawn with,
// z whether this draw is a character at all, w how far the light term is
// wrapped round the far side. See u_toonExtra.
static float32 toonExtra[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

// x rim strength, y where the rim starts, z how far the baked colour darkens
// the lookup, w how hard the shading edges are. See u_toonExtra2.
static float32 toonExtra2[4] = { 0.0f, 0.65f, 0.0f, 0.0f };

// x how strong the glint is, y how far round the half vector it starts, z how
// the rim light is put on. See header.frag.
static float32 toonExtra3[4] = { 0.0f, 0.85f, 0.0f, 0.0f };

static void
pushToonExtra(void)
{
	if(toonRegistered){
		setUniform(u_toonExtra, toonExtra);
		setUniform(u_toonExtra2, toonExtra2);
		setUniform(u_toonExtra3, toonExtra3);
	}
}

void
setToonFlatten(float32 colors)
{
	toonExtra[0] = colors;
	pushToonExtra();
}

void
setToonLook(float32 wrap, float32 rim, float32 rimEdge, float32 occlusion,
            float32 hardness)
{
	toonExtra[3] = wrap;
	toonExtra2[0] = rim;
	toonExtra2[1] = rimEdge;
	toonExtra2[2] = occlusion;
	toonExtra2[3] = hardness;
	pushToonExtra();
}

// A hard highlight, for the draws that are wet. Set per draw and cleared after,
// the way the ramp row is.
// Whether this draw takes the cel look with no lights on it. See the D3D9 twin
// in d3drender.cpp, which says why the light count was ever asked.
static bool32 toonUnlit;

void
setToonUnlit(bool32 on)
{
	toonUnlit = on ? 1 : 0;
}

bool32
getToonUnlit(void)
{
	return toonUnlit;
}

void
setToonGloss(float32 amount, float32 edge)
{
	toonExtra3[0] = amount;
	toonExtra3[1] = edge;
	pushToonExtra();
}

void
setToonRimBlend(int32 mode)
{
	toonExtra3[2] = (float32)mode;
	pushToonExtra();
}

// Which of the stacked ramps this draw reads. Set per atomic by the game, and
// meaningless to anything that is not drawing a character.
void
setToonRampRow(int32 row)
{
	toonExtra[1] = (float32)row;
	pushToonExtra();
}

// The outline's colour, and its thickness in world units in alpha. Zero
// thickness is how the pass is turned off -- see getOutline.
static float32 outlineColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

// The second ink, and in alpha the object-space height below which it is used.
// The height is set per atomic by the renderer, because it is a property of the
// model rather than of the setting.
static float32 outlineColor2[4] = { 0.0f, 0.0f, 0.0f, -1e30f };

// Whether what is about to be drawn gets a hull at all, and whether it gets one
// ink or two.
//
// **Set by the application, per draw, and off by default.** Deciding here from
// what the geometry looks like was the first attempt -- anything skinned is a
// character, near enough -- and it is not near enough: it outlines every
// skinned prop, every cutscene stand-in and anything else that happens to have
// bones. Only the game knows which atomic is a character and which character it
// is, so only the game can say.
static int32 outlineMode;

// Where the light travels, in world space. Either the direction the game named
// for a character -- his own front, or the camera -- or, failing that, the
// brightest directional in the room, resolved by setLights. As with the room
// colour, it always holds an answer.
static float32 toonLightDir[4] = { 0.0f, -1.0f, 0.0f, 1.0f };

// Whether the game named it for this draw.
static bool32 toonLightDirSet;

void
setToonRoomColor(float32 r, float32 g, float32 b)
{
	toonRoomTint[0] = r;
	toonRoomTint[1] = g;
	toonRoomTint[2] = b;
	toonRoomSet = 1;

	if(toonRegistered)
		setUniform(u_toonRoomTint, toonRoomTint);
}

void
setToonRoomTint(float32 r, float32 g, float32 b)
{
	setToonRoomColor(r, g, b);

	// The scene only ever names a room for a CHARACTER, so the same call says
	// so. Everything that is wrong on a background -- the flattening, the rim,
	// the baked occlusion, the hardened normals -- hangs off this. The goo names
	// a room and is not a character, so it calls the half above.
	toonExtra[2] = 1.0f;
	pushToonExtra();
}

void
clearToonRoomTint(void)
{
	toonRoomSet = 0;
	toonExtra[2] = 0.0f;
	pushToonExtra();
}

void
setToonLightDir(float32 x, float32 y, float32 z)
{
	toonLightDir[0] = x;
	toonLightDir[1] = y;
	toonLightDir[2] = z;
	toonLightDirSet = 1;

	if(toonRegistered)
		setUniform(u_toonLightDir, toonLightDir);
}

void
clearToonLightDir(void)
{
	toonLightDirSet = 0;
}

void
setOutlineMode(int32 mode)
{
	outlineMode = mode;
}

int32
getOutlineMode(void)
{
	return outlineColor[3] > 0.0f ? outlineMode : OUTLINE_NONE;
}

void
setOutlineInverted(bool32 on)
{
	outlineSign[0] = on ? -1.0f : 1.0f;

	if(toonRegistered)
		setUniform(u_outlineSign, outlineSign);
}

bool32
getOutlineInverted(void)
{
	return outlineSign[0] < 0.0f;
}

// How far past the hull its depth is read from, in hull widths.
//
// The margin by which the ink loses to a surface the model is nearly touching.
// Zero reads the depth where the hull actually is, which is what lets the copy
// cross into the floor and the ink come out on top of it. See default.vert.
// Below zero reads it from further IN instead, and -1 lands on the vertex the
// hull was pushed from: the model's own surface, with none of the copy's
// standing in space left in it.
void
setOutlineDepthBias(float32 widths)
{
	outlineSign[1] = widths;

	if(toonRegistered)
		setUniform(u_outlineSign, outlineSign);
}

void
setOutlineFlat(bool32 upper, bool32 lower)
{
	outlineFlags[0] = upper ? 1.0f : 0.0f;
	outlineFlags[1] = lower ? 1.0f : 0.0f;

	if(toonRegistered)
		setUniform(u_outlineFlags, outlineFlags);
}

// The floor under the hull's width, as world units per unit of view depth.
//
// The application works it out because the sum needs the camera's view window
// and the size of the picture, and the renderer knows neither. See default.vert
// for what the shader does with it.
void
setOutlineMinWidth(float32 perDepth)
{
	outlineFlags[2] = perDepth < 0.0f ? 0.0f : perDepth;

	if(toonRegistered)
		setUniform(u_outlineFlags, outlineFlags);
}

// The ceiling over it, the same units. Zero for none, which is what a hull with
// no cap gets: it keeps one world width and swells on screen as the camera
// closes.
void
setOutlineMaxWidth(float32 perDepth)
{
	outlineFlags[3] = perDepth < 0.0f ? 0.0f : perDepth;

	if(toonRegistered)
		setUniform(u_outlineFlags, outlineFlags);
}

void
setOutlineLower(float32 r, float32 g, float32 b)
{
	outlineColor2[0] = r;
	outlineColor2[1] = g;
	outlineColor2[2] = b;

	if(toonRegistered)
		setUniform(u_outlineColor2, outlineColor2);
}

// Where the two inks meet on this model, in object space.
//
// Below any vertex means one ink everywhere, which is what a model with no
// second region wants -- and what every model but the player gets, since
// nothing else has pants.
void
setOutlineSplit(float32 y)
{
	outlineColor2[3] = y;

	if(toonRegistered)
		setUniform(u_outlineColor2, outlineColor2);
}

void
setOutline(float32 r, float32 g, float32 b, float32 thickness)
{
	if(thickness < 0.0f)
		thickness = 0.0f;

	outlineColor[0] = r;
	outlineColor[1] = g;
	outlineColor[2] = b;
	outlineColor[3] = thickness;

	if(toonRegistered)
		setUniform(u_outlineColor, outlineColor);
}

// The colour strip the light term looks up. Bound to stage 3 and left there:
// nothing else in the renderer uses that stage, so it survives every draw.
void
setToonRamp(Texture *tex)
{
	setTexture(3, tex);
}

static float32 toonParams[4] = { 0.0f, 3.0f, 1.0f, 0.0f };

void
setToonShading(bool32 enable, float32 bands, float32 saturation, float32 strength)
{
	// One band would leave a surface either fully lit or fully dark with
	// nothing between, which is a silhouette rather than a drawing.
	if(bands < 2.0f)
		bands = 2.0f;

	// toonParams[1] is the model shade now; the band count moved into the ramp
	// texture and nothing read it here. setToonModelShade owns it.
	(void)bands;

	toonParams[0] = enable ? 1.0f : 0.0f;
	toonParams[2] = saturation;
	toonParams[3] = strength;

	if(toonRegistered)
		setUniform(u_toonParams, toonParams);
}

bool32
getToonShading(void)
{
	return toonParams[0] != 0.0f;
}

void
setToonRoomScale(float32 scale)
{
	toonRoomScale = scale;
}

void
setOutlineInk(float32 saturation, float32 gamma)
{
	outlineInk[0] = saturation;
	outlineInk[1] = gamma;

	if(toonRegistered)
		setUniform(u_outlineInk, outlineInk);
}

// How much of the shade the application traced from its own models to take.
void
setToonModelShade(float32 amount)
{
	toonParams[1] = amount;

	if(toonRegistered)
		setUniform(u_toonParams, toonParams);
}

void
clearShadowMap(void)
{
	float32 params[4] = { 0.0f, 0.0f, 1.0f, 0.0f };
	setTexture(2, nil);
	setUniform(u_shadowParams, params);
}

bool32
getDepthPass(void)
{
	return depthPass;
}

void
setPerPixelLightingEnabled(bool32 enable)
{
	perPixelLighting = !!enable;
}

bool32
getPerPixelLighting(void)
{
	return perPixelLighting;
}
int32 u_uvXform;

static bool32 stateDirty = 1;
static bool32 sceneDirty = 1;
static bool32 objectDirty = 1;

struct RwRasterStateCache {
	Raster *raster;
	Texture::Addressing addressingU;
	Texture::Addressing addressingV;
	Texture::FilterMode filter;
};

#define MAXNUMSTAGES 8

// cached RW render states
struct RwStateCache {
	bool32 vertexAlpha;
	// What the APPLICATION last asked for, kept apart from the effective
	// state above. The object pipelines overwrite vertexAlpha per mesh from
	// the instanced geometry, which on its own throws away a blend the app
	// explicitly requested; keeping the request lets them OR it back in.
	bool32 appVertexAlpha;
	uint32 alphaTestEnable;
	uint32 alphaFunc;
	// The alpha test reference the application asked for, in 0..255. The
	// shader takes it as a float in `alphaRef`; this is the same number kept
	// where updateAlphaStates can read it, because the reference is also how
	// the game says a bucket's alpha is a cutout.
	uint32 alpharef;
	bool32 textureAlpha;
	// The bound texture's alpha is keyed rather than graded -- see AlphaKind
	// in rwbase.h. Held apart from textureAlpha because the two answer
	// different questions: whether there is transparency at all, and whether
	// it wants to be cut or blended.
	bool32 textureKeyed;
	bool32 blendEnable;
	uint32 srcblend, destblend;
	uint32 zwrite;
	uint32 ztest;
	uint32 cullmode;
	uint32 stencilenable;
	uint32 stencilpass;
	uint32 stencilfail;
	uint32 stencilzfail;
	uint32 stencilfunc;
	uint32 stencilref;
	uint32 stencilmask;
	uint32 stencilwritemask;
	uint32 fogEnable;
	float32 fogStart;
	float32 fogEnd;

	// emulation of PS2 GS
	bool32 gsalpha;
	uint32 gsalpharef;

	uint32 colorwritemask;

	RwRasterStateCache texstage[MAXNUMSTAGES];
};
static RwStateCache rwStateCache;

enum
{
	// actual gl states
	RWGL_BLEND,
	RWGL_SRCBLEND,
	RWGL_DESTBLEND,
	RWGL_DEPTHTEST,
	RWGL_DEPTHFUNC,
	RWGL_DEPTHMASK,
	RWGL_CULL,
	RWGL_CULLFACE,
	RWGL_STENCIL,
	RWGL_STENCILFUNC,
	RWGL_STENCILFAIL,
	RWGL_STENCILZFAIL,
	RWGL_STENCILPASS,
	RWGL_STENCILREF,
	RWGL_STENCILMASK,
	RWGL_STENCILWRITEMASK,
	RWGL_COLORMASK,
	RWGL_MULTISAMPLE,

	// uniforms
	RWGL_ALPHAFUNC,
	RWGL_ALPHAREF,
	RWGL_FOG,
	RWGL_FOGSTART,
	RWGL_FOGEND,
	RWGL_FOGCOLOR,

	RWGL_NUM_STATES
};
static bool uniformStateDirty[RWGL_NUM_STATES];

struct GlState {
	bool32 blendEnable;
	uint32 srcblend, destblend;

	bool32 depthTest;
	uint32 depthFunc;

	uint32 depthMask;

	bool32 cullEnable;
	uint32 cullFace;

	bool32 stencilEnable;
	// glStencilFunc
	uint32 stencilFunc;
	uint32 stencilRef;
	uint32 stencilMask;
	// glStencilOp
	uint32 stencilPass;
	uint32 stencilFail;
	uint32 stencilZFail;
	// glStencilMask
	uint32 stencilWriteMask;

	// glColorMask, as a mask of ColorWriteMask bits
	uint32 colorMask;

	// Whether the rasterizer finds coverage per sample or gives every sample
	// in a pixel the same answer. Off is what a single-sampled target does,
	// and what a 2D primitive wants -- see setIm2DActive.
	bool32 multisample;
};
static GlState curGlState, oldGlState;

static int32 activeTexture;
static uint32 boundTexture[MAXNUMSTAGES];

static uint32 currentFramebuffer;

static uint32 blendMap[] = {
	GL_ZERO,	// actually invalid
	GL_ZERO,
	GL_ONE,
	GL_SRC_COLOR,
	GL_ONE_MINUS_SRC_COLOR,
	GL_SRC_ALPHA,
	GL_ONE_MINUS_SRC_ALPHA,
	GL_DST_ALPHA,
	GL_ONE_MINUS_DST_ALPHA,
	GL_DST_COLOR,
	GL_ONE_MINUS_DST_COLOR,
	GL_SRC_ALPHA_SATURATE,
};

static uint32 stencilOpMap[] = {
	GL_KEEP,	// actually invalid
	GL_KEEP,
	GL_ZERO,
	GL_REPLACE,
	GL_INCR,
	GL_DECR,
	GL_INVERT,
	GL_INCR_WRAP,
	GL_DECR_WRAP
};

static uint32 stencilFuncMap[] = {
	GL_NEVER,	// actually invalid
	GL_NEVER,
	GL_LESS,
	GL_EQUAL,
	GL_LEQUAL,
	GL_GREATER,
	GL_NOTEQUAL,
	GL_GEQUAL,
	GL_ALWAYS
};

static float maxAnisotropy;

/*
 * GL state cache
 */

void
setGlRenderState(uint32 state, uint32 value)
{
	switch(state){
	case RWGL_BLEND: curGlState.blendEnable = value; break;
	case RWGL_SRCBLEND: curGlState.srcblend = value; break;
	case RWGL_DESTBLEND: curGlState.destblend = value; break;
	case RWGL_DEPTHTEST: curGlState.depthTest = value; break;
	case RWGL_DEPTHFUNC: curGlState.depthFunc = value; break;
	case RWGL_DEPTHMASK: curGlState.depthMask = value; break;
	case RWGL_CULL: curGlState.cullEnable = value; break;
	case RWGL_CULLFACE: curGlState.cullFace = value; break;
	case RWGL_STENCIL: curGlState.stencilEnable = value; break;
	case RWGL_STENCILFUNC: curGlState.stencilFunc = value; break;
	case RWGL_STENCILFAIL: curGlState.stencilFail = value; break;
	case RWGL_STENCILZFAIL: curGlState.stencilZFail = value; break;
	case RWGL_STENCILPASS: curGlState.stencilPass = value; break;
	case RWGL_STENCILREF: curGlState.stencilRef = value; break;
	case RWGL_STENCILMASK: curGlState.stencilMask = value; break;
	case RWGL_STENCILWRITEMASK: curGlState.stencilWriteMask = value; break;
	case RWGL_COLORMASK: curGlState.colorMask = value; break;
	case RWGL_MULTISAMPLE: curGlState.multisample = value; break;
	}
}

// Open the colour, depth and stencil write masks for a clear, and record that
// they are open. GL holds the flushed masks, oldGlState, which differ from the
// requested ones in rwStateCache until the next draw flushes; so the masks are
// opened unconditionally, and that next flush puts back what was asked for.
static void
openWriteMasks(void)
{
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	oldGlState.colorMask = COLORWRITEALL;
	glDepthMask(GL_TRUE);
	oldGlState.depthMask = GL_TRUE;
	glStencilMask(0xFFFFFFFF);
	oldGlState.stencilWriteMask = 0xFFFFFFFF;
}

void
flushGlRenderState(void)
{
	if(oldGlState.blendEnable != curGlState.blendEnable){
		oldGlState.blendEnable = curGlState.blendEnable;
		(oldGlState.blendEnable ? glEnable : glDisable)(GL_BLEND);
	}

	if(oldGlState.srcblend != curGlState.srcblend ||
	   oldGlState.destblend != curGlState.destblend){
		oldGlState.srcblend = curGlState.srcblend;
		oldGlState.destblend = curGlState.destblend;
		glBlendFunc(oldGlState.srcblend, oldGlState.destblend);
	}

	if(oldGlState.depthTest != curGlState.depthTest){
		oldGlState.depthTest = curGlState.depthTest;
		(oldGlState.depthTest ? glEnable : glDisable)(GL_DEPTH_TEST);
	}
	if(oldGlState.depthFunc != curGlState.depthFunc){
		oldGlState.depthFunc = curGlState.depthFunc;
		glDepthFunc(oldGlState.depthFunc);
	}
	if(oldGlState.depthMask != curGlState.depthMask){
		oldGlState.depthMask = curGlState.depthMask;
		glDepthMask(oldGlState.depthMask);
	}

	if(oldGlState.stencilEnable != curGlState.stencilEnable){
		oldGlState.stencilEnable = curGlState.stencilEnable;
		(oldGlState.stencilEnable ? glEnable : glDisable)(GL_STENCIL_TEST);
	}
	if(oldGlState.stencilFunc != curGlState.stencilFunc ||
	   oldGlState.stencilRef != curGlState.stencilRef ||
	   oldGlState.stencilMask != curGlState.stencilMask){
		oldGlState.stencilFunc = curGlState.stencilFunc;
		oldGlState.stencilRef = curGlState.stencilRef;
		oldGlState.stencilMask = curGlState.stencilMask;
		glStencilFunc(oldGlState.stencilFunc, oldGlState.stencilRef, oldGlState.stencilMask);
	}
	if(oldGlState.stencilPass != curGlState.stencilPass ||
	   oldGlState.stencilFail != curGlState.stencilFail ||
	   oldGlState.stencilZFail != curGlState.stencilZFail){
		oldGlState.stencilPass = curGlState.stencilPass;
		oldGlState.stencilFail = curGlState.stencilFail;
		oldGlState.stencilZFail = curGlState.stencilZFail;
		glStencilOp(oldGlState.stencilFail, oldGlState.stencilZFail, oldGlState.stencilPass);
	}
	if(oldGlState.stencilWriteMask != curGlState.stencilWriteMask){
		oldGlState.stencilWriteMask = curGlState.stencilWriteMask;
		glStencilMask(oldGlState.stencilWriteMask);
	}

	if(oldGlState.multisample != curGlState.multisample){
		oldGlState.multisample = curGlState.multisample;
		// GLES has no GL_MULTISAMPLE; a multisampled target always is.
		if(!gl3Caps.gles)
			(oldGlState.multisample ? glEnable : glDisable)(GL_MULTISAMPLE);
	}

	if(oldGlState.colorMask != curGlState.colorMask){
		oldGlState.colorMask = curGlState.colorMask;
		glColorMask(!!(oldGlState.colorMask & COLORWRITERED),
			!!(oldGlState.colorMask & COLORWRITEGREEN),
			!!(oldGlState.colorMask & COLORWRITEBLUE),
			!!(oldGlState.colorMask & COLORWRITEALPHA));
	}

	if(oldGlState.cullEnable != curGlState.cullEnable){
		oldGlState.cullEnable = curGlState.cullEnable;
		(oldGlState.cullEnable ? glEnable : glDisable)(GL_CULL_FACE);
	}
	if(oldGlState.cullFace != curGlState.cullFace){
		oldGlState.cullFace = curGlState.cullFace;
		glCullFace(oldGlState.cullFace);
	}
}



void
setAlphaBlend(bool32 enable)
{
	if(rwStateCache.blendEnable != enable){
		rwStateCache.blendEnable = enable;
		setGlRenderState(RWGL_BLEND, enable);
	}
}

bool32
getAlphaBlend(void)
{
	return rwStateCache.blendEnable;
}

bool32 getAlphaTest(void) { return rwStateCache.alphaTestEnable; }

// Defined below, beside the rest of the transparency states, and called from
// setDepthWrite above it: whether a draw writes depth is one of its inputs.
static void updateAlphaStates(void);

static void
setDepthTest(bool32 enable)
{
	if(rwStateCache.ztest != enable){
		rwStateCache.ztest = enable;
		if(rwStateCache.zwrite && !enable){
			// If we still want to write, enable but set mode to always
			setGlRenderState(RWGL_DEPTHTEST, true);
			setGlRenderState(RWGL_DEPTHFUNC, GL_ALWAYS);
		}else{
			setGlRenderState(RWGL_DEPTHTEST, rwStateCache.ztest);
			setGlRenderState(RWGL_DEPTHFUNC, GL_LEQUAL);
		}
	}
}

static void
setDepthWrite(bool32 enable)
{
	enable = enable ? GL_TRUE : GL_FALSE;
	if(rwStateCache.zwrite != enable){
		rwStateCache.zwrite = enable;
		// Coverage is only claimed by a draw that writes depth, so this is one
		// of its inputs. See updateAlphaStates.
		updateAlphaStates();
		if(enable && !rwStateCache.ztest){
			// Have to switch on ztest so writing can work
			setGlRenderState(RWGL_DEPTHTEST, true);
			setGlRenderState(RWGL_DEPTHFUNC, GL_ALWAYS);
		}
		setGlRenderState(RWGL_DEPTHMASK, rwStateCache.zwrite);
	}
}

static void
setAlphaTest(bool32 enable)
{
	uint32 shaderfunc;
	if(rwStateCache.alphaTestEnable != enable){
		rwStateCache.alphaTestEnable = enable;
		shaderfunc = rwStateCache.alphaTestEnable ? rwStateCache.alphaFunc : ALPHAALWAYS;
		if(alphaFunc != shaderfunc){
			alphaFunc = shaderfunc;
			uniformStateDirty[RWGL_ALPHAFUNC] = true;
			stateDirty = 1;
		}
	}
}

static void
setAlphaTestFunction(uint32 function)
{
	uint32 shaderfunc;
	if(rwStateCache.alphaFunc != function){
		rwStateCache.alphaFunc = function;
		shaderfunc = rwStateCache.alphaTestEnable ? rwStateCache.alphaFunc : ALPHAALWAYS;
		if(alphaFunc != shaderfunc){
			alphaFunc = shaderfunc;
			uniformStateDirty[RWGL_ALPHAFUNC] = true;
			stateDirty = 1;
		}
	}
}

// Set while a 2D primitive is being drawn. A screen-space quad has no place in
// the depth buffer, so it is never a cutout.
static bool32 im2DActive;

void
setIm2DActive(bool32 active)
{
	if(im2DActive != active){
		im2DActive = active;

		// And no edge antialiasing while it lasts. A 2D quad's edges are
		// PLACED, in pixels, rather than found by the rasterizer: a letterbox
		// bar ends where the game said it ends. Antialiasing an edge that lands
		// between two pixel centres turns that line into a row of partly
		// covered samples, which resolves to a soft one you can see through --
		// on a black bar or a fade to black, exactly where the artwork has a
		// hard edge.
		//
		// Disabling GL_MULTISAMPLE gives every sample in the pixel the same
		// coverage answer, which is what a single-sampled target would have
		// done. It is the same statement D3DRS_MULTISAMPLEANTIALIAS makes.
		setGlRenderState(RWGL_MULTISAMPLE, !active);
		updateAlphaStates();
	}
}

// The transparency states, from the two things that can ask for them.
//
// Both used to write the blend and the test themselves, each guarded on the
// other being off so either could hold them on. That is an OR and is written as
// one here, because coverage makes the two sources mean different things: a
// texture's own transparency can be spread over the pixel's samples where the
// application's own request always asks for a blend.
static void
updateAlphaStates(void)
{
	// A texture's own transparency on a draw the application is NOT blending;
	// the reasoning is in the D3D9 device. A keyed raster on a surface that
	// takes part in the depth buffer, and is not a screen-space quad, is cut
	// rather than blended, at the reference the artwork's silhouette sits on.
	bool32 cutout = rwStateCache.textureAlpha && rwStateCache.textureKeyed &&
	                !rwStateCache.vertexAlpha && rwStateCache.zwrite &&
	                !im2DActive;
	bool32 test = rwStateCache.vertexAlpha || rwStateCache.textureAlpha;
	bool32 blend = rwStateCache.vertexAlpha ||
	               (rwStateCache.textureAlpha && !cutout);

	// Where the band is cut. The reasoning is in the D3D9 device; the only
	// difference here is that the reference reaches the shader as a float.
	uint32 wantref = cutout && rwStateCache.alpharef <= 1 ?
	                 (uint32)ALPHACUTOUTREF : rwStateCache.alpharef;
	if(alphaRef != wantref/255.0f){
		alphaRef = wantref/255.0f;
		uniformStateDirty[RWGL_ALPHAREF] = true;
		stateDirty = 1;
	}

	setAlphaBlend(blend);
	setAlphaTest(test);
}

static void
setVertexAlpha(bool32 enable)
{
	if(rwStateCache.vertexAlpha != enable){
		rwStateCache.vertexAlpha = enable;
		updateAlphaStates();
	}
}

// The object pipelines' way in. `enable` is what the instanced geometry needs
// -- vertex alpha in the mesh, or a material that is not fully opaque -- and
// it is OR'd with what the application asked for rather than replacing it.
//
// Without that, a mesh whose own data says "opaque" switches blending off
// again after the app has set up a blend for it, and the src/dest factors it
// just chose never reach the framebuffer. Real RenderWare's console drivers
// leave blending to the application; only the PC pipelines here infer it, so
// the inference has to be additive to the request, not a substitute for it.
void
setPipelineVertexAlpha(bool32 enable)
{
	setVertexAlpha(enable || rwStateCache.appVertexAlpha);
}

static void
setActiveTexture(int32 n)
{
	if(activeTexture != n){
		activeTexture = n;
		glActiveTexture(GL_TEXTURE0+n);
	}
}

uint32
bindTexture(uint32 texid)
{
	uint32 prev = boundTexture[activeTexture];
	if(prev != texid){
		boundTexture[activeTexture] = texid;
		glBindTexture(GL_TEXTURE_2D, texid);
	}
	return prev;
}

uint32
bindFramebuffer(uint32 fbo)
{
	uint32 prev = currentFramebuffer;
	if(currentFramebuffer != fbo){
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		currentFramebuffer = fbo;
	}
	return prev;
}

void
rebindFramebuffer(void)
{
	glBindFramebuffer(GL_FRAMEBUFFER, currentFramebuffer);
}

void
deleteFramebuffer(uint32 fbo)
{
	if(fbo == 0)
		return;
	if(currentFramebuffer == fbo)
		currentFramebuffer = 0;
	glDeleteFramebuffers(1, &fbo);
}

void
deleteTexture(uint32 texid)
{
	if(texid == 0)
		return;
	for(int i = 0; i < MAXNUMSTAGES; i++)
		if(boundTexture[i] == texid)
			boundTexture[i] = 0;
	glDeleteTextures(1, &texid);
}

static GLint filterConvMap_NoMIP[] = {
	0, GL_NEAREST, GL_LINEAR,
	   GL_NEAREST, GL_LINEAR,
	   GL_NEAREST, GL_LINEAR
};
static GLint filterConvMap_MIP[] = {
	0, GL_NEAREST, GL_LINEAR,
	   GL_NEAREST_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_NEAREST,
	   GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR_MIPMAP_LINEAR
};

uint32
virtualScreenTexture(void)
{
	return virtualScreenTex;
}

// Where to READ the frame, as against virtualScreenFramebuffer, which is where
// to draw it. The two differ only when the scene is drawn multisampled.
uint32
virtualScreenResolvedFramebuffer(void)
{
	return virtualScreenFbo;
}

// The multisampled pair, when one was asked for and the driver will grant it.
//
// Both attachments are renderbuffers: a multisample texture needs GL 3.2 and a
// sampler the shaders do not have, and nothing samples this surface -- it is
// resolved into virtualScreenTex, which is what readers see.
//
// The depth is owned here rather than taken from the camera's ZBUFFER raster,
// as D3D9 owns virtualScreenDepth rather than using the camera's. A single-
// sample depth attachment beside a multisample colour one is an incomplete
// framebuffer, and the camera's raster is single-sampled.
static void
acquireVirtualScreenMS(void)
{
	if(virtualScreenSamplesWanted <= 1)
		return;

	int32 maxSamples = 0;
	glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
	int32 want = virtualScreenSamplesWanted;
	if(want > maxSamples)
		want = maxSamples;
	if(want <= 1)
		return;

	glGenRenderbuffers(1, &virtualScreenMSColor);
	glBindRenderbuffer(GL_RENDERBUFFER, virtualScreenMSColor);
	// The resolve's format; a multisample resolve between two formats fails.
	glRenderbufferStorageMultisample(GL_RENDERBUFFER, want, GL_RGBA8,
	                                 virtualScreenWidth, virtualScreenHeight);

	glGenRenderbuffers(1, &virtualScreenMSDepth);
	glBindRenderbuffer(GL_RENDERBUFFER, virtualScreenMSDepth);
	glRenderbufferStorageMultisample(GL_RENDERBUFFER, want, GL_DEPTH24_STENCIL8,
	                                 virtualScreenWidth, virtualScreenHeight);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);

	glGenFramebuffers(1, &virtualScreenMSFbo);
	uint32 prevFbo = currentFramebuffer;
	bindFramebuffer(virtualScreenMSFbo);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                          GL_RENDERBUFFER, virtualScreenMSColor);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
	                          GL_RENDERBUFFER, virtualScreenMSDepth);

	// Refused, the scene is drawn into the single-sample buffer instead. That
	// is the picture without the extra samples, not a broken one.
	if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE){
		bindFramebuffer(prevFbo);
		deleteFramebuffer(virtualScreenMSFbo);
		glDeleteRenderbuffers(1, &virtualScreenMSColor);
		glDeleteRenderbuffers(1, &virtualScreenMSDepth);
		virtualScreenMSFbo = 0;
		virtualScreenMSColor = 0;
		virtualScreenMSDepth = 0;
		return;
	}

	bindFramebuffer(prevFbo);
	virtualScreenSamples = want;
}

// Fold the samples down into virtualScreenTex, so that everything reading the
// frame reads one picture whatever it was drawn with. A no-op at one sample.
void
resolveVirtualScreen(void)
{
	if(virtualScreenMSFbo == 0 || virtualScreenFbo == 0)
		return;

	glBindFramebuffer(GL_READ_FRAMEBUFFER, virtualScreenMSFbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, virtualScreenFbo);
	// The scissor is forced off for the reason clearCamera forces it: a blit
	// obeys it, and a frame that ended with one set would resolve a corner.
	glDisable(GL_SCISSOR_TEST);
	glBlitFramebuffer(0, 0, virtualScreenWidth, virtualScreenHeight,
	                  0, 0, virtualScreenWidth, virtualScreenHeight,
	                  GL_COLOR_BUFFER_BIT, GL_NEAREST);
	rebindFramebuffer();
}

// Set when the driver refused the virtual screen, so that the allocation is not
// tried again on every setFrameBuffer.
static bool32 virtualScreenRefused;

uint32
virtualScreenFramebuffer(void)
{
	if(virtualScreenFbo)
		return virtualScreenMSFbo ? virtualScreenMSFbo : virtualScreenFbo;
	if(virtualScreenRefused)
		return 0;
	if(virtualScreenWidth <= 0 || virtualScreenHeight <= 0)
		return 0;

	// The colour attachment is a texture rather than a renderbuffer because the
	// frame is worth sampling: every effect that reads the frame buffer back --
	// glow, heat haze, a loading-screen still -- wants exactly this.
	//
	// The depth is attached here rather than left to setFrameBuffer, which used
	// to hand over the camera's own ZBUFFER raster. Every camera raster shares
	// this one framebuffer, so the pairing setFrameBuffer keeps on the raster
	// stopped describing the framebuffer as soon as a second camera appeared --
	// and a multisampled colour buffer cannot take a single-sample raster as
	// its depth at all. D3D9 owns virtualScreenDepth for the same reason.
	glGenTextures(1, &virtualScreenTex);
	uint32 prev = bindTexture(virtualScreenTex);
	// With alpha, as D3D9's A8R8G8B8 back buffer and D3D11's target have: the
	// DESTALPHA blends read it and a colour mask can write only it.
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, virtualScreenWidth, virtualScreenHeight,
	             0, GL_RGBA, GL_UNSIGNED_BYTE, nil);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	bindTexture(prev);

	glGenRenderbuffers(1, &virtualScreenDepth);
	glBindRenderbuffer(GL_RENDERBUFFER, virtualScreenDepth);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
	                      virtualScreenWidth, virtualScreenHeight);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);

	glGenFramebuffers(1, &virtualScreenFbo);
	uint32 prevFbo = currentFramebuffer;
	bindFramebuffer(virtualScreenFbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                       GL_TEXTURE_2D, virtualScreenTex, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
	                          GL_RENDERBUFFER, virtualScreenDepth);

	// A driver that will not take this combination gets the window's own
	// framebuffer back rather than a camera that draws nowhere. The picture
	// then stretches on a resize, which is the behaviour before there was a
	// virtual screen and a great deal better than a black screen.
	if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE){
		bindFramebuffer(prevFbo);
		deleteFramebuffer(virtualScreenFbo);
		deleteTexture(virtualScreenTex);
		glDeleteRenderbuffers(1, &virtualScreenDepth);
		virtualScreenFbo = 0;
		virtualScreenTex = 0;
		virtualScreenDepth = 0;
		virtualScreenRefused = 1;
		return 0;
	}

	bindFramebuffer(prevFbo);
	acquireVirtualScreenMS();
	return virtualScreenMSFbo ? virtualScreenMSFbo : virtualScreenFbo;
}

bool32
copyVirtualScreen(Raster *dst)
{
	if(dst == nil || dst->type != Raster::CAMERATEXTURE)
		return 0;
	if(virtualScreenFbo == 0)
		return 0;
	if(dst->width != virtualScreenWidth || dst->height != virtualScreenHeight)
		return 0;

	Gl3Raster *natdst = PLUGINOFFSET(Gl3Raster, dst, nativeRasterOffset);
	if(natdst->fbo == 0)
		return 0;

	// Extents are equal, so this is a copy and not a scale; GL_NEAREST says so.
	// The scissor is forced off for the reason clearCamera forces it -- a blit
	// obeys it -- and the colour mask is not, because a blit does not.
	// The resolved buffer, not the multisampled one: glBlitFramebuffer will
	// not scale a multisample source, and this is the same picture.
	resolveVirtualScreen();
	glBindFramebuffer(GL_READ_FRAMEBUFFER, virtualScreenFbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, natdst->fbo);
	glDisable(GL_SCISSOR_TEST);
	glBlitFramebuffer(0, 0, virtualScreenWidth, virtualScreenHeight,
	                  0, 0, virtualScreenWidth, virtualScreenHeight,
	                  GL_COLOR_BUFFER_BIT, GL_NEAREST);

	// The read and draw bindings went round bindFramebuffer's cache, so it no
	// longer describes the binding. Put both back by hand.
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	currentFramebuffer = 0;
	return 1;
}

// --- the depth buffer, for looking at -----------------------------------
//
// A copy, for two reasons. The depth buffer is attached to the framebuffer
// being drawn into, and sampling a texture attached to the current framebuffer
// is undefined; and a multisampled depth buffer is a renderbuffer, which cannot
// be sampled at all. One blit answers both.
//
// Kept here rather than handed out as a Raster because librw has no raster kind
// for a sampleable depth texture, and inventing one for a debug view would be
// the wrong way round.
static uint32 depthCopyTex, depthCopyFbo;

static bool32
acquireDepthCopy(void)
{
	if(depthCopyFbo)
		return 1;
	// glDrawBuffer is desktop GL. GLES needs glDrawBuffers, and this is a
	// debug path -- it says no rather than growing an arm for it.
	if(gl3Caps.gles)
		return 0;
	if(virtualScreenWidth <= 0 || virtualScreenHeight <= 0)
		return 0;

	glGenTextures(1, &depthCopyTex);
	uint32 prev = bindTexture(depthCopyTex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8,
	             virtualScreenWidth, virtualScreenHeight, 0,
	             GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nil);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	// Read as a value, not compared against one. A depth texture left in
	// compare mode reads as zero through a plain sampler2D.
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
	bindTexture(prev);

	glGenFramebuffers(1, &depthCopyFbo);
	uint32 prevFbo = currentFramebuffer;
	bindFramebuffer(depthCopyFbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
	                       GL_TEXTURE_2D, depthCopyTex, 0);
	// No colour attachment, so say so; a framebuffer expecting one it has not
	// got is incomplete.
	glDrawBuffer(GL_NONE);
	glReadBuffer(GL_NONE);
	bool32 ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
	bindFramebuffer(prevFbo);

	if(!ok){
		deleteFramebuffer(depthCopyFbo);
		deleteTexture(depthCopyTex);
		depthCopyFbo = 0;
		depthCopyTex = 0;
		return 0;
	}

	return 1;
}

bool32
bindVirtualScreenDepth(int32 stage)
{
	uint32 src = virtualScreenFramebuffer();
	if(src == 0)
		return 0;
	if(!acquireDepthCopy())
		return 0;

	// Multisampled or not: a NEAREST depth blit from a multisampled read
	// framebuffer resolves to one sample per pixel, which is the picture a
	// debug view wants.
	glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, depthCopyFbo);
	glDisable(GL_SCISSOR_TEST);
	glBlitFramebuffer(0, 0, virtualScreenWidth, virtualScreenHeight,
	                  0, 0, virtualScreenWidth, virtualScreenHeight,
	                  GL_DEPTH_BUFFER_BIT, GL_NEAREST);
	rebindFramebuffer();

	// Round the raster cache, which tracks Rasters and has none for this. The
	// stage is cleared first so the cache reads nil while the depth texture is
	// bound, and unbindVirtualScreenDepth puts the white texture back -- a
	// later setTexture(stage, nil) would be skipped as already-nil and would
	// leave this bound.
	setTexture(stage, nil);
	setActiveTexture(stage);
	bindTexture(depthCopyTex);
	setActiveTexture(0);
	return 1;
}

void
unbindVirtualScreenDepth(int32 stage)
{
	if(depthCopyTex == 0)
		return;

	setActiveTexture(stage);
	bindTexture(whitetex);
	setActiveTexture(0);
}

void
destroyVirtualScreen(void)
{
	if(virtualScreenMSFbo){
		deleteFramebuffer(virtualScreenMSFbo);
		virtualScreenMSFbo = 0;
	}
	if(virtualScreenMSColor){
		glDeleteRenderbuffers(1, &virtualScreenMSColor);
		virtualScreenMSColor = 0;
	}
	if(virtualScreenMSDepth){
		glDeleteRenderbuffers(1, &virtualScreenMSDepth);
		virtualScreenMSDepth = 0;
	}
	virtualScreenSamples = 1;
	if(virtualScreenFbo){
		deleteFramebuffer(virtualScreenFbo);
		virtualScreenFbo = 0;
	}
	if(virtualScreenTex){
		deleteTexture(virtualScreenTex);
		virtualScreenTex = 0;
	}
	if(virtualScreenDepth){
		glDeleteRenderbuffers(1, &virtualScreenDepth);
		virtualScreenDepth = 0;
	}
	if(depthCopyFbo){
		deleteFramebuffer(depthCopyFbo);
		depthCopyFbo = 0;
	}
	if(depthCopyTex){
		deleteTexture(depthCopyTex);
		depthCopyTex = 0;
	}
	virtualScreenRefused = 0;
}

static GLint addressConvMap[] = {
	0, GL_REPEAT, GL_MIRRORED_REPEAT,
	GL_CLAMP_TO_EDGE, GL_CLAMP_TO_BORDER
};

static void
setFilterMode(uint32 stage, int32 filter, int32 maxAniso = 1)
{
	if(rwStateCache.texstage[stage].filter != (Texture::FilterMode)filter){
		rwStateCache.texstage[stage].filter = (Texture::FilterMode)filter;
		Raster *raster = rwStateCache.texstage[stage].raster;
		if(raster && filter != 0){
			Gl3Raster *natras = PLUGINOFFSET(Gl3Raster, rwStateCache.texstage[stage].raster, nativeRasterOffset);
			if(natras->filterMode != filter){
				setActiveTexture(stage);
				if(natras->autogenMipmap || natras->numLevels > 1){
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filterConvMap_MIP[filter]);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filterConvMap_NoMIP[filter]);
				}else{
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filterConvMap_NoMIP[filter]);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filterConvMap_NoMIP[filter]);
				}
				natras->filterMode = filter;
			}
			if(natras->maxAnisotropy != maxAniso && GLAD_GL_EXT_texture_filter_anisotropic){
				setActiveTexture(stage);
				glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, (float)maxAniso);
				natras->maxAnisotropy = maxAniso;
			}
		}
	}
}

static void
setAddressU(uint32 stage, int32 addressing)
{
	if(rwStateCache.texstage[stage].addressingU != (Texture::Addressing)addressing){
		rwStateCache.texstage[stage].addressingU = (Texture::Addressing)addressing;
		Raster *raster = rwStateCache.texstage[stage].raster;
		if(raster && addressing != 0){
			Gl3Raster *natras = PLUGINOFFSET(Gl3Raster, raster, nativeRasterOffset);
			// != , not ==. glTexParameteri is what actually changes the
			// texture, and it is needed exactly when the raster does not
			// already have the addressing being asked for -- the comparison
			// was the wrong way round, so the call was made only when it
			// would have done nothing and skipped whenever it mattered. The
			// cache above was updated either way, so the state looked set and
			// the texture kept whatever it was created with. A texture the
			// game asks to CLAMP stayed on GL_REPEAT: the character shadows
			// are a projected texture whose coordinates leave 0..1, and they
			// tiled into a grid around the character.
			if(natras->addressU != addressing){
				setActiveTexture(stage);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, addressConvMap[addressing]);
				natras->addressU = addressing;
			}
		}
	}
}

static void
setAddressV(uint32 stage, int32 addressing)
{
	if(rwStateCache.texstage[stage].addressingV != (Texture::Addressing)addressing){
		rwStateCache.texstage[stage].addressingV = (Texture::Addressing)addressing;
		Raster *raster = rwStateCache.texstage[stage].raster;
		if(raster && addressing != 0){
			Gl3Raster *natras = PLUGINOFFSET(Gl3Raster, rwStateCache.texstage[stage].raster, nativeRasterOffset);
			// The same inversion as setAddressU above; see the note there.
			if(natras->addressV != addressing){
				setActiveTexture(stage);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, addressConvMap[addressing]);
				natras->addressV = addressing;
			}
		}
	}
}

static void
setRasterStageOnly(uint32 stage, Raster *raster)
{
	bool32 alpha, keyed;
	if(raster != rwStateCache.texstage[stage].raster){
		rwStateCache.texstage[stage].raster = raster;
		setActiveTexture(stage);
		if(raster){
			assert(raster->platform == PLATFORM_GL3);
			Gl3Raster *natras = PLUGINOFFSET(Gl3Raster, raster, nativeRasterOffset);
			bindTexture(natras->texid);

			rwStateCache.texstage[stage].filter = (rw::Texture::FilterMode)natras->filterMode;
			rwStateCache.texstage[stage].addressingU = (rw::Texture::Addressing)natras->addressU;
			rwStateCache.texstage[stage].addressingV = (rw::Texture::Addressing)natras->addressV;

			alpha = natras->alphaKind != ALPHAOPAQUE;
			keyed = natras->alphaKind == ALPHAKEYED;
		}else{
			bindTexture(whitetex);
			alpha = 0;
			keyed = 0;
		}

		if(stage == 0){
			if(alpha != rwStateCache.textureAlpha ||
			   keyed != rwStateCache.textureKeyed){
				rwStateCache.textureAlpha = alpha;
				rwStateCache.textureKeyed = keyed;
				updateAlphaStates();
			}
		}
	}
}

static void
setRasterStage(uint32 stage, Raster *raster)
{
	bool32 alpha, keyed;
	if(raster != rwStateCache.texstage[stage].raster){
		rwStateCache.texstage[stage].raster = raster;
		setActiveTexture(stage);
		if(raster){
			assert(raster->platform == PLATFORM_GL3);
			Gl3Raster *natras = PLUGINOFFSET(Gl3Raster, raster, nativeRasterOffset);
			bindTexture(natras->texid);
			uint32 filter = rwStateCache.texstage[stage].filter;
			uint32 addrU = rwStateCache.texstage[stage].addressingU;
			uint32 addrV = rwStateCache.texstage[stage].addressingV;
			// Zero is a stage nothing has set a filter or addressing on, and
			// no GL enum: the texture keeps what it has.
			if(filter != 0 && natras->filterMode != filter){
				if(natras->autogenMipmap || natras->numLevels > 1){
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filterConvMap_MIP[filter]);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filterConvMap_NoMIP[filter]);
				}else{
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filterConvMap_NoMIP[filter]);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filterConvMap_NoMIP[filter]);
				}
				natras->filterMode = filter;
			}
			if(addrU != 0 && natras->addressU != addrU){
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, addressConvMap[addrU]);
				natras->addressU = addrU;
			}
			if(addrV != 0 && natras->addressV != addrV){
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, addressConvMap[addrV]);
				natras->addressV = addrV;
			}
			alpha = natras->alphaKind != ALPHAOPAQUE;
			keyed = natras->alphaKind == ALPHAKEYED;
		}else{
			bindTexture(whitetex);
			alpha = 0;
			keyed = 0;
		}

		if(stage == 0){
			if(alpha != rwStateCache.textureAlpha ||
			   keyed != rwStateCache.textureKeyed){
				rwStateCache.textureAlpha = alpha;
				rwStateCache.textureKeyed = keyed;
				updateAlphaStates();
			}
		}
	}
}

void
evictRaster(Raster *raster)
{
	int i;
	for(i = 0; i < MAXNUMSTAGES; i++){
		//assert(rwStateCache.texstage[i].raster != raster);
		if(rwStateCache.texstage[i].raster != raster)
			continue;
		setRasterStage(i, nil);
	}
}

void
setTexture(int32 stage, Texture *tex)
{
	if(tex == nil || tex->raster == nil){
		setRasterStage(stage, nil);
		return;
	}
	setRasterStageOnly(stage, tex->raster);
	setFilterMode(stage, tex->getFilter(), tex->getMaxAnisotropy());
	setAddressU(stage, tex->getAddressU());
	setAddressV(stage, tex->getAddressV());
}

static void
// **The switch is over RenderState, not over the int it was handed, and it has
// no default.** That is what makes -Wswitch an error when rwrender.h gains a
// state this backend has not answered: without it the state is accepted and
// silently dropped, which is how COLORWRITEMASK reached D3D9 and not GL3 and
// left every depth-priming pass painting.
//
// A backend that cannot support a state says so with an empty case and a
// comment. The point is that it is a decision somebody made, not a hole.
setRenderState(int32 state, void *pvalue)
{
	uint32 value = (uint32)(uintptr)pvalue;
	switch((RenderState)state){
	case TEXTURERASTER:
		setRasterStage(0, (Raster*)pvalue);
		break;
	case TEXTUREADDRESS:
		setAddressU(0, value);
		setAddressV(0, value);
		break;
	case TEXTUREADDRESSU:
		setAddressU(0, value);
		break;
	case TEXTUREADDRESSV:
		setAddressV(0, value);
		break;
	case TEXTUREFILTER:
		setFilterMode(0, value);
		break;
	case VERTEXALPHA:
		rwStateCache.appVertexAlpha = value;
		setVertexAlpha(value);
		break;
	case SRCBLEND:
		if(rwStateCache.srcblend != value){
			rwStateCache.srcblend = value;
			setGlRenderState(RWGL_SRCBLEND, blendMap[rwStateCache.srcblend]);
		}
		break;
	case DESTBLEND:
		if(rwStateCache.destblend != value){
			rwStateCache.destblend = value;
			setGlRenderState(RWGL_DESTBLEND, blendMap[rwStateCache.destblend]);
		}
		break;
	case ZTESTENABLE:
		setDepthTest(value);
		break;
	case ZWRITEENABLE:
		setDepthWrite(value);
		break;
	case FOGENABLE:
		if(rwStateCache.fogEnable != value){
			rwStateCache.fogEnable = value;
			uniformStateDirty[RWGL_FOG] = true;
			stateDirty = 1;
		}
		break;
	case FOGCOLOR:
		// no cache check here...too lazy
		RGBA c;
		c.red = value;
		c.green = value>>8;
		c.blue = value>>16;
		c.alpha = value>>24;
		convColor(&uniformState.fogColor, &c);
		uniformStateDirty[RWGL_FOGCOLOR] = true;
		stateDirty = 1;
		break;
	case CULLMODE:
		if(rwStateCache.cullmode != value){
			rwStateCache.cullmode = value;
			if(rwStateCache.cullmode == CULLNONE)
				setGlRenderState(RWGL_CULL, false);
			else{
				setGlRenderState(RWGL_CULL, true);
				setGlRenderState(RWGL_CULLFACE, rwStateCache.cullmode == CULLBACK ? GL_BACK : GL_FRONT);
			}
		}
		break;

	case STENCILENABLE:
		if(rwStateCache.stencilenable != value){
			rwStateCache.stencilenable = value;
			setGlRenderState(RWGL_STENCIL, value);
		}
		break;
	case STENCILFAIL:
		if(rwStateCache.stencilfail != value){
			rwStateCache.stencilfail = value;
			setGlRenderState(RWGL_STENCILFAIL, stencilOpMap[value]);
		}
		break;
	case STENCILZFAIL:
		if(rwStateCache.stencilzfail != value){
			rwStateCache.stencilzfail = value;
			setGlRenderState(RWGL_STENCILZFAIL, stencilOpMap[value]);
		}
		break;
	case STENCILPASS:
		if(rwStateCache.stencilpass != value){
			rwStateCache.stencilpass = value;
			setGlRenderState(RWGL_STENCILPASS, stencilOpMap[value]);
		}
		break;
	case STENCILFUNCTION:
		if(rwStateCache.stencilfunc != value){
			rwStateCache.stencilfunc = value;
			setGlRenderState(RWGL_STENCILFUNC, stencilFuncMap[value]);
		}
		break;
	case STENCILFUNCTIONREF:
		if(rwStateCache.stencilref != value){
			rwStateCache.stencilref = value;
			setGlRenderState(RWGL_STENCILREF, value);
		}
		break;
	case STENCILFUNCTIONMASK:
		if(rwStateCache.stencilmask != value){
			rwStateCache.stencilmask = value;
			setGlRenderState(RWGL_STENCILMASK, value);
		}
		break;
	case STENCILFUNCTIONWRITEMASK:
		if(rwStateCache.stencilwritemask != value){
			rwStateCache.stencilwritemask = value;
			setGlRenderState(RWGL_STENCILWRITEMASK, value);
		}
		break;

	case ALPHATESTFUNC:
		setAlphaTestFunction(value);
		break;
	case ALPHATESTREF:
		// updateAlphaStates owns alphaRef: a cutout raises the reference past
		// the one asked for.
		if(rwStateCache.alpharef != value){
			rwStateCache.alpharef = value;
			updateAlphaStates();
		}
		break;
	case GSALPHATEST:
		rwStateCache.gsalpha = value;
		break;
	case GSALPHATESTREF:
		rwStateCache.gsalpharef = value;
		break;
	case COLORWRITEMASK:
		if(rwStateCache.colorwritemask != value){
			rwStateCache.colorwritemask = value;
			setGlRenderState(RWGL_COLORMASK, value);
		}
		break;
	}
}

static void*
getRenderState(int32 state)
{
	// Initialised because the switch is exhaustive over the enum but the int
	// that arrives does not have to be one of its values.
	uint32 val = 0;
	RGBA rgba;
	switch((RenderState)state){
	case TEXTURERASTER:
		return rwStateCache.texstage[0].raster;
	case TEXTUREADDRESS:
		if(rwStateCache.texstage[0].addressingU == rwStateCache.texstage[0].addressingV)
			val = rwStateCache.texstage[0].addressingU;
		else
			val = 0;	// invalid
		break;
	case TEXTUREADDRESSU:
		val = rwStateCache.texstage[0].addressingU;
		break;
	case TEXTUREADDRESSV:
		val = rwStateCache.texstage[0].addressingV;
		break;
	case TEXTUREFILTER:
		val = rwStateCache.texstage[0].filter;
		break;

	case VERTEXALPHA:
		val = rwStateCache.vertexAlpha;
		break;
	case SRCBLEND:
		val = rwStateCache.srcblend;
		break;
	case DESTBLEND:
		val = rwStateCache.destblend;
		break;
	case ZTESTENABLE:
		val = rwStateCache.ztest;
		break;
	case ZWRITEENABLE:
		val = rwStateCache.zwrite;
		break;
	case FOGENABLE:
		val = rwStateCache.fogEnable;
		break;
	case FOGCOLOR:
		convColor(&rgba, &uniformState.fogColor);
		val = RWRGBAINT(rgba.red, rgba.green, rgba.blue, rgba.alpha);
		break;
	case CULLMODE:
		val = rwStateCache.cullmode;
		break;

	case STENCILENABLE:
		val = rwStateCache.stencilenable;
		break;
	case STENCILFAIL:
		val = rwStateCache.stencilfail;
		break;
	case STENCILZFAIL:
		val = rwStateCache.stencilzfail;
		break;
	case STENCILPASS:
		val = rwStateCache.stencilpass;
		break;
	case STENCILFUNCTION:
		val = rwStateCache.stencilfunc;
		break;
	case STENCILFUNCTIONREF:
		val = rwStateCache.stencilref;
		break;
	case STENCILFUNCTIONMASK:
		val = rwStateCache.stencilmask;
		break;
	case STENCILFUNCTIONWRITEMASK:
		val = rwStateCache.stencilwritemask;
		break;

	case ALPHATESTFUNC:
		val = rwStateCache.alphaFunc;
		break;
	case ALPHATESTREF:
		val = rwStateCache.alpharef;
		break;
	case GSALPHATEST:
		val = rwStateCache.gsalpha;
		break;
	case GSALPHATESTREF:
		val = rwStateCache.gsalpharef;
		break;
	case COLORWRITEMASK:
		val = rwStateCache.colorwritemask;
		break;
	}
	return (void*)(uintptr)val;
}

static void
resetRenderState(void)
{	
	rwStateCache.alphaFunc = ALPHAGREATEREQUAL;
	alphaFunc = 0;
	alphaRef = 10.0f/255.0f;
	rwStateCache.alpharef = 10;
	rwStateCache.textureKeyed = 0;
	uniformStateDirty[RWGL_ALPHAREF] = true;
	uniformState.fogDisable = 1.0f;
	uniformState.fogStart = 0.0f;
	uniformState.fogEnd = 0.0f;
	uniformState.fogRange = 0.0f;
	uniformState.fogColor = { 1.0f, 1.0f, 1.0f, 1.0f };
	rwStateCache.gsalpha = 0;
	rwStateCache.gsalpharef = 128;
	stateDirty = 1;

	rwStateCache.vertexAlpha = 0;
	rwStateCache.appVertexAlpha = 0;
	rwStateCache.textureAlpha = 0;
	rwStateCache.alphaTestEnable = 0;

	memset(&oldGlState, 0xFE, sizeof(oldGlState));

	rwStateCache.blendEnable = 0;
	setGlRenderState(RWGL_BLEND, false);
	rwStateCache.srcblend = BLENDSRCALPHA;
	rwStateCache.destblend = BLENDINVSRCALPHA;
	setGlRenderState(RWGL_SRCBLEND, blendMap[rwStateCache.srcblend]);
	setGlRenderState(RWGL_DESTBLEND, blendMap[rwStateCache.destblend]);

	rwStateCache.zwrite = GL_TRUE;
	setGlRenderState(RWGL_DEPTHMASK, rwStateCache.zwrite);

	rwStateCache.ztest = 1;
	setGlRenderState(RWGL_DEPTHTEST, true);
	setGlRenderState(RWGL_DEPTHFUNC, GL_LEQUAL);

	rwStateCache.cullmode = CULLNONE;
	setGlRenderState(RWGL_CULL, false);
	setGlRenderState(RWGL_CULLFACE, GL_BACK);

	rwStateCache.stencilenable = 0;
	setGlRenderState(RWGL_STENCIL, GL_FALSE);
	rwStateCache.stencilfail = STENCILKEEP;
	setGlRenderState(RWGL_STENCILFAIL, GL_KEEP);
	rwStateCache.stencilzfail = STENCILKEEP;
	setGlRenderState(RWGL_STENCILZFAIL, GL_KEEP);
	rwStateCache.stencilpass = STENCILKEEP;
	setGlRenderState(RWGL_STENCILPASS, GL_KEEP);
	rwStateCache.stencilfunc = STENCILALWAYS;
	setGlRenderState(RWGL_STENCILFUNC, GL_ALWAYS);
	rwStateCache.stencilref = 0;
	setGlRenderState(RWGL_STENCILREF, 0);
	rwStateCache.stencilmask = 0xFFFFFFFF;
	setGlRenderState(RWGL_STENCILMASK, 0xFFFFFFFF);
	rwStateCache.stencilwritemask = 0xFFFFFFFF;
	setGlRenderState(RWGL_STENCILWRITEMASK, 0xFFFFFFFF);

	rwStateCache.colorwritemask = COLORWRITEALL;
	setGlRenderState(RWGL_COLORMASK, COLORWRITEALL);
	setGlRenderState(RWGL_MULTISAMPLE, true);

	// Nothing cached survives: whitetex may have the name a texture had before
	// the engine last stopped, and a cached raster pointer may be a new raster.
	activeTexture = -1;
	for(int i = 0; i < MAXNUMSTAGES; i++){
		setActiveTexture(i);
		boundTexture[i] = ~(uint32)0;
		bindTexture(whitetex);
		rwStateCache.texstage[i].raster = nil;
	}
	setActiveTexture(0);
	currentFramebuffer = ~(uint32)0;
	bindFramebuffer(0);
	im2DActive = 0;
}

void
setWorldMatrix(Matrix *mat)
{
	convMatrix(&uniformObject.world, mat);
	setUniform(u_world, &uniformObject.world);

	RawMatrix normal;
	RawMatrix::normalMatrix(&normal, &uniformObject.world);
	setUniform(u_normal, &normal);

	objectDirty = 1;
}

int32
setLights(WorldLights *lightData)
{
	int i, n;
	Light *l;
	int32 bits;

	uniformObject.ambLight.red = lightData->ambient.red*lightIntensity;
	uniformObject.ambLight.green = lightData->ambient.green*lightIntensity;
	uniformObject.ambLight.blue = lightData->ambient.blue*lightIntensity;
	uniformObject.ambLight.alpha = lightData->ambient.alpha;

	bits = 0;

	if(lightData->numAmbients)
		bits |= VSLIGHT_AMBIENT;

	n = 0;
	for(i = 0; i < lightData->numDirectionals && i < 8; i++){
		l = lightData->directionals[i];
		uniformObject.lightParams[n].type = 1.0f;
		uniformObject.lightColor[n].red = l->color.red*lightIntensity;
		uniformObject.lightColor[n].green = l->color.green*lightIntensity;
		uniformObject.lightColor[n].blue = l->color.blue*lightIntensity;
		uniformObject.lightColor[n].alpha = l->color.alpha;
		memcpy(&uniformObject.lightDirection[n], &l->getFrame()->getLTM()->at, sizeof(V3d));
		bits |= VSLIGHT_DIRECT;
		n++;
		if(n >= MAX_LIGHTS)
			goto out;
	}

	for(i = 0; i < lightData->numLocals; i++){
		Light *l = lightData->locals[i];

		switch(l->getType()){
		case Light::POINT:
			uniformObject.lightParams[n].type = 2.0f;
			uniformObject.lightParams[n].radius = l->radius;
			uniformObject.lightColor[n] = l->color;
			memcpy(&uniformObject.lightPosition[n], &l->getFrame()->getLTM()->pos, sizeof(V3d));
			bits |= VSLIGHT_POINT;
			n++;
			if(n >= MAX_LIGHTS)
				goto out;
			break;
		case Light::SPOT:
		case Light::SOFTSPOT:
			uniformObject.lightParams[n].type = 3.0f;
			uniformObject.lightParams[n].minusCosAngle = l->minusCosAngle;
			uniformObject.lightParams[n].radius = l->radius;
			uniformObject.lightColor[n] = l->color;
			memcpy(&uniformObject.lightPosition[n], &l->getFrame()->getLTM()->pos, sizeof(V3d));
			memcpy(&uniformObject.lightDirection[n], &l->getFrame()->getLTM()->at, sizeof(V3d));
			// lower bound of falloff
			if(l->getType() == Light::SOFTSPOT)
				uniformObject.lightParams[n].hardSpot = 0.0f;
			else
				uniformObject.lightParams[n].hardSpot = 1.0f;
			bits |= VSLIGHT_SPOT;
			n++;
			if(n >= MAX_LIGHTS)
				goto out;
			break;
		}
	}

	// The terminator the shader loop stops on, and only when a slot is left to
	// put it in. A full array needs none: the loop runs out on its own.
	uniformObject.lightParams[n].type = 0.0f;
out:
	// The two things the toon pixel shader would otherwise loop for.
	//
	// Neither depends on the normal, so neither varies across a model, and
	// finding them per pixel was eight comparisons a fragment for two numbers
	// that were already settled here. The key is the brightest directional. The
	// room is every light summed as if the surface faced all of them at once --
	// a meaningless quantity for lighting a surface and the right one for
	// asking what colour it is in here -- scaled down to fit rather than
	// clamped per channel, so Rock Bottom stays blue instead of clipping cyan.
	//
	// The D3D9 backend has always done it this way because ps_2_0 has neither
	// loops nor branches. This is the same arithmetic, in the same place: the
	// lights' own colours, without lightIntensity, which scales the lighting
	// and not the room.
	if(toonParams[0] != 0.0f){
		float32 room[3];
		float32 bestLum = -1.0f;

		room[0] = lightData->ambient.red;
		room[1] = lightData->ambient.green;
		room[2] = lightData->ambient.blue;

		for(i = 0; i < lightData->numDirectionals && i < 8; i++){
			l = lightData->directionals[i];

			float32 r = l->color.red;
			float32 g = l->color.green;
			float32 b = l->color.blue;

			room[0] += r;
			room[1] += g;
			room[2] += b;

			if(!toonLightDirSet && r + g + b > bestLum){
				bestLum = r + g + b;
				V3d *at = &l->getFrame()->getLTM()->at;
				toonLightDir[0] = at->x;
				toonLightDir[1] = at->y;
				toonLightDir[2] = at->z;
			}
		}

		if(!toonRoomSet){
			float32 m = room[0];

			if(room[1] > m) m = room[1];
			if(room[2] > m) m = room[2];

			if(m > 1.0f){
				room[0] /= m;
				room[1] /= m;
				room[2] /= m;
			}

			toonRoomTint[0] = room[0] < 0.0f ? 0.0f : room[0];
			toonRoomTint[1] = room[1] < 0.0f ? 0.0f : room[1];
			toonRoomTint[2] = room[2] < 0.0f ? 0.0f : room[2];
		}

		// The level's own brightness, steepened. See setToonRoomPower.
		float32 shown[4];

		shown[0] = toonRoomTint[0];
		shown[1] = toonRoomTint[1];
		shown[2] = toonRoomTint[2];
		shown[3] = toonRoomTint[3];

		shown[0] *= toonRoomScale;
		shown[1] *= toonRoomScale;
		shown[2] *= toonRoomScale;

		setUniform(u_toonLightDir, toonLightDir);
		setUniform(u_toonRoomTint, shown);
	}

	// Reached by the gotos above as well, which is the point. They used to jump
	// PAST these, so an atomic lit by exactly MAX_LIGHTS lights filled
	// uniformObject and then uploaded none of it -- it drew with whatever the
	// previous atomic had been lit by. Reachable in BFBB, which ships a light
	// kit of eight directionals.
	setUniform(u_ambLight, &uniformObject.ambLight);
	setUniform(u_lightParams, uniformObject.lightParams);
	setUniform(u_lightPosition, uniformObject.lightPosition);
	setUniform(u_lightDirection, uniformObject.lightDirection);
	setUniform(u_lightColor, uniformObject.lightColor);

	objectDirty = 1;
	return bits;
}

void
setProjectionMatrix(float32 *mat)
{
	memcpy(&uniformScene.proj, mat, 64);
	setUniform(u_proj, uniformScene.proj);
	sceneDirty = 1;
}

void
setViewMatrix(float32 *mat)
{
	memcpy(&uniformScene.view, mat, 64);
	setUniform(u_view, uniformScene.view);
	sceneDirty = 1;
}

Shader *lastShaderUploaded;

void
setMaterial(const RGBA &color, const SurfaceProperties &surfaceprops, float extraSurfProp)
{
	rw::RGBAf col;
	convColor(&col, &color);
	setUniform(u_matColor, &col);

	float surfProps[4];
	surfProps[0] = surfaceprops.ambient;
	surfProps[1] = surfaceprops.specular;
	surfProps[2] = surfaceprops.diffuse;
	surfProps[3] = extraSurfProp;
	setUniform(u_surfProps, surfProps);
}

// 1/(start - end), for DoFog. Fog that starts where it ends is a step at that
// distance rather than a division by zero, whose infinity times the zero at
// w == end is NaN.
static float32
fogRangeOf(float32 start, float32 end)
{
	float32 d = start - end;
	if(fabs(d) < 1e-6f)
		d = -1e-6f;
	return 1.0f/d;
}

void
flushCache(void)
{
	flushGlRenderState();

#ifndef RW_GL_USE_UBOS

	// what's this doing here??
	uniformState.fogDisable = rwStateCache.fogEnable ? 0.0f : 1.0f;
	uniformState.fogStart = rwStateCache.fogStart;
	uniformState.fogEnd = rwStateCache.fogEnd;
	uniformState.fogRange = fogRangeOf(rwStateCache.fogStart, rwStateCache.fogEnd);

	if(uniformStateDirty[RWGL_ALPHAFUNC] || uniformStateDirty[RWGL_ALPHAREF]){
		float alphaTest[4] = {};
		switch(alphaFunc){
		case ALPHAALWAYS:
		default:
			alphaTest[0] = -1000.0f;
			alphaTest[1] = 1000.0f;
			break;
		case ALPHAGREATEREQUAL:
			alphaTest[0] = alphaRef;
			alphaTest[1] = 1000.0f;
			break;
		case ALPHALESS:
			alphaTest[0] = -1000.0f;
			alphaTest[1] = alphaRef;
			break;
		}
		setUniform(u_alphaRef, alphaTest);
		uniformStateDirty[RWGL_ALPHAFUNC] = false;
		uniformStateDirty[RWGL_ALPHAREF] = false;
	}

	if(uniformStateDirty[RWGL_FOG] ||
	   uniformStateDirty[RWGL_FOGSTART] ||
	   uniformStateDirty[RWGL_FOGEND]){
		float fogData[4] = {
			uniformState.fogStart,
			uniformState.fogEnd,
			uniformState.fogRange,
			uniformState.fogDisable
		};
		setUniform(u_fogData, fogData);
		uniformStateDirty[RWGL_FOG] = false;
		uniformStateDirty[RWGL_FOGSTART] = false;
		uniformStateDirty[RWGL_FOGEND] = false;
	}

	if(uniformStateDirty[RWGL_FOGCOLOR]){
		setUniform(u_fogColor, &uniformState.fogColor);
		uniformStateDirty[RWGL_FOGCOLOR] = false;
	}

#else
	if(objectDirty){
		glBindBuffer(GL_UNIFORM_BUFFER, ubo_object);
		glBufferData(GL_UNIFORM_BUFFER, sizeof(UniformObject), nil, GL_STREAM_DRAW);
		glBufferData(GL_UNIFORM_BUFFER, sizeof(UniformObject), &uniformObject, GL_STREAM_DRAW);
		objectDirty = 0;
	}
	if(sceneDirty){
		glBindBuffer(GL_UNIFORM_BUFFER, ubo_scene);
		glBufferData(GL_UNIFORM_BUFFER, sizeof(UniformScene), nil, GL_STREAM_DRAW);
		glBufferData(GL_UNIFORM_BUFFER, sizeof(UniformScene), &uniformScene, GL_STREAM_DRAW);
		sceneDirty = 0;
	}
	if(stateDirty){
		switch(alphaFunc){
		case ALPHAALWAYS:
		default:
			uniformState.alphaRefLow = -1000.0f;
			uniformState.alphaRefHigh = 1000.0f;
			break;
		case ALPHAGREATEREQUAL:
			uniformState.alphaRefLow = alphaRef;
			uniformState.alphaRefHigh = 1000.0f;
			break;
		case ALPHALESS:
			uniformState.alphaRefLow = -1000.0f;
			uniformState.alphaRefHigh = alphaRef;
			break;
		}
		uniformState.fogDisable = rwStateCache.fogEnable ? 0.0f : 1.0f;
		uniformState.fogStart = rwStateCache.fogStart;
		uniformState.fogEnd = rwStateCache.fogEnd;
		uniformState.fogRange = fogRangeOf(rwStateCache.fogStart, rwStateCache.fogEnd);
		glBindBuffer(GL_UNIFORM_BUFFER, ubo_state);
		glBufferData(GL_UNIFORM_BUFFER, sizeof(UniformState), nil, GL_STREAM_DRAW);
		glBufferData(GL_UNIFORM_BUFFER, sizeof(UniformState), &uniformState, GL_STREAM_DRAW);
		stateDirty = 0;
	}
#endif
	flushUniforms();
}

static void
setFrameBuffer(Camera *cam)
{
	Raster *fbuf = cam->frameBuffer->parent;
	// A camera is allowed no z buffer at all, and one that renders a
	// full-screen pass into a texture has none: RwCameraSetZRaster is never
	// called on it. The d3d9 device has always taken nil here; this one read
	// through it before it looked.
	Raster *zbuf = cam->zBuffer ? cam->zBuffer->parent : nil;
	assert(fbuf);

	Gl3Raster *natfb = PLUGINOFFSET(Gl3Raster, fbuf, nativeRasterOffset);
	assert(fbuf->type == Raster::CAMERA || fbuf->type == Raster::CAMERATEXTURE);

	bindFramebuffer(natfb->fbo);

	// The virtual screen owns its depth -- see virtualScreenFramebuffer -- so
	// there is nothing to pair here. Every camera raster shares that one
	// framebuffer, and the pairing below is per-raster: a second camera would
	// swap the attachment out from under the first, whose fboMate still claims
	// it. It is also the only way a multisampled colour buffer can have depth
	// at all, the camera's own ZBUFFER raster being single-sampled.
	if(natfb->fbo != 0 && natfb->fbo == virtualScreenFramebuffer())
		return;

	// Have to make sure depth buffer is attached to FB's fbo
	if(zbuf){
		Gl3Raster *natzb = PLUGINOFFSET(Gl3Raster, zbuf, nativeRasterOffset);
		if(natfb->fboMate == zbuf){
			// all good
			assert(natzb->fboMate == fbuf);
		}else{
			if(natzb->fboMate){
				// have to detatch from fbo first!
				Gl3Raster *oldfb = PLUGINOFFSET(Gl3Raster, natzb->fboMate, nativeRasterOffset);
				if(oldfb->fbo){
					bindFramebuffer(oldfb->fbo);
					glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
					bindFramebuffer(natfb->fbo);
				}
				oldfb->fboMate = nil;
			}
			natfb->fboMate = zbuf;
			natzb->fboMate = fbuf;
			if(natfb->fbo){
				if(gl3Caps.gles)
					glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, natzb->texid);
				else
					glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, natzb->texid, 0);
			}
		}
	}else{
		// remove z-buffer
		if(natfb->fboMate && natfb->fbo)
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
		natfb->fboMate = nil;
	}

	// **Say so when a camera texture cannot be drawn into.**
	//
	// An incomplete framebuffer is not an error GL raises anywhere: draws
	// aimed at it are discarded and the texture keeps whatever it held, so the
	// symptom is a render target that reads as one flat colour and never
	// changes however much is drawn. Nothing in that points at the target --
	// it looks like the geometry, the transform or the shader, and all three
	// can be ruled out at length before anyone suspects the framebuffer.
	//
	// Once per raster, and only when it is actually incomplete. A size the
	// driver will not attach is the usual cause.
	if(natfb->fbo && !natfb->fboChecked){
		natfb->fboChecked = 1;
		GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		if(status != GL_FRAMEBUFFER_COMPLETE)
			fprintf(stderr, "librw: camera texture %dx%d cannot be rendered into "
			                "(framebuffer status 0x%x); everything drawn to it is "
			                "discarded\n",
			        fbuf->width, fbuf->height, (unsigned)status);
	}
}

static Rect
getFramebufferRect(Raster *frameBuffer)
{
	Rect r;
	Raster *fb = frameBuffer->parent;
	Gl3Raster *natfb = PLUGINOFFSET(Gl3Raster, fb, nativeRasterOffset);
	if(fb->type == Raster::CAMERA && natfb->fbo){
		// A virtual screen. The picture is ITS size -- not the raster's, which
		// several cameras disagree about -- and the window's shape has nothing
		// to do with it until showRaster blits it out.
		r.w = virtualScreenWidth;
		r.h = virtualScreenHeight;
	}else if(fb->type == Raster::CAMERA){
#ifdef LIBRW_SDL2
		SDL_GetWindowSize(glGlobals.window, &r.w, &r.h);
#elif defined(LIBRW_SDL3)
		SDL_GetWindowSize(glGlobals.window, &r.w, &r.h);
#elif defined(LIBRW_GLFW)
		glfwGetFramebufferSize(glGlobals.window, &r.w, &r.h);
#else
		missing implementation
#endif
	}else{
		r.w = fb->width;
		r.h = fb->height;
	}
	r.x = 0;
	r.y = 0;

	// Got a subraster
	if(frameBuffer != fb){
		r.x = frameBuffer->offsetX;
		// GL y offset is from bottom
		r.y = r.h - frameBuffer->height - frameBuffer->offsetY;
		r.w = frameBuffer->width;
		r.h = frameBuffer->height;
	}

	return r;
}

static void
setViewport(Raster *frameBuffer)
{
	Rect r = getFramebufferRect(frameBuffer);
	if(r.w != glGlobals.presentWidth || r.h != glGlobals.presentHeight ||
	   r.x != glGlobals.presentOffX || r.y != glGlobals.presentOffY){
		glViewport(r.x, r.y, r.w, r.h);
		glGlobals.presentWidth = r.w;
		glGlobals.presentHeight = r.h;
		glGlobals.presentOffX = r.x;
		glGlobals.presentOffY = r.y;
	}
}

static void
beginUpdate(Camera *cam)
{
	float view[16], proj[16];
	// View Matrix
	Matrix inv;
	Matrix::invert(&inv, cam->getFrame()->getLTM());
	// Since we're looking into positive Z,
	// flip X to ge a left handed view space.
	view[0]  = -inv.right.x;
	view[1]  =  inv.right.y;
	view[2]  =  inv.right.z;
	view[3]  =  0.0f;
	view[4]  = -inv.up.x;
	view[5]  =  inv.up.y;
	view[6]  =  inv.up.z;
	view[7]  =  0.0f;
	view[8]  =  -inv.at.x;
	view[9]  =   inv.at.y;
	view[10] =  inv.at.z;
	view[11] =  0.0f;
	view[12] = -inv.pos.x;
	view[13] =  inv.pos.y;
	view[14] =  inv.pos.z;
	view[15] =  1.0f;
	memcpy(&cam->devView, &view, sizeof(RawMatrix));
	setViewMatrix(view);

	// Projection Matrix
	float32 invwx = 1.0f/cam->viewWindow.x;
	float32 invwy = 1.0f/cam->viewWindow.y;
	float32 invz = 1.0f/(cam->farPlane-cam->nearPlane);

	proj[0] = invwx;
	proj[1] = 0.0f;
	proj[2] = 0.0f;
	proj[3] = 0.0f;

	proj[4] = 0.0f;
	proj[5] = invwy;
	proj[6] = 0.0f;
	proj[7] = 0.0f;

	proj[8] = cam->viewOffset.x*invwx;
	proj[9] = cam->viewOffset.y*invwy;
	proj[12] = -proj[8];
	proj[13] = -proj[9];
	if(cam->projection == Camera::PERSPECTIVE){
		proj[10] = (cam->farPlane+cam->nearPlane)*invz;
		proj[11] = 1.0f;

		proj[14] = -2.0f*cam->nearPlane*cam->farPlane*invz;
		proj[15] = 0.0f;
	}else{
		proj[10] = 2.0f*invz;
		proj[11] = 0.0f;

		proj[14] = -(cam->farPlane+cam->nearPlane)*invz;
		proj[15] = 1.0f;
	}
	memcpy(&cam->devProj, &proj, sizeof(RawMatrix));
	setProjectionMatrix(proj);

	if(rwStateCache.fogStart != cam->fogPlane){
		rwStateCache.fogStart = cam->fogPlane;
		uniformStateDirty[RWGL_FOGSTART] = true;
		stateDirty = 1;
	}
	if(rwStateCache.fogEnd != cam->farPlane){
		rwStateCache.fogEnd = cam->farPlane;
		uniformStateDirty[RWGL_FOGEND] = true;
		stateDirty = 1;
	}

	setFrameBuffer(cam);

	setViewport(cam->frameBuffer);
}

static void
endUpdate(Camera *cam)
{
}

static void
clearCamera(Camera *cam, RGBA *col, uint32 mode)
{
	RGBAf colf;
	GLbitfield mask;

	setFrameBuffer(cam);

	// make sure we're only clearing the part of the framebuffer
	// that is subrastered
	bool setScissor = cam->frameBuffer != cam->frameBuffer->parent;
	if(setScissor){
		Rect r = getFramebufferRect(cam->frameBuffer);
		glScissor(r.x, r.y, r.w, r.h);
		glEnable(GL_SCISSOR_TEST);
	}

	convColor(&colf, col);
	glClearColor(colf.red, colf.green, colf.blue, colf.alpha);
	mask = 0;
	if(mode & Camera::CLEARIMAGE)
		mask |= GL_COLOR_BUFFER_BIT;
	if(mode & Camera::CLEARZ)
		mask |= GL_DEPTH_BUFFER_BIT;
	if(mode & Camera::CLEARSTENCIL)
		mask |= GL_STENCIL_BUFFER_BIT;
	// glClear obeys all three write masks, so a clear issued while one is off
	// would do nothing. D3D9's Clear() obeys none, and a clear that clears
	// is what a caller of this means either way.
	openWriteMasks();
	glClear(mask);

	if(setScissor)
		glDisable(GL_SCISSOR_TEST);
}

// Stretch the virtual screen into the window, keeping its aspect ratio.
//
// The destination is the largest rectangle of the virtual screen's shape that
// fits the window, centred, and the rest is cleared to black. Clearing every
// frame rather than only when the window changes costs one fill of a surface
// that is about to be overwritten anyway, and means nothing has to track when
// the bars last moved.
//
// The scissor and the colour mask are forced off around it for the reason
// clearCamera forces them: glClear obeys both, and glBlitFramebuffer obeys the
// scissor. A frame that ended with either set would otherwise blit into a
// corner of the window, or not at all.
static PresentOverlayFn presentOverlay;

void
setPresentOverlay(PresentOverlayFn fn)
{
	presentOverlay = fn;
}

// After an application's overlay has drawn with GL directly. Every cache is
// assumed wrong: the next flush sets each state again, the next Shader::use
// binds its program, textures and framebuffers rebind on first use, and the
// next camera sets its viewport.
static void
forgetGlState(void)
{
	memset(&oldGlState, 0xFE, sizeof(oldGlState));
	currentShader = nil;
	activeTexture = -1;
	for(int i = 0; i < MAXNUMSTAGES; i++)
		boundTexture[i] = ~(uint32)0;
	currentFramebuffer = ~(uint32)0;
	glGlobals.presentWidth = 0;
	glGlobals.presentHeight = 0;
	glGlobals.presentOffX = 0;
	glGlobals.presentOffY = 0;
	if(gl3Caps.glversion >= 30)
		glBindVertexArray(vao);
}

static void
blitVirtualScreen(Raster *raster)
{
	Raster *fb = raster->parent;
	Gl3Raster *natfb = PLUGINOFFSET(Gl3Raster, fb, nativeRasterOffset);
	if(natfb->fbo == 0)
		return;

	int winw = 0, winh = 0;
#if defined(LIBRW_SDL2) || defined(LIBRW_SDL3)
	SDL_GetWindowSize(glGlobals.window, &winw, &winh);
#elif defined(LIBRW_GLFW)
	glfwGetFramebufferSize(glGlobals.window, &winw, &winh);
#endif
	if(winw <= 0 || winh <= 0)
		return;

	int32 vw = virtualScreenWidth;
	int32 vh = virtualScreenHeight;
	if(vw <= 0 || vh <= 0)
		return;

	// Widest first, then narrowed if that would not fit vertically. Done in
	// 64-bit because a 4K window times a 2160-line screen overflows 32.
	int32 dw = winw;
	int32 dh = (int32)((int64)winw * vh / vw);
	if(dh > winh){
		dh = winh;
		dw = (int32)((int64)winh * vw / vh);
	}
	int32 dx = (winw - dw)/2;
	int32 dy = (winh - dh)/2;

	// The resolved buffer rather than natfb->fbo, which is the multisampled one
	// when there is one: glBlitFramebuffer will not scale a multisample source,
	// and this blit scales to the window.
	resolveVirtualScreen();
	glBindFramebuffer(GL_READ_FRAMEBUFFER, virtualScreenFbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

	glDisable(GL_SCISSOR_TEST);
	openWriteMasks();

	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	glBlitFramebuffer(0, 0, vw, vh, dx, dy, dx + dw, dy + dh,
	                  GL_COLOR_BUFFER_BIT, GL_LINEAR);

	if(presentOverlay){
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		if(presentOverlay(winw, winh))
			forgetGlState();
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glDisable(GL_SCISSOR_TEST);
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	}

	// The blit carried the frame's alpha into a window that has an alpha
	// channel, and a compositor that honours it shows the desktop through.
	// Opaque again, colour untouched; the next flush restores the mask.
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
	oldGlState.colorMask = COLORWRITEALPHA;
	glClear(GL_COLOR_BUFFER_BIT);

	// The read and draw bindings above went round bindFramebuffer's cache, so
	// it no longer describes the binding. Put both back by hand.
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	currentFramebuffer = 0;
}

static void
showRaster(Raster *raster, uint32 flags)
{
//	glViewport(raster->offsetX, raster->offsetY,
//		raster->width, raster->height);

	blitVirtualScreen(raster);

#ifdef LIBRW_SDL2
	if(flags & Raster::FLIPWAITVSYNCH)
		SDL_GL_SetSwapInterval(1);
	else
		SDL_GL_SetSwapInterval(0);
	SDL_GL_SwapWindow(glGlobals.window);
#elif defined(LIBRW_SDL3)
	if(flags & Raster::FLIPWAITVSYNCH)
		SDL_GL_SetSwapInterval(1);
	else
		SDL_GL_SetSwapInterval(0);
	SDL_GL_SwapWindow(glGlobals.window);
#elif defined(LIBRW_GLFW)
	if(flags & Raster::FLIPWAITVSYNCH)
		glfwSwapInterval(1);
	else
		glfwSwapInterval(0);
	glfwSwapBuffers(glGlobals.window);
#else
	not implemented
#endif
}

static bool32
rasterRenderFast(Raster *raster, int32 x, int32 y)
{
	Raster *src = raster;
	Raster *dst = Raster::getCurrentContext();
	Gl3Raster *natdst = PLUGINOFFSET(Gl3Raster, dst, nativeRasterOffset);
	Gl3Raster *natsrc = PLUGINOFFSET(Gl3Raster, src, nativeRasterOffset);

	switch(dst->type){
	case Raster::NORMAL:
	case Raster::TEXTURE:
	case Raster::CAMERATEXTURE:
		switch(src->type){
		case Raster::CAMERA:
			setActiveTexture(0);
			glBindTexture(GL_TEXTURE_2D, natdst->texid);
			glCopyTexSubImage2D(GL_TEXTURE_2D, 0, x, (dst->height-src->height)-y,
				0, 0, src->width, src->height);
			glBindTexture(GL_TEXTURE_2D, boundTexture[0]);
			return 1;
		}
		break;
	}
	return 0;
}

#ifdef LIBRW_SDL2

static void
addVideoMode(int displayIndex, int modeIndex)
{
	int i;
	SDL_DisplayMode mode;

	SDL_GetDisplayMode(displayIndex, modeIndex, &mode);

	for(i = 1; i < glGlobals.numModes; i++){
		if(glGlobals.modes[i].mode.w == mode.w &&
		   glGlobals.modes[i].mode.h == mode.h &&
		   glGlobals.modes[i].mode.format == mode.format){
			// had this mode already, remember highest refresh rate
			if(mode.refresh_rate > glGlobals.modes[i].mode.refresh_rate)
				glGlobals.modes[i].mode.refresh_rate = mode.refresh_rate;
			return;
		}
	}

	// none found, add
	glGlobals.modes[glGlobals.numModes].mode = mode;
	glGlobals.modes[glGlobals.numModes].flags = VIDEOMODEEXCLUSIVE;
	glGlobals.numModes++;
}

static void
makeVideoModeList(int displayIndex)
{
	int i, num, depth;

	num = SDL_GetNumDisplayModes(displayIndex);
	rwFree(glGlobals.modes);
	glGlobals.modes = rwNewT(DisplayMode, num+1, ID_DRIVER | MEMDUR_EVENT);

	SDL_GetCurrentDisplayMode(displayIndex, &glGlobals.modes[0].mode);
	glGlobals.modes[0].flags = 0;
	glGlobals.numModes = 1;

	for(i = 0; i < num; i++)
		addVideoMode(displayIndex, i);

	for(i = 0; i < glGlobals.numModes; i++){
		depth = SDL_BITSPERPIXEL(glGlobals.modes[i].mode.format);
		// set depth to power of two
		for(glGlobals.modes[i].depth = 1; glGlobals.modes[i].depth < depth; glGlobals.modes[i].depth <<= 1);
	}
}

static int
openSDL2(EngineOpenParams *openparams)
{
	glGlobals.winWidth = openparams->width;
	glGlobals.winHeight = openparams->height;
	glGlobals.winTitle = openparams->windowtitle;
	glGlobals.pWindow = openparams->window;

	memset(&gl3Caps, 0, sizeof(gl3Caps));

	/* Init SDL */
	if(SDL_InitSubSystem(SDL_INIT_VIDEO)){
		RWERROR((ERR_GENERAL, SDL_GetError()));
		return 0;
	}

	glGlobals.currentDisplay = 0;
	glGlobals.numDisplays = SDL_GetNumVideoDisplays();

	makeVideoModeList(glGlobals.currentDisplay);

	return 1;
}

static int
closeSDL2(void)
{
	SDL_QuitSubSystem(SDL_INIT_VIDEO);
	return 1;
}

static struct {
	int gl;
	int major, minor;
} profiles[] = {
	{ SDL_GL_CONTEXT_PROFILE_CORE, 3, 3 },
	{ SDL_GL_CONTEXT_PROFILE_CORE, 2, 1 },
	{ SDL_GL_CONTEXT_PROFILE_ES, 3, 1 },
	{ SDL_GL_CONTEXT_PROFILE_ES, 2, 0 },
	{ 0, 0, 0 },
};

static int
startSDL2(void)
{
	SDL_Window *win;
	SDL_GLContext ctx;
	DisplayMode *mode;

	mode = &glGlobals.modes[glGlobals.currentMode];

	SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, glGlobals.numSamples);

	int i;
	for(i = 0; profiles[i].gl; i++){
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, profiles[i].gl);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, profiles[i].major);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, profiles[i].minor);

		if(mode->flags & VIDEOMODEEXCLUSIVE) {
			win = SDL_CreateWindow(glGlobals.winTitle, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, mode->mode.w, mode->mode.h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);
			if (win)
				SDL_SetWindowDisplayMode(win, &mode->mode);
		} else {
			win = SDL_CreateWindow(glGlobals.winTitle, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, glGlobals.winWidth, glGlobals.winHeight, SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL);
			if (win)
				SDL_SetWindowDisplayMode(win, NULL);
		}
		if(win){
			gl3Caps.gles = profiles[i].gl == SDL_GL_CONTEXT_PROFILE_ES;
			gl3Caps.glversion = profiles[i].major*10 + profiles[i].minor;
			break;
		}
	}
	if(win == nil){
		RWERROR((ERR_GENERAL, SDL_GetError()));
		return 0;
	}
	ctx = SDL_GL_CreateContext(win);

	if (!((gl3Caps.gles ? gladLoadGLES2Loader : gladLoadGLLoader) ((GLADloadproc) SDL_GL_GetProcAddress, gl3Caps.glversion)) ) {
		RWERROR((ERR_GENERAL, "gladLoadGLLoader failed"));
		SDL_GL_DeleteContext(ctx);
		SDL_DestroyWindow(win);
		return 0;
	}

//	printf("OpenGL version: %s\n", glGetString(GL_VERSION));

	glGlobals.window = win;
	glGlobals.glcontext = ctx;
	*glGlobals.pWindow = win;
	glGlobals.presentWidth = 0;
	glGlobals.presentHeight = 0;
	glGlobals.presentOffX = 0;
	glGlobals.presentOffY = 0;
	return 1;
}

static int
stopSDL2(void)
{
	SDL_GL_DeleteContext(glGlobals.glcontext);
	SDL_DestroyWindow(glGlobals.window);
	return 1;
}

#elif defined(LIBRW_SDL3)

static void
addVideoMode(const SDL_DisplayMode *mode)
{
	int i;

	for(i = 1; i < glGlobals.numModes; i++){
		if(glGlobals.modes[i].mode.w == mode->w &&
		   glGlobals.modes[i].mode.h == mode->h &&
		   glGlobals.modes[i].mode.format == mode->format){
			// had this mode already, remember highest refresh rate
			if(mode->refresh_rate > glGlobals.modes[i].mode.refresh_rate)
				glGlobals.modes[i].mode.refresh_rate = mode->refresh_rate;
			return;
		}
	}

	// none found, add
	glGlobals.modes[glGlobals.numModes].mode = *mode;
	glGlobals.modes[glGlobals.numModes].flags = VIDEOMODEEXCLUSIVE;
	glGlobals.numModes++;
}

static void
makeVideoModeList(SDL_DisplayID displayIndex, SDL_DisplayID *displays)
{
	int i, num, depth;
	const SDL_DisplayMode *currentMode;
	SDL_DisplayMode **modes;

	currentMode = SDL_GetCurrentDisplayMode(displays[displayIndex]);
	modes = SDL_GetFullscreenDisplayModes(displays[displayIndex], &num);

	rwFree(glGlobals.modes);
	glGlobals.modes = rwNewT(DisplayMode, num+(currentMode != NULL ? 1 : 0), ID_DRIVER | MEMDUR_EVENT);

	if (currentMode) {
		glGlobals.modes[0].mode = *currentMode;
		glGlobals.modes[0].flags = 0;
		glGlobals.numModes = 1;
	}

	for(i = 0; i < num; i++)
		addVideoMode(modes[i]);

	for(i = 0; i < glGlobals.numModes; i++){
		depth = SDL_BITSPERPIXEL(glGlobals.modes[i].mode.format);
		// set depth to power of two
		for(glGlobals.modes[i].depth = 1; glGlobals.modes[i].depth < depth; glGlobals.modes[i].depth <<= 1);
	}

	SDL_free(modes);
}

static int
openSDL3(EngineOpenParams *openparams)
{
	glGlobals.winWidth = openparams->width;
	glGlobals.winHeight = openparams->height;
	glGlobals.winTitle = openparams->windowtitle;
	glGlobals.pWindow = openparams->window;

	memset(&gl3Caps, 0, sizeof(gl3Caps));

	/* Init SDL */
	if (!SDL_InitSubSystem(SDL_INIT_VIDEO)){
		RWERROR((ERR_GENERAL, SDL_GetError()));
		return 0;
	}

	glGlobals.currentDisplay = 0;
	SDL_DisplayID *displays = SDL_GetDisplays(&glGlobals.numDisplays);

	if (glGlobals.currentDisplay >= glGlobals.numDisplays) {
		RWERROR((ERR_GENERAL, SDL_GetError()));
		return 0;
	}

	makeVideoModeList(glGlobals.currentDisplay, displays);
	SDL_free(displays);

	return 1;
}

static int
closeSDL3(void)
{
	SDL_QuitSubSystem(SDL_INIT_VIDEO);
	return 1;
}

static struct {
	int gl;
	int major, minor;
} profiles[] = {
	{ SDL_GL_CONTEXT_PROFILE_CORE, 3, 3 },
	{ SDL_GL_CONTEXT_PROFILE_CORE, 2, 1 },
	{ SDL_GL_CONTEXT_PROFILE_ES, 3, 1 },
	{ SDL_GL_CONTEXT_PROFILE_ES, 3, 0 },
	{ SDL_GL_CONTEXT_PROFILE_ES, 2, 0 },
	{ 0, 0, 0 },
};

// The whole of one profile's attempt: a window, a context on it, and the
// entry points for the API that context turned out to speak. All three have
// to succeed together for the profile to count as available.
//
// Creating the context INSIDE the loop is the point. SDL_CreateWindow does
// not fail on a profile the driver cannot give -- the request is only
// attributes at that stage -- so a loop that breaks on the window alone
// always takes the first entry, and the fallbacks below it are unreachable.
// On a host with no desktop GL at all (Android, and any GLES-only driver)
// that meant asking for CORE 3.3, getting a window, and then failing at
// SDL_GL_CreateContext with the ES entries never tried.
static int
tryProfileSDL3(int index, SDL_Window **pwin, SDL_GLContext *pctx)
{
	SDL_Window *win;
	SDL_GLContext ctx;
	DisplayMode *mode = &glGlobals.modes[glGlobals.currentMode];

	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, profiles[index].gl);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, profiles[index].major);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, profiles[index].minor);

	if(mode->flags & VIDEOMODEEXCLUSIVE) {
		win = SDL_CreateWindow(glGlobals.winTitle, mode->mode.w, mode->mode.h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL);
		// This is the recommended way for SDL3.
		if (win) {
			SDL_SetWindowFullscreenMode(win, &mode->mode);
			SDL_SetWindowFullscreen(win, true);
		}
	} else {
		win = SDL_CreateWindow(glGlobals.winTitle, glGlobals.winWidth, glGlobals.winHeight, SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL);
		if (win)
			SDL_SetWindowFullscreenMode(win, NULL);
	}
	if(win == nil)
		return 0;

	ctx = SDL_GL_CreateContext(win);
	if(ctx == nil){
		SDL_DestroyWindow(win);
		return 0;
	}

	// Set before the load, because which loader runs is read from them.
	gl3Caps.gles = profiles[index].gl == SDL_GL_CONTEXT_PROFILE_ES;
	gl3Caps.glversion = profiles[index].major*10 + profiles[index].minor;

	// A driver may hand back a context older than the one asked for rather
	// than refusing, so this is a real failure case and not a formality:
	// glad fails when an entry point the requested version promises is not
	// there. Falling through to the next profile is the right answer.
	if(!((gl3Caps.gles ? gladLoadGLES2Loader : gladLoadGLLoader) ((GLADloadproc) SDL_GL_GetProcAddress, gl3Caps.glversion))){
		SDL_GL_DestroyContext(ctx);
		SDL_DestroyWindow(win);
		return 0;
	}

	*pwin = win;
	*pctx = ctx;
	return 1;
}

static int
startSDL3(void)
{
	SDL_Window *win = nil;
	SDL_GLContext ctx = nil;

	SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, glGlobals.numSamples);

	int i;
	for(i = 0; profiles[i].gl; i++)
		if(tryProfileSDL3(i, &win, &ctx))
			break;

	if(win == nil || ctx == nil){
		RWERROR((ERR_GENERAL, SDL_GetError()));
		return 0;
	}

//	printf("OpenGL version: %s\n", glGetString(GL_VERSION));

	glGlobals.window = win;
	glGlobals.glcontext = ctx;
	*glGlobals.pWindow = win;
	glGlobals.presentWidth = 0;
	glGlobals.presentHeight = 0;
	glGlobals.presentOffX = 0;
	glGlobals.presentOffY = 0;
	return 1;
}

static int
stopSDL3(void)
{
	SDL_GL_DestroyContext(glGlobals.glcontext);
	SDL_DestroyWindow(glGlobals.window);
	return 1;
}
#elif defined(LIBRW_GLFW)

static void
addVideoMode(const GLFWvidmode *mode)
{
	int i;

	for(i = 1; i < glGlobals.numModes; i++){
		if(glGlobals.modes[i].mode.width == mode->width &&
		   glGlobals.modes[i].mode.height == mode->height &&
		   glGlobals.modes[i].mode.redBits == mode->redBits &&
		   glGlobals.modes[i].mode.greenBits == mode->greenBits &&
		   glGlobals.modes[i].mode.blueBits == mode->blueBits){
			// had this mode already, remember highest refresh rate
			if(mode->refreshRate > glGlobals.modes[i].mode.refreshRate)
				glGlobals.modes[i].mode.refreshRate = mode->refreshRate;
			return;
		}
	}

	// none found, add
	glGlobals.modes[glGlobals.numModes].mode = *mode;
	glGlobals.modes[glGlobals.numModes].flags = VIDEOMODEEXCLUSIVE;
	glGlobals.numModes++;
}

static void
makeVideoModeList(GLFWmonitor *monitor)
{
	int i, num;
	const GLFWvidmode *modes;

	modes = glfwGetVideoModes(monitor, &num);
	rwFree(glGlobals.modes);
	glGlobals.modes = rwNewT(DisplayMode, num+1, ID_DRIVER | MEMDUR_EVENT);

	glGlobals.modes[0].mode = *glfwGetVideoMode(monitor);
	glGlobals.modes[0].flags = 0;
	glGlobals.numModes = 1;

	for(i = 0; i < num; i++)
		addVideoMode(&modes[i]);

	for(i = 0; i < glGlobals.numModes; i++){
		num = glGlobals.modes[i].mode.redBits +
			glGlobals.modes[i].mode.greenBits +
			glGlobals.modes[i].mode.blueBits;
		// set depth to power of two
		for(glGlobals.modes[i].depth = 1; glGlobals.modes[i].depth < num; glGlobals.modes[i].depth <<= 1);
	}
}

static int
openGLFW(EngineOpenParams *openparams)
{
	glGlobals.winWidth = openparams->width;
	glGlobals.winHeight = openparams->height;
	glGlobals.winTitle = openparams->windowtitle;
	glGlobals.pWindow = openparams->window;

	memset(&gl3Caps, 0, sizeof(gl3Caps));

	/* Init GLFW */
	if(!glfwInit()){
		RWERROR((ERR_GENERAL, "glfwInit() failed"));
		return 0;
	}

	glGlobals.monitor = glfwGetMonitors(&glGlobals.numMonitors)[0];

	makeVideoModeList(glGlobals.monitor);

	return 1;
}

static int
closeGLFW(void)
{
	glfwTerminate();
	return 1;
}

static void
glfwerr(int error, const char *desc)
{
	fprintf(stderr, "GLFW Error: %s\n", desc);
}

static struct {
	int gl;
	int major, minor;
} profiles[] = {
	{ GLFW_OPENGL_API, 3, 3 },
	{ GLFW_OPENGL_API, 2, 1 },
	{ GLFW_OPENGL_ES_API, 3, 1 },
	{ GLFW_OPENGL_ES_API, 2, 0 },
	{ 0, 0, 0 },
};

static int
startGLFW(void)
{
	GLFWwindow *win;
	DisplayMode *mode;

	mode = &glGlobals.modes[glGlobals.currentMode];

	glfwSetErrorCallback(glfwerr);
	glfwWindowHint(GLFW_RED_BITS, mode->mode.redBits);
	glfwWindowHint(GLFW_GREEN_BITS, mode->mode.greenBits);
	glfwWindowHint(GLFW_BLUE_BITS, mode->mode.blueBits);
	glfwWindowHint(GLFW_REFRESH_RATE, mode->mode.refreshRate);
	
	// GLX will round up to 2x or 4x if you ask for multisampling on with 1 sample
	// So only apply the SAMPLES hint if we actually want multisampling
	if (glGlobals.numSamples > 1)
		glfwWindowHint(GLFW_SAMPLES, glGlobals.numSamples);

	int i;
	for(i = 0; profiles[i].gl; i++){
		glfwWindowHint(GLFW_CLIENT_API, profiles[i].gl);
		glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, profiles[i].major);
		glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, profiles[i].minor);

		if(mode->flags & VIDEOMODEEXCLUSIVE)
			win = glfwCreateWindow(mode->mode.width, mode->mode.height, glGlobals.winTitle, glGlobals.monitor, nil);
		else
			win = glfwCreateWindow(glGlobals.winWidth, glGlobals.winHeight, glGlobals.winTitle, nil, nil);
		if(win){
			gl3Caps.gles = profiles[i].gl == GLFW_OPENGL_ES_API;
			gl3Caps.glversion = profiles[i].major*10 + profiles[i].minor;
			break;
		}
	}
	if(win == nil){
		RWERROR((ERR_GENERAL, "glfwCreateWindow() failed"));
		return 0;
	}
	glfwMakeContextCurrent(win);

	/* Init GLAD */
	if (!((gl3Caps.gles ? gladLoadGLES2Loader : gladLoadGLLoader) ((GLADloadproc) glfwGetProcAddress, gl3Caps.glversion)) ) {
		RWERROR((ERR_GENERAL, "gladLoadGLLoader failed"));
		glfwDestroyWindow(win);
		return 0;
	}

//	printf("OpenGL version: %s\n", glGetString(GL_VERSION));

	glGlobals.window = win;
	*glGlobals.pWindow = win;
	glGlobals.presentWidth = 0;
	glGlobals.presentHeight = 0;
	glGlobals.presentOffX = 0;
	glGlobals.presentOffY = 0;
	return 1;
}

static int
stopGLFW(void)
{
	glfwDestroyWindow(glGlobals.window);
	return 1;
}
#else
not implemented
#endif

static int
initOpenGL(void)
{
/*
	// this only works from 3.0 onward,
	// but luckily GLAD has already taken care of extensions for us
	int numExt;
	glGetIntegerv(GL_NUM_EXTENSIONS, &numExt);
	for(int i = 0; i < numExt; i++){
		const char *ext = (const char*)glGetStringi(GL_EXTENSIONS, i);
		if(ext == nil)
			continue;	// apparently that can happen...
		if(strcmp(ext, "GL_EXT_texture_compression_s3tc") == 0)
			gl3Caps.dxtSupported = true;
		else if(strcmp(ext, "GL_KHR_texture_compression_astc_ldr") == 0)
			gl3Caps.astcSupported = true;
//		printf("%d %s\n", i, ext);
	}
*/
	gl3Caps.dxtSupported = !!GLAD_GL_EXT_texture_compression_s3tc;
	gl3Caps.astcSupported = !!GLAD_GL_KHR_texture_compression_astc_ldr;

	// An extension, not core before 4.6; without it the enum is invalid.
	gl3Caps.maxAnisotropy = 1.0f;
	if(GLAD_GL_EXT_texture_filter_anisotropic)
		glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &gl3Caps.maxAnisotropy);

	if(gl3Caps.gles){
		if(gl3Caps.glversion >= 31)
			shaderDecl = shaderDecl310es;
		else if(gl3Caps.glversion >= 30)
			shaderDecl = shaderDecl300es;
		else
			shaderDecl = shaderDecl100es;
	}else{
		if(gl3Caps.glversion >= 30)
			shaderDecl = shaderDecl330;
		else
			shaderDecl = shaderDecl120;
	}

#ifndef RW_GL_USE_UBOS
	u_alphaRef = registerUniform("u_alphaRef", UNIFORM_VEC4);
	u_fogData = registerUniform("u_fogData", UNIFORM_VEC4);
	u_fogColor = registerUniform("u_fogColor", UNIFORM_VEC4);
	u_proj = registerUniform("u_proj", UNIFORM_MAT4);
	u_view = registerUniform("u_view", UNIFORM_MAT4);
	u_world = registerUniform("u_world", UNIFORM_MAT4);
	u_ambLight = registerUniform("u_ambLight", UNIFORM_VEC4);
	u_lightParams = registerUniform("u_lightParams", UNIFORM_VEC4, MAX_LIGHTS);
	u_lightPosition = registerUniform("u_lightPosition", UNIFORM_VEC4, MAX_LIGHTS);
	u_lightDirection = registerUniform("u_lightDirection", UNIFORM_VEC4, MAX_LIGHTS);
	u_lightColor = registerUniform("u_lightColor", UNIFORM_VEC4, MAX_LIGHTS);
	lastShaderUploaded = nil;
#else
	registerBlock("Scene");
	registerBlock("Object");
	registerBlock("State");
#endif
	u_normal = registerUniform("u_normal", UNIFORM_MAT4);
	u_uvXform = registerUniform("u_uvXform", UNIFORM_VEC4, 2);
	u_matColor = registerUniform("u_matColor", UNIFORM_VEC4);
	u_surfProps = registerUniform("u_surfProps", UNIFORM_VEC4);

	// The shadow map's transform and its knobs. Registered whether or not
	// anything uses them: a shader that does not mention a uniform gets -1 for
	// its location and flushUniforms skips it, so this costs nothing until a
	// shader reads it.
	u_shadowMatrix = registerUniform("u_shadowMatrix", UNIFORM_MAT4);
	u_shadowParams = registerUniform("u_shadowParams", UNIFORM_VEC4);
	u_shadowParams2 = registerUniform("u_shadowParams2", UNIFORM_VEC4);
	u_shadowLightDir = registerUniform("u_shadowLightDir", UNIFORM_VEC4);
	u_toonParams = registerUniform("u_toonParams", UNIFORM_VEC4);
	u_outlineColor = registerUniform("u_outlineColor", UNIFORM_VEC4);
	u_outlineColor2 = registerUniform("u_outlineColor2", UNIFORM_VEC4);
	u_toonLightDir = registerUniform("u_toonLightDir", UNIFORM_VEC4);
	u_outlineFlags = registerUniform("u_outlineFlags", UNIFORM_VEC4);
	u_outlineSign = registerUniform("u_outlineSign", UNIFORM_VEC4);
	u_outlineInk = registerUniform("u_outlineInk", UNIFORM_VEC4);
	u_toonRoomTint = registerUniform("u_toonRoomTint", UNIFORM_VEC4);
	u_toonExtra = registerUniform("u_toonExtra", UNIFORM_VEC4);
	u_toonExtra2 = registerUniform("u_toonExtra2", UNIFORM_VEC4);
	u_toonExtra3 = registerUniform("u_toonExtra3", UNIFORM_VEC4);
	toonRegistered = 1;

	// **Every one of them, and only once there is somewhere to put them.**
	//
	// The setters above run before Engine::open -- they are settings, read at
	// startup -- so each holds its value back and this is where the held values
	// go in. Two of these used to be pushed here and the rest reached the
	// shader by accident, through stray uploads in unrelated setters that fell
	// outside their own guard; the accident is gone and so is what it covered.
	setUniform(u_toonParams, toonParams);
	setUniform(u_outlineColor, outlineColor);
	setUniform(u_outlineColor2, outlineColor2);
	setUniform(u_outlineFlags, outlineFlags);
	setUniform(u_outlineSign, outlineSign);
	setUniform(u_outlineInk, outlineInk);
	setUniform(u_toonLightDir, toonLightDir);
	setUniform(u_toonRoomTint, toonRoomTint);
	setUniform(u_toonExtra, toonExtra);
	setUniform(u_toonExtra2, toonExtra2);
	setUniform(u_toonExtra3, toonExtra3);

	// for im2d
	registerUniform("u_xform", UNIFORM_VEC4);

	glClearColor(0.25, 0.25, 0.25, 1.0);

	byte whitepixel[4] = {0xFF, 0xFF, 0xFF, 0xFF};
	glGenTextures(1, &whitetex);
	glBindTexture(GL_TEXTURE_2D, whitetex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1,
	             0, GL_RGBA, GL_UNSIGNED_BYTE, &whitepixel);

	// Geometry without the PRELIT flag has no colour attribute in its vertex
	// buffer at all, so every one of its vertices reads the generic value set
	// here. That value is black, which means such a model can only be
	// brightened by lighting and never merely tinted by it -- see
	// constantVertexColorWhite for why an application might want the other
	// answer. It is context state, not vertex array state, so setting it once
	// covers every draw whether or not VAOs are in use.
	{
		float32 base = constantVertexColorWhite ? 1.0f : 0.0f;
		glVertexAttrib4f(ATTRIB_COLOR, base, base, base, 1.0f);
	}

	resetRenderState();

	maxAnisotropy = gl3Caps.maxAnisotropy;

	if(gl3Caps.glversion >= 30){
		glGenVertexArrays(1, &vao);
		glBindVertexArray(vao);
	}

#ifdef RW_GL_USE_UBOS
	glGenBuffers(1, &ubo_state);
	glBindBuffer(GL_UNIFORM_BUFFER, ubo_state);
	glBindBufferBase(GL_UNIFORM_BUFFER, gl3::findBlock("State"), ubo_state);
	glBufferData(GL_UNIFORM_BUFFER, sizeof(UniformState), &uniformState,
	             GL_STREAM_DRAW);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);

	glGenBuffers(1, &ubo_scene);
	glBindBuffer(GL_UNIFORM_BUFFER, ubo_scene);
	glBindBufferBase(GL_UNIFORM_BUFFER, gl3::findBlock("Scene"), ubo_scene);
	glBufferData(GL_UNIFORM_BUFFER, sizeof(UniformScene), &uniformScene,
	             GL_STREAM_DRAW);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);

	glGenBuffers(1, &ubo_object);
	glBindBuffer(GL_UNIFORM_BUFFER, ubo_object);
	glBindBufferBase(GL_UNIFORM_BUFFER, gl3::findBlock("Object"), ubo_object);
	glBufferData(GL_UNIFORM_BUFFER, sizeof(UniformObject), &uniformObject,
	             GL_STREAM_DRAW);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);
#endif

#include "shaders/default_vs_gl.inc"
#include "shaders/simple_fs_gl.inc"
#include "shaders/lighting_fs.inc"
	const char *vs[] = { shaderDecl, header_vert_src, default_vert_src, nil };
	const char *vs_fullLight[] = { shaderDecl, "#define DIRECTIONALS\n#define POINTLIGHTS\n#define SPOTLIGHTS\n", header_vert_src, default_vert_src, nil };
	const char *fs[] = { shaderDecl, "#define SHADOWRECEIVER\n", header_frag_src, simple_frag_src, nil };
	const char *fs_noAT[] = { shaderDecl, "#define SHADOWRECEIVER\n", "#define NO_ALPHATEST\n", header_frag_src, simple_frag_src, nil };

	defaultShader = Shader::create(vs, fs);
	assert(defaultShader);
	defaultShader_noAT = Shader::create(vs, fs_noAT);
	assert(defaultShader_noAT);

	defaultShader_fullLight = Shader::create(vs_fullLight, fs);
	assert(defaultShader_fullLight);
	defaultShader_fullLight_noAT = Shader::create(vs_fullLight, fs_noAT);
	assert(defaultShader_fullLight_noAT);

	// The same vertex shader again with the texture coordinate transform
	// compiled in. Separate programs rather than a branch, so a model with no
	// animated UVs pays nothing for the ones that have them.
	const char *vs_uv[] = { shaderDecl, "#define UVXFORM\n", header_vert_src, default_vert_src, nil };
	const char *vs_uv_fullLight[] = { shaderDecl, "#define UVXFORM\n#define DIRECTIONALS\n#define POINTLIGHTS\n#define SPOTLIGHTS\n", header_vert_src, default_vert_src, nil };

	uvXformShader = Shader::create(vs_uv, fs);
	assert(uvXformShader);
	uvXformShader_noAT = Shader::create(vs_uv, fs_noAT);
	assert(uvXformShader_noAT);

	uvXformShader_fullLight = Shader::create(vs_uv_fullLight, fs);
	assert(uvXformShader_fullLight);
	uvXformShader_fullLight_noAT = Shader::create(vs_uv_fullLight, fs_noAT);
	assert(uvXformShader_fullLight_noAT);

	// The per-pixel programs. lighting.frag goes between the header and
	// simple.frag because it declares the uniforms simple.frag's PERPIXEL block
	// reads; it is prepended here and nowhere else, so every program above is
	// byte for byte the source it was.
	const char *vs_pp[] = { shaderDecl, "#define PERPIXEL\n", header_vert_src, default_vert_src, nil };
	const char *vs_uv_pp[] = { shaderDecl, "#define PERPIXEL\n#define UVXFORM\n", header_vert_src, default_vert_src, nil };
	const char *fs_pp[] = { shaderDecl, "#define SHADOWRECEIVER\n", "#define PERPIXEL\n", header_frag_src, lighting_frag_src, simple_frag_src, nil };
	const char *fs_pp_noAT[] = { shaderDecl, "#define SHADOWRECEIVER\n", "#define PERPIXEL\n#define NO_ALPHATEST\n", header_frag_src, lighting_frag_src, simple_frag_src, nil };

	defaultShader_pp = Shader::create(vs_pp, fs_pp);
	assert(defaultShader_pp);
	defaultShader_pp_noAT = Shader::create(vs_pp, fs_pp_noAT);
	assert(defaultShader_pp_noAT);

	uvXformShader_pp = Shader::create(vs_uv_pp, fs_pp);
	assert(uvXformShader_pp);
	uvXformShader_pp_noAT = Shader::create(vs_uv_pp, fs_pp_noAT);
	assert(uvXformShader_pp_noAT);

	// The caster pass. The vertex shader is the plain one -- no light defines,
	// no UV transform -- because depth.frag reads none of its outputs and a
	// fragment shader may declare fewer inputs than the vertex stage writes.
	// That is what lets one fragment shader serve both this and the skinned
	// caster in gl3skin.cpp.
	{
#include "shaders/depth_fs.inc"
		const char *fs_depth[] = { shaderDecl, header_frag_src, depth_frag_src, nil };
		depthShader = Shader::create(vs, fs_depth);
		assert(depthShader);

		// The same again, reading the caster's texture so it can cut its own
		// shape out of the alpha channel. A second program rather than a
		// branch: most casters are solid and should not pay for a texture
		// fetch they do not need.
		const char *fs_depth_tex[] = { shaderDecl, "#define TEX\n", header_frag_src, depth_frag_src, nil };
		depthShader_tex = Shader::create(vs, fs_depth_tex);
		assert(depthShader_tex);

		// The outline hull. Its own vertex shader because the inflation
		// happens there, and the plain fragment shader because a flat colour
		// needs nothing from the surface it is drawn around.
#include "shaders/outline_fs.inc"
		const char *vs_outline[] = { shaderDecl, "#define OUTLINE\n", header_vert_src, default_vert_src, nil };
		// lighting.frag between the two, because the ink is lit and
		// ToonRoomLight is where the light uniforms are declared.
		const char *fs_outline[] = { shaderDecl, header_frag_src, lighting_frag_src, outline_frag_src, nil };
		outlineShader = Shader::create(vs_outline, fs_outline);
		assert(outlineShader);
	}

	openIm2D();
	openIm3D();

	return 1;
}

static int
termOpenGL(void)
{
	closeIm3D();
	closeIm2D();

	defaultShader->destroy();
	defaultShader = nil;
	defaultShader_noAT->destroy();
	defaultShader_noAT = nil;
	defaultShader_fullLight->destroy();
	defaultShader_fullLight = nil;
	defaultShader_fullLight_noAT->destroy();
	defaultShader_fullLight_noAT = nil;

	uvXformShader->destroy();
	uvXformShader = nil;
	uvXformShader_noAT->destroy();
	uvXformShader_noAT = nil;
	uvXformShader_fullLight->destroy();
	uvXformShader_fullLight = nil;
	uvXformShader_fullLight_noAT->destroy();
	uvXformShader_fullLight_noAT = nil;

	defaultShader_pp->destroy();
	defaultShader_pp = nil;
	defaultShader_pp_noAT->destroy();
	defaultShader_pp_noAT = nil;
	uvXformShader_pp->destroy();
	uvXformShader_pp = nil;
	uvXformShader_pp_noAT->destroy();
	uvXformShader_pp_noAT = nil;
	depthShader->destroy();
	depthShader = nil;
	depthShader_tex->destroy();
	depthShader_tex = nil;

	glDeleteTextures(1, &whitetex);
	whitetex = 0;

	destroyVirtualScreen();

	return 1;
}

static int
finalizeOpenGL(void)
{
	return 1;
}

#ifdef LIBRW_SDL2
static int
deviceSystemSDL2(DeviceReq req, void *arg, int32 n)
{
	VideoMode *rwmode;

	switch(req){
	case DEVICEOPEN:
		return openSDL2((EngineOpenParams*)arg);
	case DEVICECLOSE:
		return closeSDL2();

	case DEVICEINIT:
		return startSDL2() && initOpenGL();
	case DEVICETERM:
		return termOpenGL() && stopSDL2();

	case DEVICEFINALIZE:
		return finalizeOpenGL();


	case DEVICEGETNUMSUBSYSTEMS:
		return glGlobals.numDisplays;

	case DEVICEGETCURRENTSUBSYSTEM:
		return glGlobals.currentDisplay;

	case DEVICESETSUBSYSTEM:
		if (n >= SDL_GetNumVideoDisplays())
			return 0;
		glGlobals.currentDisplay = n;
		return 1;

	case DEVICEGETSUBSSYSTEMINFO: {
		const char *display_name = SDL_GetDisplayName(n);
		if (display_name == nil)
			return 0;
		strncpy(((SubSystemInfo*)arg)->name, display_name, sizeof(SubSystemInfo::name));
		return 1;
	}


	case DEVICEGETNUMVIDEOMODES:
		return glGlobals.numModes;

	case DEVICEGETCURRENTVIDEOMODE:
		return glGlobals.currentMode;

	case DEVICESETVIDEOMODE:
		if(n >= glGlobals.numModes)
			return 0;
		glGlobals.currentMode = n;
		return 1;

	case DEVICEGETVIDEOMODEINFO:
		rwmode = (VideoMode*)arg;
		rwmode->width = glGlobals.modes[n].mode.w;
		rwmode->height = glGlobals.modes[n].mode.h;
		rwmode->depth = glGlobals.modes[n].depth;
		rwmode->flags = glGlobals.modes[n].flags;
		return 1;

	case DEVICEGETMAXMULTISAMPLINGLEVELS:
		{
			GLint maxSamples;
			glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
			if(maxSamples == 0)
				return 1;
			return maxSamples;
		}
	case DEVICEGETMULTISAMPLINGLEVELS:
		if(glGlobals.numSamples == 0)
			return 1;
		return glGlobals.numSamples;
	case DEVICESETMULTISAMPLINGLEVELS:
		glGlobals.numSamples = (uint32)n;
		return 1;
	default:
		assert(0 && "not implemented");
		return 0;
	}
	return 1;
}

#elif defined(LIBRW_SDL3)

static int
deviceSystemSDL3(DeviceReq req, void *arg, int32 n)
{
	VideoMode *rwmode;

	switch(req){
	case DEVICEOPEN:
		return openSDL3((EngineOpenParams*)arg);
	case DEVICECLOSE:
		return closeSDL3();

	case DEVICEINIT:
		return startSDL3() && initOpenGL();
	case DEVICETERM:
		return termOpenGL() && stopSDL3();

	case DEVICEFINALIZE:
		return finalizeOpenGL();


	case DEVICEGETNUMSUBSYSTEMS:
		return glGlobals.numDisplays;

	case DEVICEGETCURRENTSUBSYSTEM:
		return glGlobals.currentDisplay;

	case DEVICESETSUBSYSTEM:
		if (n >= glGlobals.numDisplays)
			return 0;
		glGlobals.currentDisplay = n;
		return 1;

	case DEVICEGETSUBSSYSTEMINFO: {
		const char *display_name = SDL_GetDisplayName(n);
		if (display_name == nil)
			return 0;
		strncpy(((SubSystemInfo*)arg)->name, display_name, sizeof(SubSystemInfo::name));
		return 1;
	}


	case DEVICEGETNUMVIDEOMODES:
		return glGlobals.numModes;

	case DEVICEGETCURRENTVIDEOMODE:
		return glGlobals.currentMode;

	case DEVICESETVIDEOMODE:
		if(n >= glGlobals.numModes)
			return 0;
		glGlobals.currentMode = n;
		return 1;

	case DEVICEGETVIDEOMODEINFO:
		if (n < 0)
			return 0;
		rwmode = (VideoMode*)arg;
		rwmode->width = glGlobals.modes[n].mode.w;
		rwmode->height = glGlobals.modes[n].mode.h;
		rwmode->depth = glGlobals.modes[n].depth;
		rwmode->flags = glGlobals.modes[n].flags;
		return 1;

	case DEVICEGETMAXMULTISAMPLINGLEVELS:
		{
			GLint maxSamples;
			glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
			if(maxSamples == 0)
				return 1;
			return maxSamples;
		}
	case DEVICEGETMULTISAMPLINGLEVELS:
		if(glGlobals.numSamples == 0)
			return 1;
		return glGlobals.numSamples;
	case DEVICESETMULTISAMPLINGLEVELS:
		glGlobals.numSamples = (uint32)n;
		return 1;
	default:
		assert(0 && "not implemented");
		return 0;
	}
	return 1;
}

#elif defined(LIBRW_GLFW)

static int
deviceSystemGLFW(DeviceReq req, void *arg, int32 n)
{
	GLFWmonitor **monitors;
	VideoMode *rwmode;

	switch(req){
	case DEVICEOPEN:
		return openGLFW((EngineOpenParams*)arg);
	case DEVICECLOSE:
		return closeGLFW();

	case DEVICEINIT:
		return startGLFW() && initOpenGL();
	case DEVICETERM:
		return termOpenGL() && stopGLFW();

	case DEVICEFINALIZE:
		return finalizeOpenGL();


	case DEVICEGETNUMSUBSYSTEMS:
		return glGlobals.numMonitors;

	case DEVICEGETCURRENTSUBSYSTEM:
		return glGlobals.currentMonitor;

	case DEVICESETSUBSYSTEM:
		monitors = glfwGetMonitors(&glGlobals.numMonitors);
		if(n >= glGlobals.numMonitors)
			return 0;
		glGlobals.currentMonitor = n;
		glGlobals.monitor = monitors[glGlobals.currentMonitor];
		return 1;

	case DEVICEGETSUBSSYSTEMINFO:
		monitors = glfwGetMonitors(&glGlobals.numMonitors);
		if(n >= glGlobals.numMonitors)
			return 0;
		strncpy(((SubSystemInfo*)arg)->name, glfwGetMonitorName(monitors[n]), sizeof(SubSystemInfo::name));
		return 1;


	case DEVICEGETNUMVIDEOMODES:
		return glGlobals.numModes;

	case DEVICEGETCURRENTVIDEOMODE:
		return glGlobals.currentMode;

	case DEVICESETVIDEOMODE:
		if(n >= glGlobals.numModes)
			return 0;
		glGlobals.currentMode = n;
		return 1;

	case DEVICEGETVIDEOMODEINFO:
		rwmode = (VideoMode*)arg;
		rwmode->width = glGlobals.modes[n].mode.width;
		rwmode->height = glGlobals.modes[n].mode.height;
		rwmode->depth = glGlobals.modes[n].depth;
		rwmode->flags = glGlobals.modes[n].flags;
		return 1;

	case DEVICEGETMAXMULTISAMPLINGLEVELS:
		{
			GLint maxSamples;
			glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
			if(maxSamples == 0)
				return 1;
			return maxSamples;
		}
	case DEVICEGETMULTISAMPLINGLEVELS:
		if(glGlobals.numSamples == 0)
			return 1;
		return glGlobals.numSamples;
	case DEVICESETMULTISAMPLINGLEVELS:
		glGlobals.numSamples = (uint32)n;
		return 1;
	default:
		assert(0 && "not implemented");
		return 0;
	}
	return 1;
}

#else

not implemented

#endif

Device renderdevice = {
	-1.0f, 1.0f,
	gl3::beginUpdate,
	gl3::endUpdate,
	gl3::clearCamera,
	gl3::showRaster,
	gl3::rasterRenderFast,
	gl3::setRenderState,
	gl3::getRenderState,
	gl3::im2DRenderLine,
	gl3::im2DRenderTriangle,
	gl3::im2DRenderPrimitive,
	gl3::im2DRenderIndexedPrimitive,
	gl3::im3DTransform,
	gl3::im3DRenderPrimitive,
	gl3::im3DRenderIndexedPrimitive,
	gl3::im3DEnd,
#ifdef LIBRW_SDL2
	gl3::deviceSystemSDL2
#elif defined(LIBRW_SDL3)
	gl3::deviceSystemSDL3
#elif defined(LIBRW_GLFW)
	gl3::deviceSystemGLFW
#else
	not implemented
#endif
};

}
}

#else
// urgh, probably should get rid of that eventually
#include "rwgl3.h"
namespace rw {
namespace gl3 { 
Gl3Caps gl3Caps;
bool32 needToReadBackTextures;
}
}
#endif
