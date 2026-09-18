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
#include "rwd3d.h"

namespace rw {
namespace d3d {

// Whether the fixed-function pipeline draws instead of the shaders. Read at
// driverOpen to pick the render callbacks, and at every seam that would
// otherwise name a shader.
//
// Outside the platform guard because d3d9.cpp -- which is the PLATFORM_D3D9
// stream plugin and is built for every backend -- reads it when it makes its
// pipelines.
static bool32 fixedFunction;

void
setFixedFunctionEnabled(bool32 enable)
{
	fixedFunction = !!enable;
}

bool32
getFixedFunction(void)
{
	return fixedFunction;
}

#if defined(RW_D3D_ANY)

#ifdef RW_D3D9
IDirect3DDevice9 *d3ddevice = nil;
#endif

#define MAX_LIGHTS 8


void *default_amb_VS;
void *default_amb_dir_VS;
void *default_all_VS;
void *uvxform_amb_VS;
void *uvxform_amb_dir_VS;
void *uvxform_all_VS;
void *default_pp_VS;
void *uvxform_pp_VS;
void *default_PS;
void *default_tex_PS;
void *default_pp_PS;
void *default_tex_pp_PS;
void *im2d_VS;
void *im2d_PS;
void *im2d_tex_PS;

static bool32 perPixelLighting;

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


#ifdef RW_D3D9
namespace sm2 {
#include "shaders/default_amb_VS.h"
#include "shaders/default_amb_dir_VS.h"
#include "shaders/default_all_VS.h"
#include "shaders/uvxform_amb_VS.h"
#include "shaders/uvxform_amb_dir_VS.h"
#include "shaders/uvxform_all_VS.h"
#include "shaders/default_pp_VS.h"
#include "shaders/uvxform_pp_VS.h"
#include "shaders/default_PS.h"
#include "shaders/default_tex_PS.h"
#include "shaders/default_pp_PS.h"
#include "shaders/default_tex_pp_PS.h"
#include "shaders/default_toon_PS.h"
#include "shaders/default_tex_toon_PS.h"
#include "shaders/outline_VS.h"
#include "shaders/outline_PS.h"
#include "shaders/im2d_VS.h"
#include "shaders/im2d_PS.h"
#include "shaders/im2d_tex_PS.h"
}
#endif
#ifdef RW_D3D11
namespace sm4 {
#include "shaders11/default_amb_VS.h"
#include "shaders11/default_amb_dir_VS.h"
#include "shaders11/default_all_VS.h"
#include "shaders11/uvxform_amb_VS.h"
#include "shaders11/uvxform_amb_dir_VS.h"
#include "shaders11/uvxform_all_VS.h"
#include "shaders11/default_pp_VS.h"
#include "shaders11/uvxform_pp_VS.h"
#include "shaders11/default_PS.h"
#include "shaders11/default_tex_PS.h"
#include "shaders11/default_pp_PS.h"
#include "shaders11/default_tex_pp_PS.h"
#include "shaders11/default_toon_PS.h"
#include "shaders11/default_tex_toon_PS.h"
#include "shaders11/outline_VS.h"
#include "shaders11/outline_PS.h"
#include "shaders11/im2d_VS.h"
#include "shaders11/im2d_PS.h"
#include "shaders11/im2d_tex_PS.h"
}
#endif
#ifdef RW_VULKAN
namespace spv {
#include "shadersvk/default_amb_VS.h"
#include "shadersvk/default_amb_dir_VS.h"
#include "shadersvk/default_all_VS.h"
#include "shadersvk/uvxform_amb_VS.h"
#include "shadersvk/uvxform_amb_dir_VS.h"
#include "shadersvk/uvxform_all_VS.h"
#include "shadersvk/default_pp_VS.h"
#include "shadersvk/uvxform_pp_VS.h"
#include "shadersvk/default_PS.h"
#include "shadersvk/default_tex_PS.h"
#include "shadersvk/default_pp_PS.h"
#include "shadersvk/default_tex_pp_PS.h"
#include "shadersvk/default_toon_PS.h"
#include "shadersvk/default_tex_toon_PS.h"
#include "shadersvk/outline_VS.h"
#include "shadersvk/outline_PS.h"
#include "shadersvk/im2d_VS.h"
#include "shadersvk/im2d_PS.h"
#include "shadersvk/im2d_tex_PS.h"
}
#endif

void
createDefaultShaders(void)
{
	default_amb_VS = createVertexShader(RWD3D_SHADER(default_amb_VS));
	assert(default_amb_VS);
	default_amb_dir_VS = createVertexShader(RWD3D_SHADER(default_amb_dir_VS));
	assert(default_amb_dir_VS);
	default_all_VS = createVertexShader(RWD3D_SHADER(default_all_VS));
	assert(default_all_VS);

	uvxform_amb_VS = createVertexShader(RWD3D_SHADER(uvxform_amb_VS));
	assert(uvxform_amb_VS);
	uvxform_amb_dir_VS = createVertexShader(RWD3D_SHADER(uvxform_amb_dir_VS));
	assert(uvxform_amb_dir_VS);
	uvxform_all_VS = createVertexShader(RWD3D_SHADER(uvxform_all_VS));
	assert(uvxform_all_VS);

	default_pp_VS = createVertexShader(RWD3D_SHADER(default_pp_VS));
	assert(default_pp_VS);
	uvxform_pp_VS = createVertexShader(RWD3D_SHADER(uvxform_pp_VS));
	assert(uvxform_pp_VS);

	// The cel look and the hull that goes round it.
	default_toon_PS = createPixelShader(RWD3D_SHADER(default_toon_PS));
	assert(default_toon_PS);
	default_tex_toon_PS = createPixelShader(RWD3D_SHADER(default_tex_toon_PS));
	assert(default_tex_toon_PS);

	// **Said out loud, because these two are the only ps_3_0 shaders in the D3D9
	// tree.**
	//
	// createPixelShader hands back nil on a device that will not have it, and
	// the asserts above are compiled out of a release build -- so a card
	// without Shader Model 3 would take the cel path and draw it with no pixel
	// shader at all, which is a black screen with no explanation anywhere.
	// Everything else in the renderer is ps_2_0 and unaffected. A D3D11 device
	// takes shader model 4 or nothing, so there it cannot happen.
	if(default_toon_PS == nil || default_tex_toon_PS == nil)
		printf("bfbb: this card has no Shader Model 3; the cel look needs it "
		       "for its antialiased bands. Turn experimental.toon off.\n");

	outline_VS = createVertexShader(RWD3D_SHADER(outline_VS));
	assert(outline_VS);
	outline_PS = createPixelShader(RWD3D_SHADER(outline_PS));
	assert(outline_PS);

	default_PS = createPixelShader(RWD3D_SHADER(default_PS));
	assert(default_PS);
	default_tex_PS = createPixelShader(RWD3D_SHADER(default_tex_PS));
	assert(default_tex_PS);
	default_pp_PS = createPixelShader(RWD3D_SHADER(default_pp_PS));
	assert(default_pp_PS);
	default_tex_pp_PS = createPixelShader(RWD3D_SHADER(default_tex_pp_PS));
	assert(default_tex_pp_PS);

	im2d_VS = createVertexShader(RWD3D_SHADER(im2d_VS));
	assert(im2d_VS);
	im2d_PS = createPixelShader(RWD3D_SHADER(im2d_PS));
	assert(im2d_PS);
	im2d_tex_PS = createPixelShader(RWD3D_SHADER(im2d_tex_PS));
	assert(im2d_tex_PS);
}

void
destroyDefaultShaders(void)
{
	destroyVertexShader(default_amb_VS);
	default_amb_VS = nil;
	destroyVertexShader(default_amb_dir_VS);
	default_amb_dir_VS = nil;
	destroyVertexShader(default_all_VS);
	default_all_VS = nil;
	destroyVertexShader(uvxform_amb_VS);
	uvxform_amb_VS = nil;
	destroyVertexShader(uvxform_amb_dir_VS);
	uvxform_amb_dir_VS = nil;
	destroyVertexShader(uvxform_all_VS);
	uvxform_all_VS = nil;
	destroyVertexShader(default_pp_VS);
	default_pp_VS = nil;
	destroyVertexShader(uvxform_pp_VS);
	uvxform_pp_VS = nil;

	destroyPixelShader(default_toon_PS);
	default_toon_PS = nil;
	destroyPixelShader(default_tex_toon_PS);
	default_tex_toon_PS = nil;
	destroyVertexShader(outline_VS);
	outline_VS = nil;
	destroyPixelShader(outline_PS);
	outline_PS = nil;

	destroyPixelShader(default_PS);
	default_PS = nil;
	destroyPixelShader(default_tex_PS);
	default_tex_PS = nil;
	destroyPixelShader(default_pp_PS);
	default_pp_PS = nil;
	destroyPixelShader(default_tex_pp_PS);
	default_tex_pp_PS = nil;

	destroyVertexShader(im2d_VS);
	im2d_VS = nil;
	destroyPixelShader(im2d_PS);
	im2d_PS = nil;
	destroyPixelShader(im2d_tex_PS);
	im2d_tex_PS = nil;
}


#ifdef RW_D3D9

// The lights the fixed-function stage will use, handed to the device. Split
// out so that an atomic and a loose primitive can share it, as the shader
// path's uploadLights is shared by lightingCB_Shader's two overloads.
static void
applyLights_Fix(WorldLights *lights)
{
	WorldLights &lightData = *lights;

	int i, n;
	RGBA amb;
	D3DLIGHT9 light;
	light.Type = D3DLIGHT_DIRECTIONAL;
	//light.Diffuse =  { 0.8f, 0.8f, 0.8f, 1.0f };
	light.Specular = { 0.0f, 0.0f, 0.0f, 0.0f };
	light.Ambient =  { 0.0f, 0.0f, 0.0f, 0.0f };
	light.Position = { 0.0f, 0.0f, 0.0f };
	//light.Direction = { 0.0f, 0.0f, -1.0f };
	light.Range = 0.0f;
	light.Falloff = 0.0f;
	light.Attenuation0 = 0.0f;
	light.Attenuation1 = 0.0f;
	light.Attenuation2 = 0.0f;
	light.Theta = 0.0f;
	light.Phi = 0.0f;

	convColor(&amb, &lightData.ambient);
	d3d::setRenderState(D3DRS_AMBIENT, D3DCOLOR_RGBA(amb.red, amb.green, amb.blue, amb.alpha));

	n = 0;
	for(i = 0; i < lightData.numDirectionals; i++){
		if(n >= MAX_LIGHTS)
			return;
		Light *l = lightData.directionals[i];
		light.Type = D3DLIGHT_DIRECTIONAL;
		light.Diffuse =  *(D3DCOLORVALUE*)&l->color;
		light.Direction = *(D3DVECTOR*)&l->getFrame()->getLTM()->at;
		d3ddevice->SetLight(n, &light);
		d3ddevice->LightEnable(n, TRUE);
		n++;
	}

	for(i = 0; i < lightData.numLocals; i++){
		if(n >= MAX_LIGHTS)
			return;
		Light *l = lightData.locals[i];
		switch(l->getType()){
		case Light::POINT:
			light.Type = D3DLIGHT_POINT;
			light.Diffuse =  *(D3DCOLORVALUE*)&l->color;
			light.Position = *(D3DVECTOR*)&l->getFrame()->getLTM()->pos;
			light.Direction.x = 0.0f;
			light.Direction.y = 0.0f;
			light.Direction.z = 0.0f;
			light.Range = l->radius;
			light.Falloff = 1.0f;
			light.Attenuation0 = 1.0f;
			light.Attenuation1 = 0.0f/l->radius;
			light.Attenuation2 = 5.0f/(l->radius*l->radius);
			d3ddevice->SetLight(n, &light);
			d3ddevice->LightEnable(n, TRUE);
			n++;
			break;

		case Light::SPOT:
			light.Type = D3DLIGHT_SPOT;
			light.Diffuse =  *(D3DCOLORVALUE*)&l->color;
			light.Position = *(D3DVECTOR*)&l->getFrame()->getLTM()->pos;
			light.Direction = *(D3DVECTOR*)&l->getFrame()->getLTM()->at;
			light.Range = l->radius;
			light.Falloff = 1.0f;
			light.Attenuation0 = 1.0f;
			light.Attenuation1 = 0.0f/l->radius;
			light.Attenuation2 = 5.0f/(l->radius*l->radius);
			light.Theta = l->getAngle()*2.0f;
			light.Phi = light.Theta;
			d3ddevice->SetLight(n, &light);
			d3ddevice->LightEnable(n, TRUE);
			n++;
			break;

		case Light::SOFTSPOT:
			light.Type = D3DLIGHT_SPOT;
			light.Diffuse =  *(D3DCOLORVALUE*)&l->color;
			light.Position = *(D3DVECTOR*)&l->getFrame()->getLTM()->pos;
			light.Direction = *(D3DVECTOR*)&l->getFrame()->getLTM()->at;
			light.Range = l->radius;
			light.Falloff = 1.0f;
			light.Attenuation0 = 1.0f;
			light.Attenuation1 = 0.0f/l->radius;
			light.Attenuation2 = 5.0f/(l->radius*l->radius);
			light.Theta = 0.0f;
			light.Phi = l->getAngle()*2.0f;
			d3ddevice->SetLight(n, &light);
			d3ddevice->LightEnable(n, TRUE);
			n++;
			break;
		}
	}

	for(; n < MAX_LIGHTS; n++)
		d3ddevice->LightEnable(n, FALSE);
}

void
lightingCB_Fix(Atomic *atomic)
{
	WorldLights lightData;
	Light *directionals[8];
	Light *locals[8];
	lightData.directionals = directionals;
	lightData.numDirectionals = 8;
	lightData.locals = locals;
	lightData.numLocals = 8;

	((World*)engine->currentWorld)->enumerateLights(atomic, &lightData);
	applyLights_Fix(&lightData);
}

void
lightingCB_Fix(void)
{
	WorldLights lightData;
	Light *directionals[8];
	Light *locals[8];
	lightData.directionals = directionals;
	lightData.numDirectionals = 8;
	lightData.locals = locals;
	lightData.numLocals = 8;

	((World*)engine->currentWorld)->enumerateLights(&lightData);
	applyLights_Fix(&lightData);
}

#endif


struct LightVS
{
	V3d color; float param0;
	V3d position; float param1;
	V3d direction; float param2;
};

void
setAmbient(const RGBAf &color)
{
	if(!equal(d3dShaderState.ambient, color)){
		d3dShaderState.ambient = color;
		setVertexShaderConstantF(VSLOC_ambLight, (float*)&color, 1);
		// The pixel stage gets it too, whether or not per-pixel lighting is on
		// right now. Both uploads sit inside the same cache test on purpose: if
		// this one were conditional, turning the setting on would not upload
		// until the ambient next changed, which for most levels is never.
		setPixelShaderConstantF(PSLOC_ppAmbient, (float*)&color, 1);
	}
}

void
setNumLights(int numDir, int numPoint, int numSpot)
{
	static int32 numLights[4*3];
	if(d3dShaderState.numDir != numDir ||
	   d3dShaderState.numPoint != numPoint ||
	   d3dShaderState.numSpot != numSpot){
		numLights[0] = d3dShaderState.numDir = numDir;
		numLights[4] = d3dShaderState.numPoint = numPoint;
		numLights[8] = d3dShaderState.numSpot = numSpot;
		setVertexShaderConstantI(VSLOC_numLights, numLights, 3);
	}
}

// The cel look's state. Held rather than pushed where it is set: some of it is
// set before the device exists, and all of it is per-draw rather than per-frame.
// How much brighter than authored every light burns. The kits were lit for a
// television and read dark on a modern display; scaled on the way to the
// uniform, so nothing the application owns is modified.
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

void *default_toon_PS;
void *default_tex_toon_PS;
void *outline_VS;
void *outline_PS;

// y is how much of the traced model shade to take; see toonConstants.h, which
// says why that slot and not a new one.
static float32 toonParams[4] = { 0.0f, 0.0f, 1.0f, 0.0f };

// The stretch on a scaled ink: saturation, then a curve. 1 and 1 is the plain
// multiply this had before either existed. See outline_PS.hlsl.
static float32 outlineInk[4] = { 1.0f, 1.0f, 0.0f, 0.0f };
static float32 toonLightDir[4] = { 0.0f, -1.0f, 0.0f, 0.0f };
static float32 toonRoom[4] = { 1.0f, 1.0f, 1.0f, 0.0f };
// x how flat a character's colours are cut, y which ramp row he is drawn with,
// z whether this draw is a character at all, w how far the light term is
// wrapped round the far side. See toonConstants.h.
static float32 toonExtra[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

// x rim strength, y where the rim starts, z how far the baked colour darkens
// the lookup, w how hard the shading edges are.
static float32 toonExtra2[4] = { 0.0f, 0.65f, 0.0f, 0.0f };

// x how strong the glint is, y how far round the half vector it starts, z how
// the rim light is put on. See toonConstants.h.
static float32 toonExtra3[4] = { 0.0f, 0.85f, 0.0f, 0.0f };
static float32 outlineColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
static float32 outlineColor2[4] = { 0.0f, 0.0f, 0.0f, -1.0e30f };
static float32 outlineFlags[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
// x is +1 for a hull pushed out of the surface, -1 for one pushed into it.
static float32 outlineSign[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
static int32 outlineMode;
static Texture *toonRamp;

// What the application asked for, kept apart from what gets uploaded: the
// direction and the room are resolved per draw from the lights unless the
// application has overridden them.
static bool32 toonLightDirSet;
static bool32 toonRoomSet;

// **How bright the room is, which the lights cannot say.**
//
// The room colour here is every light summed and then held at one, so a house
// indoors comes out the same as open sunlight: four lights pointing four ways
// add up past one whether or not any surface sees more than one of them.
//
// So the level's brightness is not worked out here. The application measures it
// off the paint the artists baked -- the one record of how bright a room was
// meant to be -- and hands it over as a scale on the room. 1 leaves the lights'
// own answer standing.
static float32 toonRoomScale = 1.0f;

void
setToonShading(bool32 enable, float32 bands, float32 saturation, float32 strength)
{
	(void)bands;	// the ramp texture carries the band count

	toonParams[0] = enable ? 1.0f : 0.0f;
	toonParams[2] = saturation;
	toonParams[3] = strength;
}

bool32
getToonShading(void)
{
	return toonParams[0] != 0.0f;
}

void
setToonFlatten(float32 colors)
{
	toonExtra[0] = colors;
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
}

// A hard highlight, for the draws that are wet. Set per draw and cleared after,
// the way the ramp row is.
void
setToonGloss(float32 amount, float32 edge)
{
	toonExtra3[0] = amount;
	toonExtra3[1] = edge;
}

void
setToonRimBlend(int32 mode)
{
	toonExtra3[2] = (float32)mode;
}

// Whether this draw takes the cel look with no lights on it.
//
// **The look needs no lights and the renderer needed one anyway.** The key
// direction and the room colour are resolved before the draw, so the shader
// reads neither light array -- but a draw with no lights is usually art that
// wants nothing done to it, and the light count stood in for that. The goo is
// the case where the two come apart: a wide surface of liquid, drawn with no
// light kit at all, that wants bands more than anything else in a scene does.
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
setToonRampRow(int32 row)
{
	toonExtra[1] = (float32)row;
}

// The floor under the hull's width, as world units per unit of view depth. The
// application works it out; the sum needs the camera's view window and the size
// of the picture, and neither is known here.
void
setOutlineMinWidth(float32 perDepth)
{
	outlineFlags[2] = perDepth < 0.0f ? 0.0f : perDepth;
}

// The ceiling over it, the same units. Zero for none, which is what a hull with
// no cap gets: it keeps one world width and swells on screen as the camera
// closes.
void
setOutlineMaxWidth(float32 perDepth)
{
	outlineFlags[3] = perDepth < 0.0f ? 0.0f : perDepth;
}

void
setToonRamp(Texture *tex)
{
	toonRamp = tex;
}

// What colour it is in here, named rather than resolved from the lights. A draw
// with no lights has nothing to resolve it from and would come out black.
void
setToonRoomColor(float32 r, float32 g, float32 b)
{
	toonRoom[0] = r;
	toonRoom[1] = g;
	toonRoom[2] = b;
	toonRoomSet = 1;
}

void
setToonRoomTint(float32 r, float32 g, float32 b)
{
	setToonRoomColor(r, g, b);

	// The scene only ever names a room for a CHARACTER, so the same call says
	// so. Everything that is wrong on a background -- the flattening, the rim,
	// the baked occlusion, the hardened normals -- hangs off this.
	//
	// It used to ride in toonRoom.w, which worked while an unset room meant
	// something. It does not any more: uploadLights fills one in for every
	// draw, so the two facts had to come apart. The goo took them apart again:
	// it names a room and is not a character, so it calls the half above.
	toonExtra[2] = 1.0f;
}

void
clearToonRoomTint(void)
{
	toonRoomSet = 0;
	toonExtra[2] = 0.0f;
}

void
setToonLightDir(float32 x, float32 y, float32 z)
{
	toonLightDir[0] = x;
	toonLightDir[1] = y;
	toonLightDir[2] = z;
	toonLightDirSet = 1;
}

void
clearToonLightDir(void)
{
	toonLightDirSet = 0;
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
}

void
setOutlineLower(float32 r, float32 g, float32 b)
{
	outlineColor2[0] = r;
	outlineColor2[1] = g;
	outlineColor2[2] = b;
}

void
setOutlineFlat(bool32 upper, bool32 lower)
{
	outlineFlags[0] = upper ? 1.0f : 0.0f;
	outlineFlags[1] = lower ? 1.0f : 0.0f;
}

void
setOutlineSplit(float32 y)
{
	outlineColor2[3] = y;
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
// cross into the floor and the ink come out on top of it. See default_VS.hlsl.
// Below zero reads it from further IN instead, and -1 lands on the vertex the
// hull was pushed from: the model's own surface, with none of the copy's
// standing in space left in it.
void
setOutlineDepthBias(float32 widths)
{
	outlineSign[1] = widths;
}

// How much of the shade the scene traced from its placed models to take, 0 for
// none. Per draw: the world takes it and the things standing in it do not.
void
setToonModelShade(float32 amount)
{
	toonParams[1] = amount;
}

void
setToonRoomScale(float32 scale)
{
	toonRoomScale = scale;
}

// Push what the toon pixel shaders read. Called once a draw, after the lights
// are known, because two of the three are resolved from them.
void
uploadToonConstants(void)
{
	if(!getToonShading())
		return;

	float32 room[4];

	room[0] = toonRoom[0];
	room[1] = toonRoom[1];
	room[2] = toonRoom[2];
	room[3] = toonRoom[3];

	// One factor across all three, so a level's hue is untouched and a blue room
	// stays blue as it darkens.
	room[0] *= toonRoomScale;
	room[1] *= toonRoomScale;
	room[2] *= toonRoomScale;

	// **Where the eye is, uploaded here rather than when a camera begins.**
	//
	// A frame is not one camera. Measured in gl01: the scene's camera begins
	// eight passes, a projection camera parked 2000 units overhead begins six,
	// and the glow's three begin from the world origin -- fourteen passes, one
	// eye position between them. Whoever went last owned it, so a surface drawn
	// after the glow worked out its eye vector from the origin: a direction that
	// depends on where the model stands and not at all on where you are, which
	// holds still on the model while the camera moves round it.
	//
	// This runs immediately before each draw, off the camera doing the drawing,
	// so there is no window for another pass to leave something behind. The GL3
	// side never had the bug: it undoes the view matrix in the vertex shader,
	// and that matrix is uploaded per draw already.
	{
		Camera *cam = engine->currentCamera;

		if(cam){
			V3d *p = &cam->getFrame()->getLTM()->pos;
			float32 campos[4] = { p->x, p->y, p->z, 1.0f };

			setVertexShaderConstantF(VSLOC_toonCamPos, campos, 1);
		}
	}

	setTexture(3, toonRamp);
	setPixelShaderConstantF(PSLOC_toonParams, toonParams, 1);
	setPixelShaderConstantF(PSLOC_toonLightDir, toonLightDir, 1);
	setPixelShaderConstantF(PSLOC_toonRoom, room, 1);
	setPixelShaderConstantF(PSLOC_toonExtra, toonExtra, 1);
	setPixelShaderConstantF(PSLOC_toonExtra2, toonExtra2, 1);
	setPixelShaderConstantF(PSLOC_toonExtra3, toonExtra3, 1);
}

void
setOutlineInk(float32 saturation, float32 gamma)
{
	outlineInk[0] = saturation;
	outlineInk[1] = gamma;
}

void
uploadOutlineConstants(void)
{
	setPixelShaderConstantF(PSLOC_outlineInk, outlineInk, 1);
	setVertexShaderConstantF(VSLOC_outlineColor, outlineColor, 1);
	setVertexShaderConstantF(VSLOC_outlineColor2, outlineColor2, 1);
	setVertexShaderConstantF(VSLOC_outlineFlags, outlineFlags, 1);
	setVertexShaderConstantF(VSLOC_outlineSign, outlineSign, 1);
}

int32
uploadLights(WorldLights *lightData)
{
	int i;
	int bits = 0;
	float32 firstLight[4];
	firstLight[0] = 0;	// directional
	firstLight[1] = 0;	// point
	firstLight[2] = 0;	// spot
	firstLight[3] = 0;

	if(lightData->numAmbients)
		bits |= VSLIGHT_AMBIENT;

	LightVS directionals[8];
	LightVS points[8];
	LightVS spots[8];
	for(i = 0; i < lightData->numDirectionals; i++){
		Light *l = lightData->directionals[i];
		directionals[i].color.x = l->color.red*lightIntensity;
		directionals[i].color.y = l->color.green*lightIntensity;
		directionals[i].color.z = l->color.blue*lightIntensity;
		directionals[i].direction = l->getFrame()->getLTM()->at;
		bits |= VSLIGHT_DIRECT;
	}

	// The two things the toon pixel shader would otherwise have to loop for.
	//
	// The room is every light summed as if the surface faced all of them at
	// once -- a meaningless quantity for lighting a surface and the right one
	// for asking what colour it is in here -- scaled down to fit rather than
	// clamped per channel, so a blue room stays blue instead of clipping to
	// cyan. The key is simply the brightest directional.
	if(getToonShading()){
		float32 room[3];
		float32 bestLum = -1.0f;

		room[0] = lightData->ambient.red;
		room[1] = lightData->ambient.green;
		room[2] = lightData->ambient.blue;

		for(i = 0; i < lightData->numDirectionals; i++){
			Light *l = lightData->directionals[i];
			float32 lum = l->color.red + l->color.green + l->color.blue;

			room[0] += l->color.red;
			room[1] += l->color.green;
			room[2] += l->color.blue;

			if(lum > bestLum && !toonLightDirSet){
				bestLum = lum;
				toonLightDir[0] = l->getFrame()->getLTM()->at.x;
				toonLightDir[1] = l->getFrame()->getLTM()->at.y;
				toonLightDir[2] = l->getFrame()->getLTM()->at.z;
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

			toonRoom[0] = room[0];
			toonRoom[1] = room[1];
			toonRoom[2] = room[2];
		}
	}

	int np = 0;
	int ns = 0;
	for(i = 0; i < lightData->numLocals; i++){
		Light *l = lightData->locals[i];

		switch(l->getType()){
		case Light::POINT:
			points[np].color.x = l->color.red;
			points[np].color.y = l->color.green;
			points[np].color.z = l->color.blue;
			points[np].param0 = l->radius;
			points[np].position = l->getFrame()->getLTM()->pos;
			np++;
			bits |= VSLIGHT_POINT;
			break;
		case Light::SPOT:
		case Light::SOFTSPOT:
			spots[ns].color.x = l->color.red;
			spots[ns].color.y = l->color.green;
			spots[ns].color.z = l->color.blue;
			spots[ns].param0 = l->radius;
			spots[ns].position = l->getFrame()->getLTM()->pos;
			spots[ns].direction = l->getFrame()->getLTM()->at;
			spots[ns].param1 = l->minusCosAngle;
			// lower bound of falloff
			if(l->getType() == Light::SOFTSPOT)
				spots[ns].param2 = 0.0f;
			else
				spots[ns].param2 = 1.0f;
			bits |= VSLIGHT_SPOT;
			ns++;
			break;
		}
	}

	firstLight[0] = 0;
	int numDir = lightData->numDirectionals;
	firstLight[1] = numDir + firstLight[0];
	int numPoint = np;
	firstLight[2] = numPoint + firstLight[1];
	int numSpot = ns;

	setNumLights(numDir, numPoint, numSpot);
	if(d3dShaderState.lightOffset[0] != firstLight[0] ||
	   d3dShaderState.lightOffset[1] != firstLight[1] ||
	   d3dShaderState.lightOffset[2] != firstLight[2]){
		d3dShaderState.lightOffset[0] = firstLight[0];
		d3dShaderState.lightOffset[1] = firstLight[1];
		d3dShaderState.lightOffset[2] = firstLight[2];
		setVertexShaderConstantF(VSLOC_lightOffset, firstLight, 1);
	}

	// The per-pixel path's copy of the directional lights.
	//
	// All MAX_LIGHTS slots go up every time, not just the ones in use: the
	// pixel shader is ps_2_0, which has neither loops nor branches, so it
	// evaluates a fixed eight lights and the only way to say "no light here" is
	// a colour of zero. Uploading the whole array also means there is nothing
	// stale to correct when the light count drops between atomics.
	//
	// Unconditional on the setting would cost 16 registers per atomic for
	// nothing, and there is no staleness to fear from skipping it: this runs
	// per atomic, so the frame after the setting changes is already right.
	if(perPixelLighting){
		float32 psColor[MAX_LIGHTS*4];
		float32 psDir[MAX_LIGHTS*4];
		memset(psColor, 0, sizeof(psColor));
		memset(psDir, 0, sizeof(psDir));
		for(i = 0; i < lightData->numDirectionals; i++){
			psColor[i*4+0] = directionals[i].color.x;
			psColor[i*4+1] = directionals[i].color.y;
			psColor[i*4+2] = directionals[i].color.z;
			psDir[i*4+0] = directionals[i].direction.x;
			psDir[i*4+1] = directionals[i].direction.y;
			psDir[i*4+2] = directionals[i].direction.z;
		}
		setPixelShaderConstantF(PSLOC_ppLightColor, psColor, MAX_LIGHTS);
		setPixelShaderConstantF(PSLOC_ppLightDirection, psDir, MAX_LIGHTS);
	}

	int32 off = VSLOC_lights;
	if(numDir)
		setVertexShaderConstantF(off, (float*)&directionals, numDir*3);
	off += numDir*3;

	if(numPoint)
		setVertexShaderConstantF(off, (float*)&points, numPoint*3);
	off += numPoint*3;

	if(numSpot)
		setVertexShaderConstantF(off, (float*)&spots, numSpot*3);

	return bits;
}

int32
lightingCB_Shader(Atomic *atomic)
{
	WorldLights lightData;
	Light *directionals[8];
	Light *locals[8];
	lightData.directionals = directionals;
	lightData.numDirectionals = 8;
	lightData.locals = locals;
	lightData.numLocals = 8;

	if(atomic->geometry->flags & rw::Geometry::LIGHT){
		((World*)engine->currentWorld)->enumerateLights(atomic, &lightData);

		// Scaled like the directionals below, or the setting would brighten
		// only half the rig and warm every shadow.
		RGBAf amb = lightData.ambient;

		amb.red *= lightIntensity;
		amb.green *= lightIntensity;
		amb.blue *= lightIntensity;
		setAmbient(amb);

		return uploadLights(&lightData);
	}else{
		static const RGBAf black = { 0.0f, 0.0f, 0.0f, 0.0f };
		setAmbient(black);
		setNumLights(0, 0, 0);
		return 0;
	}
}

int32
lightingCB_Shader(void)
{
	WorldLights lightData;
	Light *directionals[8];
	Light *locals[8];
	lightData.directionals = directionals;
	lightData.numDirectionals = 8;
	lightData.locals = locals;
	lightData.numLocals = 8;

	((World*)engine->currentWorld)->enumerateLights(&lightData);
	setAmbient(lightData.ambient);
	return uploadLights(&lightData);
}

static RawMatrix identityXform = {
	{ 1.0f, 0.0f, 0.0f }, 0.0f,
	{ 0.0f, 1.0f, 0.0f }, 0.0f,
	{ 0.0f, 0.0f, 1.0f }, 0.0f,
	{ 0.0f, 0.0f, 0.0f }, 1.0f
};

void
uploadMatrices(void)
{
	RawMatrix combined;
	Camera *cam = engine->currentCamera;
	setVertexShaderConstantF(VSLOC_world, (float*)&identityXform, 4);
	setVertexShaderConstantF(VSLOC_normal, (float*)&identityXform, 4);

	RawMatrix::mult(&combined, &cam->devView, &cam->devProj);
	setVertexShaderConstantF(VSLOC_combined, (float*)&combined, 4);
}

void
uploadMatrices(Matrix *worldMat)
{
	RawMatrix combined, world, worldview;
	Camera *cam = engine->currentCamera;
	convMatrix(&world, worldMat);
	setVertexShaderConstantF(VSLOC_world, (float*)&world, 4);

	RawMatrix normal;
	RawMatrix::normalMatrix(&normal, &world);
	setVertexShaderConstantF(VSLOC_normal, (float*)&normal, 4);

	RawMatrix::mult(&worldview, &world, &cam->devView);
	RawMatrix::mult(&combined, &worldview, &cam->devProj);
	setVertexShaderConstantF(VSLOC_combined, (float*)&combined, 4);
}



#endif

}
}
