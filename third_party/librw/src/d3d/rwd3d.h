#ifdef WITH_D3D
#ifdef RW_D3D9
#include <d3d9.h>
#endif
#ifdef RW_D3D11
#include <d3d11.h>
#include <dxgi.h>
#endif
#endif

#ifdef RW_VULKAN
struct SDL_Window;
#endif

namespace rw {

#if !defined(RW_D3D9) && !defined(RW_D3D11)
#ifdef _D3D9_H_
#error "please don't include d3d9.h for non-d3d9 platforms"
#endif
#endif

namespace d3d {

#ifdef RW_D3D_ANY
struct EngineOpenParams : rw::EngineOpenParams
{
#if defined(RW_D3D9) || defined(RW_D3D11)
#ifdef _WINDOWS_
	HWND window;
#else
	uint32 please_include_windows_h;
#endif
#endif
#ifdef RW_VULKAN
	// The window the Vulkan device presents to. The application makes it,
	// with SDL_WINDOW_VULKAN; the Direct3D devices take the HWND instead.
	::SDL_Window *sdlWindow;
#endif
};
#endif

extern bool32 isP8supported;

// A fixed-size screen the game renders into, scaled onto the window at present
// time with its aspect ratio kept.
//
// Without it a camera raster smaller than the window renders at its own size in
// the window's top-left corner, because setViewport takes the viewport from the
// raster while the back buffer follows the client rect. That is right for a
// resolution-independent game and wrong for one whose framebuffer is a fixed
// part of its design: it wants the picture SCALED, not a bigger window with the
// same picture in a corner of it.
//
// When set, the default render target becomes an off-screen surface of this size
// rather than the back buffer, so every camera that renders to the frame buffer
// lands there whatever the window is doing. showRaster then stretches it into a
// centred rectangle of the back buffer and clears what is left to black.
//
// Zero disables it and restores the stock behaviour. Safe to call before the
// device exists; the surfaces are created with it and recreated across a reset.
void setVirtualScreen(int32 width, int32 height);
void getVirtualScreen(int32 *width, int32 *height);
// Samples the virtual screen is drawn with. Set before the size, because the
// surfaces are made when the size is set.
void setVirtualScreenSamples(int32 samples);
// What was actually granted -- 1 when multisampling is off or was refused.
int32 getVirtualScreenSamples(void);
// Evaluate lighting per pixel rather than per vertex, in the default, uvxform
// and skin pipelines.
//
// Only where it can change the picture: an atomic lit by ambient alone gets
// nothing from it, and one reached by a point or a spot light falls back to the
// per-vertex path for that draw, because the per-pixel shaders do directional
// lights only. Geometry with no normals enumerates no directional lights at
// all, so it is never affected either.
//
// Safe to call before the device exists, and safe to change at any time -- the
// lights are uploaded fresh per atomic and the material constants go to both
// shader stages whatever this says.
void setPerPixelLightingEnabled(bool32 enable);

// The cel look. Mirrors rw::gl3's, and the arithmetic in the shaders is the
// same -- see toonConstants.h for the one place they differ and why.
//
// **The key direction and the room colour are worked out on the way to the
// uniform, not in the shader.** ps_2_0 has neither loops nor branches, so
// picking the brightest of eight lights per pixel means eight unrolled
// comparisons. They are per-draw quantities, so uploadLights resolves them
// once. That is what makes the toon path cheaper than the lighting it replaces
// rather than an addition to it.
enum OutlineMode
{
	OUTLINE_NONE = 0,
	OUTLINE_PLAIN,
	OUTLINE_TWOTONE
};

// How much brighter than authored every light from a light kit burns. 1 is as
// the level says. Applied on the way to the uniform, so nothing the
// application owns is modified.
void setLightIntensity(float32 scale);
float32 getLightIntensity(void);

void setToonShading(bool32 enable, float32 bands, float32 saturation, float32 strength);
void setToonRamp(Texture *tex);

// How many shades a character's colours are cut down to, keeping their hue. 0
// leaves them alone; the world is never touched.
void setToonFlatten(float32 colors);

// The rest of the look, none of which is lighting. See rwgl3.h, which declares
// the same call and says what each argument does.
void setToonLook(float32 wrap, float32 rim, float32 rimEdge, float32 occlusion,
                 float32 hardness);

// A hard highlight on a surface the light bounces off. See rwgl3.h.
void setToonGloss(float32 amount, float32 edge);

// How the rim light is put on: 0 towards the room's colour, 1 screened over the
// surface, 2 added. See rwgl3.h, which says what each looks like.
void setToonRimBlend(int32 mode);

// Whether the next draw takes the cel look although no light is on it. See
// rwgl3.h, which declares the same call and says what it is for.
void setToonUnlit(bool32 on);
bool32 getToonUnlit(void);

// What colour it is in here, without saying the draw is a character. See
// setToonRoomTint, which says both.
void setToonRoomColor(float32 r, float32 g, float32 b);

// Which of the stacked ramps the next draw is shaded with.
void setToonRampRow(int32 row);

// A floor under the hull's width, in world units per unit of view depth, so a
// distant character keeps a line instead of losing it below a pixel.
void setOutlineMinWidth(float32 perDepth);
void setOutlineMaxWidth(float32 perDepth);
void setToonRoomTint(float32 r, float32 g, float32 b);
void clearToonRoomTint(void);
void setToonLightDir(float32 x, float32 y, float32 z);
void clearToonLightDir(void);
bool32 getToonShading(void);

void setOutline(float32 r, float32 g, float32 b, float32 thickness);
void setOutlineLower(float32 r, float32 g, float32 b);
void setOutlineFlat(bool32 upper, bool32 lower);
// How much of the shade the application traced from its own models to take.
// Read per pixel off the prelight; see toonConstants.h.
void setToonModelShade(float32 amount);

// The stretch on an ink that is a scale on the surface: how much colour it
// keeps, and a curve on its brightness. outline_PS.hlsl says what each does.
void setOutlineInk(float32 saturation, float32 gamma);

// How bright the room is, as a scale on the colour the lights resolve to. 1 is
// the lights' own answer. d3drender.cpp says why they cannot answer it alone.
void setToonRoomScale(float32 scale);

void setOutlineSplit(float32 y);
void setOutlineMode(int32 mode);
int32 getOutlineMode(void);

// Whether the next hull is drawn for a model wound inside out: inflated
// along the negated normal, with back faces culled instead of front ones.
// Together those treat the mesh as wound the other way, which it is.
void setOutlineInverted(bool32 on);
void setOutlineDepthBias(float32 widths);
bool32 getOutlineInverted(void);

// Push what the toon shaders read. The pixel constants go once a draw, after
// the lights, because the direction and the room colour are resolved from
// them; the vertex ones go before the hull pass that reads them.
void uploadToonConstants(void);
void uploadOutlineConstants(void);

// Where the toon constants live. c27..c29 in the pixel shader, above
// perPixelConstants.h; c233..c235 in the vertex shader, above the skin
// pipeline's bone matrices.
enum
{
	// The outline pass alone, and uploaded right before it draws, so what any
	// other pass leaves in c2 does not matter.
	PSLOC_outlineInk = 2,

	PSLOC_toonParams = 27,
	PSLOC_toonLightDir = 28,
	PSLOC_toonRoom = 29,
	PSLOC_toonExtra = 30,
	PSLOC_toonExtra2 = 31,

	// Past what ps_2_0 has. toonConstants.h declares it for the TOON
	// permutations alone for that reason, and those are the only ps_3_0 shaders
	// here. outline_PS.hlsl shares the header and is ps_2_0.
	PSLOC_toonExtra3 = 32,

	VSLOC_outlineColor = 233,
	VSLOC_outlineColor2 = 234,
	VSLOC_outlineFlags = 235,

	// Where the camera is. The toon pixel shader wants the vector from the
	// surface to the eye, and a vertex shader here has no view matrix to
	// recover it from -- combinedMat has already swallowed the projection.
	VSLOC_toonCamPos = 236,

	// Which way the hull inflates: x is +1 out of the surface or -1 into it.
	// A model wound inside out has its normals pointing in, and pushing it
	// along them shrinks the copy instead of swelling it.
	VSLOC_outlineSign = 237
};

extern void *default_toon_PS;
extern void *default_tex_toon_PS;
extern void *outline_VS;
extern void *outline_PS;
bool32 getPerPixelLighting(void);
// Draw with D3D9's fixed-function transform, lighting and texture stages
// instead of vertex and pixel shaders. The bar it lowers the backend to is
// DX7-class hardware T&L; what it costs is every effect that is a shader.
//
// Set BEFORE the engine is opened and not changed afterwards. driverOpen picks
// the pipelines' render callbacks from it and createDefaultShaders is skipped
// entirely, so a device that never had a shader compiled for it cannot be
// asked for one later.
//
// This is a look-alike, not a match: fixed-function lighting has its own
// normalisation and attenuation, and the transform of a normal under a scaled
// matrix is D3DRS_NORMALIZENORMALS rather than the inverse transpose the
// vertex shader is handed.
void setFixedFunctionEnabled(bool32 enable);
bool32 getFixedFunction(void);
// The single-sampled picture, for anything that needs to read the frame back:
// the samples are collapsed into it on the way out. nil when there is no
// virtual screen, in which case the back buffer is what was drawn into.
// D3D9's own; see the note in rwd3dimpl.h on why it is declared this way.
namespace impl9 {
struct IDirect3DSurface9 *resolveVirtualScreen(void);
}
using impl9::resolveVirtualScreen;
// Copy what has been rendered so far into a camera texture, so a pass can
// sample the frame it is about to draw over. The raster must be a
// CAMERATEXTURE of the size getScreenExtent reports; the caller owns it.
//
// Called with no scene open. D3D9 cannot copy between surfaces while one is
// open, and a multisampled target has to be resolved first either way.
bool32 captureFrame(Raster *dst);
// Whether there is a device to draw with at all. A pass that runs before the
// engine has started, or after it has stopped, asks this rather than naming
// one backend's device pointer.
bool32 deviceOpen(void);


#ifdef RW_D3D9
#ifdef _D3D9_H_
extern IDirect3DDevice9 *d3ddevice;
void setD3dMaterial(D3DMATERIAL9 *mat9);
#endif
#endif

#ifdef RW_D3D_ANY

// What fxc called the array inside a compiled blob.
//
// The blobs themselves are included by bare name and found on the include path,
// which CMake points at shaders/ or shaders11/ depending on the build's shader
// model. The two trees carry the same file names and the same permutations, so
// the code that loads them is one copy.
#ifdef RW_D3D11
#define VS_NAME g_main
#define PS_NAME g_main
#else
#define VS_NAME g_vs20_main
#define PS_NAME g_ps20_main
#endif

#define COLOR_ARGB(a, r, g, b) ((rw::uint32)((((a)&0xff)<<24)|(((r)&0xff)<<16)|(((g)&0xff)<<8)|((b)&0xff)))

struct Im3DVertex
{
	V3d position;
	V3d normal;		// librw extension
	uint32 color;
	float32 u, v;

	void setX(float32 x) { this->position.x = x; }
	void setY(float32 y) { this->position.y = y; }
	void setZ(float32 z) { this->position.z = z; }
	void setNormalX(float32 x) { this->normal.x = x; }
	void setNormalY(float32 y) { this->normal.y = y; }
	void setNormalZ(float32 z) { this->normal.z = z; }
	void setColor(uint8 r, uint8 g, uint8 b, uint8 a) { this->color = COLOR_ARGB(a, r, g, b); }
	void setU(float32 u) { this->u = u; }
	void setV(float32 v) { this->v = v; }

	float getX(void) { return this->position.x; }
	float getY(void) { return this->position.y; }
	float getZ(void) { return this->position.z; }
	float getNormalX(void) { return this->normal.x; }
	float getNormalY(void) { return this->normal.y; }
	float getNormalZ(void) { return this->normal.z; }
	RGBA getColor(void) { return makeRGBA(this->color>>16 & 0xFF, this->color>>8 & 0xFF,
		this->color & 0xFF, this->color>>24 & 0xFF); }
	float getU(void) { return this->u; }
	float getV(void) { return this->v; }
};
extern RGBA im3dMaterialColor;
extern SurfaceProperties im3dSurfaceProps;

struct Im2DVertex
{
	float32 x, y, z;
	//float32 q;	// recipz no longer used because we have a vertex stage now
	float32 w;
	uint32 color;
	float32 u, v;

	void setScreenX(float32 x) { this->x = x; }
	void setScreenY(float32 y) { this->y = y; }
	void setScreenZ(float32 z) { this->z = z; }
	void setCameraZ(float32 z) { this->w = z; }
//	void setRecipCameraZ(float32 recipz) { this->q = recipz; }
	void setRecipCameraZ(float32 recipz) { this->w = 1.0f/recipz; }
	void setColor(uint8 r, uint8 g, uint8 b, uint8 a) { this->color = COLOR_ARGB(a, r, g, b); }
	void setU(float32 u, float recipZ) { this->u = u; }
	void setV(float32 v, float recipZ) { this->v = v; }

	float getScreenX(void) { return this->x; }
	float getScreenY(void) { return this->y; }
	float getScreenZ(void) { return this->z; }
//	float getCameraZ(void) { return 1.0f/this->q; }
//	float getRecipCameraZ(void) { return this->q; }
	float getCameraZ(void) { return this->w; }
	float getRecipCameraZ(void) { return 1.0f/this->w; }
	RGBA getColor(void) { return makeRGBA(this->color>>16 & 0xFF, this->color>>8 & 0xFF,
		this->color & 0xFF, this->color>>24 & 0xFF); }
	float getU(void) { return this->u; }
	float getV(void) { return this->v; }
};

#endif

// D3DFORMAT, for a unit that did not include d3d9.h -- which is most of them:
// only the D3D translation units define WITH_D3D. Keyed on the HEADER and not
// on RW_D3D9, because raster.cpp reads DXT format codes off a D3D raster while
// converting it for GL3, and a build that carries both backends compiles that
// with RW_D3D9 defined and d3d9.h nowhere in sight.
#ifndef _D3D9_H_
#ifndef MAKEFOURCC
#define MAKEFOURCC(ch0, ch1, ch2, ch3)                              \
            ((uint32)(uint8)(ch0) | ((uint32)(uint8)(ch1) << 8) |       \
            ((uint32)(uint8)(ch2) << 16) | ((uint32)(uint8)(ch3) << 24 ))
#endif
enum {
	D3DFMT_UNKNOWN              =  0,

	D3DFMT_R8G8B8               = 20,
	D3DFMT_A8R8G8B8             = 21,
	D3DFMT_X8R8G8B8             = 22,
	D3DFMT_R5G6B5               = 23,
	D3DFMT_X1R5G5B5             = 24,
	D3DFMT_A1R5G5B5             = 25,
	D3DFMT_A4R4G4B4             = 26,
	D3DFMT_R3G3B2               = 27,
	D3DFMT_A8                   = 28,
	D3DFMT_A8R3G3B2             = 29,
	D3DFMT_X4R4G4B4             = 30,
	D3DFMT_A2B10G10R10          = 31,
	D3DFMT_A8B8G8R8             = 32,
	D3DFMT_X8B8G8R8             = 33,
	D3DFMT_G16R16               = 34,
	D3DFMT_A2R10G10B10          = 35,
	D3DFMT_A16B16G16R16         = 36,

	D3DFMT_A8P8                 = 40,
	D3DFMT_P8                   = 41,

	D3DFMT_L8                   = 50,
	D3DFMT_A8L8                 = 51,
	D3DFMT_A4L4                 = 52,

	D3DFMT_V8U8                 = 60,
	D3DFMT_L6V5U5               = 61,
	D3DFMT_X8L8V8U8             = 62,
	D3DFMT_Q8W8V8U8             = 63,
	D3DFMT_V16U16               = 64,
	D3DFMT_A2W10V10U10          = 67,

	D3DFMT_UYVY                 = MAKEFOURCC('U', 'Y', 'V', 'Y'),
	D3DFMT_R8G8_B8G8            = MAKEFOURCC('R', 'G', 'B', 'G'),
	D3DFMT_YUY2                 = MAKEFOURCC('Y', 'U', 'Y', '2'),
	D3DFMT_G8R8_G8B8            = MAKEFOURCC('G', 'R', 'G', 'B'),
	D3DFMT_DXT1                 = MAKEFOURCC('D', 'X', 'T', '1'),
	D3DFMT_DXT2                 = MAKEFOURCC('D', 'X', 'T', '2'),
	D3DFMT_DXT3                 = MAKEFOURCC('D', 'X', 'T', '3'),
	D3DFMT_DXT4                 = MAKEFOURCC('D', 'X', 'T', '4'),
	D3DFMT_DXT5                 = MAKEFOURCC('D', 'X', 'T', '5'),

	D3DFMT_D16_LOCKABLE         = 70,
	D3DFMT_D32                  = 71,
	D3DFMT_D15S1                = 73,
	D3DFMT_D24S8                = 75,
	D3DFMT_D24X8                = 77,
	D3DFMT_D24X4S4              = 79,
	D3DFMT_D16                  = 80,

	D3DFMT_D32F_LOCKABLE        = 82,
	D3DFMT_D24FS8               = 83,

	// d3d9ex only
	/* Z-Stencil formats valid for CPU access */
	D3DFMT_D32_LOCKABLE         = 84,
	D3DFMT_S8_LOCKABLE          = 85,

	D3DFMT_L16                  = 81,

	D3DFMT_VERTEXDATA           =100,
	D3DFMT_INDEX16              =101,
	D3DFMT_INDEX32              =102,

	D3DFMT_Q16W16V16U16         =110,

	D3DFMT_MULTI2_ARGB8         = MAKEFOURCC('M','E','T','1'),

	// Floating point surface formats

	// s10e5 formats (16-bits per channel)
	D3DFMT_R16F                 = 111,
	D3DFMT_G16R16F              = 112,
	D3DFMT_A16B16G16R16F        = 113,

	// IEEE s23e8 formats (32-bits per channel)
	D3DFMT_R32F                 = 114,
	D3DFMT_G32R32F              = 115,
	D3DFMT_A32B32G32R32F        = 116,

	D3DFMT_CxV8U8               = 117,

	// d3d9ex only
	// Monochrome 1 bit per pixel format
	D3DFMT_A1                   = 118,
	// 2.8 biased fixed point
	D3DFMT_A2B10G10R10_XR_BIAS  = 119,
	// Binary format indicating that the data has no inherent type
	D3DFMT_BINARYBUFFER         = 199
};

enum {
	D3DLOCK_NOSYSLOCK     =  0,  // ignored
	D3DLOCK_DISCARD       =  0,  // ignored
	D3DPOOL_MANAGED       =  0,  // ignored
	D3DPT_POINTLIST       =  1,
	D3DPT_LINELIST        =  2,
	D3DPT_LINESTRIP       =  3,
	D3DPT_TRIANGLELIST    =  4,
	D3DPT_TRIANGLESTRIP   =  5,
	D3DPT_TRIANGLEFAN     =  6,


	D3DDECLTYPE_FLOAT1    =  0,  // 1D float expanded to (value, 0., 0., 1.)
	D3DDECLTYPE_FLOAT2    =  1,  // 2D float expanded to (value, value, 0., 1.)
	D3DDECLTYPE_FLOAT3    =  2,  // 3D float expanded to (value, value, value, 1.)
	D3DDECLTYPE_FLOAT4    =  3,  // 4D float
	D3DDECLTYPE_D3DCOLOR  =  4,  // 4D packed unsigned bytes mapped to 0. to 1. range
	                             // Input is in D3DCOLOR format (ARGB) expanded to (R, G, B, A)
	D3DDECLTYPE_UBYTE4    =  5,  // 4D unsigned byte
	D3DDECLTYPE_SHORT2    =  6,  // 2D signed short expanded to (value, value, 0., 1.)
	D3DDECLTYPE_SHORT4    =  7,  // 4D signed short

	D3DDECLTYPE_UBYTE4N   =  8,  // Each of 4 bytes is normalized by dividing to 255.0
	D3DDECLTYPE_SHORT2N   =  9,  // 2D signed short normalized (v[0]/32767.0,v[1]/32767.0,0,1)
	D3DDECLTYPE_SHORT4N   = 10,  // 4D signed short normalized (v[0]/32767.0,v[1]/32767.0,v[2]/32767.0,v[3]/32767.0)
	D3DDECLTYPE_USHORT2N  = 11,  // 2D unsigned short normalized (v[0]/65535.0,v[1]/65535.0,0,1)
	D3DDECLTYPE_USHORT4N  = 12,  // 4D unsigned short normalized (v[0]/65535.0,v[1]/65535.0,v[2]/65535.0,v[3]/65535.0)
	D3DDECLTYPE_UDEC3     = 13,  // 3D unsigned 10 10 10 format expanded to (value, value, value, 1)
	D3DDECLTYPE_DEC3N     = 14,  // 3D signed 10 10 10 format normalized and expanded to (v[0]/511.0, v[1]/511.0, v[2]/511.0, 1)
	D3DDECLTYPE_FLOAT16_2 = 15,  // Two 16-bit floating point values, expanded to (value, value, 0, 1)
	D3DDECLTYPE_FLOAT16_4 = 16,  // Four 16-bit floating point values
	D3DDECLTYPE_UNUSED    = 17,  // When the type field in a decl is unused.


	D3DDECLMETHOD_DEFAULT =  0,


	D3DDECLUSAGE_POSITION = 0,
	D3DDECLUSAGE_BLENDWEIGHT,   // 1
	D3DDECLUSAGE_BLENDINDICES,  // 2
	D3DDECLUSAGE_NORMAL,        // 3
	D3DDECLUSAGE_PSIZE,         // 4
	D3DDECLUSAGE_TEXCOORD,      // 5
	D3DDECLUSAGE_TANGENT,       // 6
	D3DDECLUSAGE_BINORMAL,      // 7
	D3DDECLUSAGE_TESSFACTOR,    // 8
	D3DDECLUSAGE_POSITIONT,     // 9
	D3DDECLUSAGE_COLOR,         // 10
	D3DDECLUSAGE_FOG,           // 11
	D3DDECLUSAGE_DEPTH,         // 12
	D3DDECLUSAGE_SAMPLE         // 13
	,

	D3DUSAGE_AUTOGENMIPMAP = 0x400
};
#endif

extern int vertFormatMap[];

void *createIndexBuffer(uint32 length, bool dynamic);
void destroyIndexBuffer(void *indexBuffer);
uint16 *lockIndices(void *indexBuffer, uint32 offset, uint32 size, uint32 flags);
void unlockIndices(void *indexBuffer);

void *createVertexBuffer(uint32 length, uint32 fvf, bool dynamic);
void destroyVertexBuffer(void *vertexBuffer);
uint8 *lockVertices(void *vertexBuffer, uint32 offset, uint32 size, uint32 flags);
void unlockVertices(void *vertexBuffer);

void *createTexture(int32 width, int32 height, int32 levels, uint32 usage, uint32 format);
void destroyTexture(void *texture);
uint8 *lockTexture(void *texture, int32 level);
void unlockTexture(void *texture, int32 level);

// Native Texture and Raster

struct D3dRaster
{
	void *texture;
	void *palette;
	void *lockedSurf;
	uint32 format;
	uint32 bpp;	// bytes per pixel
	bool hasAlpha;
	bool customFormat;
	bool autogenMipmap;
	// One of AlphaKind. ALPHAGRADED is the safe default: it is what every
	// raster with an alpha channel was treated as before this existed.
	uint8 alphaKind;
#if defined(RW_D3D11) || defined(RW_VULKAN)
	// Whether the system-memory copy has been written since it was last
	// uploaded. Both backends below keep the texels twice; see D3D11's.
	bool dirty;
#endif
#ifdef RW_D3D11
	// The GPU side. `texture` above stays the system-memory copy, because
	// D3D11 has nothing like D3D9's managed pool: a texture the GPU reads
	// cannot also hand out a pointer to lock. So the texels are kept twice,
	// and `dirty` says the copy has been written since it was last uploaded.
	void *tex11;	// ID3D11Texture2D
	void *srv;	// ID3D11ShaderResourceView
	void *rtv;	// ID3D11RenderTargetView, camera textures only
	void *dsv;	// ID3D11DepthStencilView, z buffers only
#endif
#ifdef RW_VULKAN
	// The GPU side, kept the way D3D11 keeps it. A pointer to the backend's
	// own record rather than the handles themselves: a Vulkan image is a
	// 64-bit handle even in a 32-bit build, and a void* cannot hold one.
	void *vk;
#endif
};

int32 getLevelSize(Raster *raster, int32 level);
void allocateDXT(Raster *raster, int32 dxt, int32 numLevels, bool32 hasAlpha);
void setRasterAlphaKind(Raster *raster, int32 kind);
void setPalette(Raster *raster, void *palette, int32 size);
void setTexels(Raster *raster, void *texels, int32 level);

extern int32 nativeRasterOffset;
void registerNativeRaster(void);
#define GETD3DRASTEREXT(raster) PLUGINOFFSET(rw::d3d::D3dRaster, raster, rw::d3d::nativeRasterOffset)

// Rendering

void setRenderState(uint32 state, uint32 value);
void getRenderState(uint32 state, uint32 *value);
void setTextureStageState(uint32 stage, uint32 type, uint32 value);
void getTextureStageState(uint32 stage, uint32 type, uint32 *value);
void setSamplerState(uint32 stage, uint32 type, uint32 value);
void getSamplerState(uint32 stage, uint32 type, uint32 *value);
void flushCache(void);

void setTexture(uint32 stage, Texture *tex);
// Object pipelines announce what the instanced geometry needs here; it is
// combined with the application's own VERTEXALPHA request, never replaces it.
void setPipelineVertexAlpha(bool32 enable);
// Whether the draw about to happen is blending: what the application asked for
// or what the pipeline asked for, whichever is on.
bool32 getBlendEnabled(void);
// Bracket a 2D primitive. Alpha to coverage is a statement about a surface's
// place in the depth buffer, and a screen-space quad has none.
void setIm2DActive(bool32 active);
void setMaterial(const RGBA &color, const SurfaceProperties &surfaceprops, float extraSurfProp = 0.0f);
// The same material, as a D3DMATERIAL9 for the fixed-function lighting stage.
// The shader one uploads constants nothing reads when there is no shader.
//
// The fixed-function path hands this WHITE and applies the material colour in
// a texture stage instead, because the shaders multiply by it after the
// lighting has been clamped and a D3DMATERIAL9 cannot say that.
namespace impl9 {
void setMaterial_fix(const RGBA &color, const SurfaceProperties &surfProps);
}
using impl9::setMaterial_fix;
inline void setMaterial(uint32 flags, const RGBA &color, const SurfaceProperties &surfaceprops, float extraSurfProp = 0.0f)
{
	static RGBA white = { 255, 255, 255, 255 };
	if(flags & Geometry::MODULATE)
		setMaterial(color, surfaceprops, extraSurfProp);
	else
		setMaterial(white, surfaceprops, extraSurfProp);
}

void setVertexShader(void *vs);
void setPixelShader(void *ps);
// The shader constant registers and the draw calls. Pipelines reach the device
// through these rather than through d3ddevice, so that a backend with no
// IDirect3DDevice9 behind it can carry the same pipeline code. Register
// numbering is the D3D9 constant file's: a register is four floats wide.
void setVertexShaderConstantF(uint32 reg, const float32 *data, int32 numRegs);
void setVertexShaderConstantI(uint32 reg, const int32 *data, int32 numRegs);
void setPixelShaderConstantF(uint32 reg, const float32 *data, int32 numRegs);
void drawPrimitive(uint32 primType, uint32 startVertex, uint32 numPrimitives);
void drawIndexedPrimitive(uint32 primType, int32 baseVertex, uint32 minVertex,
	uint32 numVertices, uint32 startIndex, uint32 numPrimitives);
void setIndices(void *indexBuffer);
void setStreamSource(int n, void *buffer, uint32 offset, uint32 stride);
void setVertexDeclaration(void *declaration);

void *createVertexShader(void *csosrc);
void *createPixelShader(void *csosrc);
void destroyVertexShader(void *shader);
void destroyPixelShader(void *shader);


/*
 * Vertex shaders and common pipeline stuff
 */

// This data will be available in vertex stream 2
struct VertexConstantData
{
	V3d normal;
	RGBA color;
	TexCoords texCoors[8];
};
extern void *constantVertexStream;
// The colour the constant vertex stream carries, for geometry drawn without a
// per-vertex colour. Zero -- black -- is the default and means lighting is the
// only thing that can brighten such a model. An application whose art expects
// an unlit vertex to start white rather than black sets this before the device
// is created.
extern bool32 constantVertexColorWhite;

// TODO: figure out why this even still exists...
struct D3dShaderState
{
	// for VS
	struct {
		float32 start;
		float32 end;
		float32 range;	// 1/(start-end)
		float32 disable;	// lower clamp
	} fogData, fogDisable;
	RGBA matColor;
	SurfaceProperties surfProps;
	float extraSurfProp;
	float lightOffset[3];
	int32 numDir, numPoint, numSpot;
	RGBAf ambient;
	// for PS
	RGBAf fogColor;

	bool fogDirty;
};
extern D3dShaderState d3dShaderState;

// Standard Vertex shader locations
enum
{
	VSLOC_combined	= 0,
	VSLOC_world	= 4,
	VSLOC_normal	= 8,
	VSLOC_matColor	= 12,
	VSLOC_surfProps	= 13,
	VSLOC_fogData	= 14,
	VSLOC_ambLight	= 15,
	VSLOC_lightOffset	= 16,
	VSLOC_lights	= 17,
	VSLOC_afterLights	= VSLOC_lights + 8*3,

	// Two rows of a 2x4 texture coordinate transform, for the pipeline
	// GetUVTransformPipeline() returns. Shares VSLOC_afterLights with the
	// skin pipeline's bone matrices and the matfx pipeline's texture matrix,
	// which is safe because each pipeline uploads its own before drawing.
	VSLOC_uvXform	= VSLOC_afterLights,

	VSLOC_numLights	= 0,

	PSLOC_fogColor = 0,

	// The per-pixel lighting path's copy of what the vertex shader is given
	// above. It starts at 8 and not at 1 because c1 upwards is scratch for
	// whoever is drawing -- d3d9matfx.cpp's shininess is c1, and the port's
	// glow and distortion passes take c1 to c3. PSLOC_ppMatColor and
	// PSLOC_ppSurfProps are uploaded only when the material changes, so a pass
	// that wrote over them would not be corrected until the next material
	// change. Keep this block clear of anything transient.
	//
	// ps_2_0 allows 32 float constants and this ends at c26. A ninth light
	// does not fit.
	PSLOC_ppMatColor = 8,
	PSLOC_ppSurfProps = 9,
	PSLOC_ppAmbient = 10,
	PSLOC_ppLightColor = 11,
	PSLOC_ppLightDirection = PSLOC_ppLightColor + 8
};

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

void lightingCB_Fix(Atomic *atomic);
// The same, for a primitive that has no atomic to enumerate lights against.
void lightingCB_Fix(void);
int32 lightingCB_Shader(Atomic *atomic);
int32 lightingCB_Shader(void);
// for VS
void uploadMatrices(void);	// no world transform
void uploadMatrices(Matrix *worldMat);
void setAmbient(const RGBAf &color);
void setNumLights(int numDir, int numPoint, int numSpot);
int32 uploadLights(WorldLights *lightData);	// called by lightingCB_Shader

extern void *im2dOverridePS;

extern void *default_amb_VS;
extern void *default_amb_dir_VS;
extern void *default_all_VS;
// The same three with a texture coordinate transform applied, built from
// default_VS.hlsl with UVXFORM defined. They pair with the same pixel shaders.
extern void *uvxform_amb_VS;
extern void *uvxform_amb_dir_VS;
extern void *uvxform_all_VS;
// The per-pixel lighting path: one vertex shader each rather than three,
// because these do no lighting to switch on. They must be paired with a _pp_
// pixel shader -- they emit a normal the others do not and leave the ambient
// term, the clamp and the material colour undone.
extern void *default_pp_VS;
extern void *uvxform_pp_VS;
extern void *default_PS;
extern void *default_tex_PS;
extern void *default_pp_PS;
extern void *default_tex_pp_PS;
extern void *im2d_VS;
extern void *im2d_PS;
extern void *im2d_tex_PS;
void createDefaultShaders(void);
void destroyDefaultShaders(void);

// ---------------------------------------------------------------------------
// The fixed-function pipeline
//
// Everything below is defined in d3d9ff.cpp and does nothing unless
// getFixedFunction(). What it reproduces is default_VS.hlsl and
// default_PS.hlsl: transform, vertex lighting, a UV transform, texture times
// vertex colour times material colour, and linear fog.

// D3DTS_WORLD, cached against the last matrix set. The overload with no
// argument sets the identity, for a primitive already in world space.
void ffSetWorldTransform(Matrix *worldMat);
void ffSetWorldTransform(void);
// D3DTS_VIEW, D3DTS_PROJECTION and the fog range, from the camera the scene
// was begun with. The shader path uploads the same numbers as constants.
void ffBeginUpdate(Camera *cam);

// D3DRS_LIGHTING and the lights themselves, per atomic. Returns whether
// lighting ended up on: an atomic that is not LIGHT-flagged, or that has no
// normals, is drawn with its vertex colours untouched, which is what the
// shaders do with it.
bool32 ffSetLighting(Atomic *atomic);
// Where the lighting stage takes each colour term from, per mesh. The prelight
// goes into the emissive term, and the diffuse term follows the vertices only
// when the mesh has vertex alpha to carry -- otherwise it is the material's
// white, so that a dynamic light is not tinted by the baked one.
void ffSetVertexColorSource(bool32 lighting, uint32 geoFlags, bool32 vertexAlpha);
// Stage 0 modulates `texture` into the vertex colour, stage 1 modulates
// `matColor` in through D3DRS_TEXTUREFACTOR. Stage 1 is disabled when that
// colour is opaque white, which is most materials and which leaves a DX7 part
// with both of its stages free.
void ffSetColorStages(Texture *texture, const RGBA &matColor);
// D3DTSS_TEXTURETRANSFORMFLAGS and D3DTS_TEXTURE0, from rw::uvTransform.
void ffSetUVTransform(bool32 enable);

// im2d. Pre-transformed vertices -- POSITIONT, and the camera-space z the
// shader path carries in w becomes the RHW D3D wants -- so the 2D path needs
// its own declaration and a copy loop rather than a memcpy.
void ffOpenIm2D(void);
void ffCloseIm2D(void);
void *ffIm2DDeclaration(void);
void ffCopyIm2DVertices(void *dst, const void *src, int32 numVertices);
void ffSetupIm2D(void);

// im3d. The declaration is already POSITION/NORMAL/COLOR/TEXCOORD and needs no
// fixed-function counterpart; only the state does.
void ffSetupIm3D(uint32 flags);
void ffSetupIm3DDraw(void);



// The same interface again, one namespace deeper, once per implementation.
//
// The comments are on the interface above; these are the same functions.
// d3ddispatch.cpp defines the interface by forwarding each one to whichever
// implementation is running, which is what lets a build carry both.
#ifdef RW_D3D9
namespace impl9 {
bool32 captureFrame(Raster *dst);
void *createPixelShader(void *csosrc);
void *createVertexShader(void *csosrc);
void destroyPixelShader(void *shader);
void destroyVertexShader(void *shader);
bool32 deviceOpen(void);
void drawIndexedPrimitive(uint32 primType, int32 baseVertex, uint32 minVertex,
	uint32 numVertices, uint32 startIndex, uint32 numPrimitives);
void drawPrimitive(uint32 primType, uint32 startVertex, uint32 numPrimitives);
void flushCache(void);
bool32 getBlendEnabled(void);
void getRenderState(uint32 state, uint32 *value);
void getSamplerState(uint32 stage, uint32 type, uint32 *value);
void getScreenExtent(int32 *width, int32 *height);
void getTextureStageState(uint32 stage, uint32 type, uint32 *value);
void getVirtualScreen(int32 *width, int32 *height);
int32 getVirtualScreenSamples(void);
void setIm2DActive(bool32 active);
void setIndices(void *indexBuffer);
void setMaterial(const RGBA &color, const SurfaceProperties &surfaceprops, float extraSurfProp);
void setPipelineVertexAlpha(bool32 enable);
void setPixelShader(void *ps);
void setPixelShaderConstantF(uint32 reg, const float32 *data, int32 numRegs);
void setRasterStage(uint32 stage, Raster *raster);
void setRenderState(uint32 state, uint32 value);
void setSamplerState(uint32 stage, uint32 type, uint32 value);
void setStreamSource(int n, void *buffer, uint32 offset, uint32 stride);
void setTexture(uint32 stage, Texture *tex);
void setTextureStageState(uint32 stage, uint32 type, uint32 value);
void setVertexDeclaration(void *declaration);
void setVertexShader(void *vs);
void setVertexShaderConstantF(uint32 reg, const float32 *data, int32 numRegs);
void setVertexShaderConstantI(uint32 reg, const int32 *data, int32 numRegs);
void setVirtualScreen(int32 width, int32 height);
void setVirtualScreenSamples(int32 samples);
extern Device renderdevice;
}
#endif
#ifdef RW_D3D11
namespace impl11 {
bool32 captureFrame(Raster *dst);
void *createPixelShader(void *csosrc);
void *createVertexShader(void *csosrc);
void destroyPixelShader(void *shader);
void destroyVertexShader(void *shader);
bool32 deviceOpen(void);
void drawIndexedPrimitive(uint32 primType, int32 baseVertex, uint32 minVertex,
	uint32 numVertices, uint32 startIndex, uint32 numPrimitives);
void drawPrimitive(uint32 primType, uint32 startVertex, uint32 numPrimitives);
void flushCache(void);
bool32 getBlendEnabled(void);
void getRenderState(uint32 state, uint32 *value);
void getSamplerState(uint32 stage, uint32 type, uint32 *value);
void getScreenExtent(int32 *width, int32 *height);
void getTextureStageState(uint32 stage, uint32 type, uint32 *value);
void getVirtualScreen(int32 *width, int32 *height);
int32 getVirtualScreenSamples(void);
void setIm2DActive(bool32 active);
void setIndices(void *indexBuffer);
void setMaterial(const RGBA &color, const SurfaceProperties &surfaceprops, float extraSurfProp);
void setPipelineVertexAlpha(bool32 enable);
void setPixelShader(void *ps);
void setPixelShaderConstantF(uint32 reg, const float32 *data, int32 numRegs);
void setRasterStage(uint32 stage, Raster *raster);
void setRenderState(uint32 state, uint32 value);
void setSamplerState(uint32 stage, uint32 type, uint32 value);
void setStreamSource(int n, void *buffer, uint32 offset, uint32 stride);
void setTexture(uint32 stage, Texture *tex);
void setTextureStageState(uint32 stage, uint32 type, uint32 value);
void setVertexDeclaration(void *declaration);
void setVertexShader(void *vs);
void setVertexShaderConstantF(uint32 reg, const float32 *data, int32 numRegs);
void setVertexShaderConstantI(uint32 reg, const int32 *data, int32 numRegs);
void setVirtualScreen(int32 width, int32 height);
void setVirtualScreenSamples(int32 samples);
extern Device renderdevice;
}
#endif

#ifdef RW_VULKAN
namespace implvk {
bool32 captureFrame(Raster *dst);
void *createPixelShader(void *csosrc);
void *createVertexShader(void *csosrc);
void destroyPixelShader(void *shader);
void destroyVertexShader(void *shader);
bool32 deviceOpen(void);
void drawIndexedPrimitive(uint32 primType, int32 baseVertex, uint32 minVertex,
	uint32 numVertices, uint32 startIndex, uint32 numPrimitives);
void drawPrimitive(uint32 primType, uint32 startVertex, uint32 numPrimitives);
void flushCache(void);
bool32 getBlendEnabled(void);
void getRenderState(uint32 state, uint32 *value);
void getSamplerState(uint32 stage, uint32 type, uint32 *value);
void getScreenExtent(int32 *width, int32 *height);
void getTextureStageState(uint32 stage, uint32 type, uint32 *value);
void getVirtualScreen(int32 *width, int32 *height);
int32 getVirtualScreenSamples(void);
void setIm2DActive(bool32 active);
void setIndices(void *indexBuffer);
void setMaterial(const RGBA &color, const SurfaceProperties &surfaceprops, float extraSurfProp);
void setPipelineVertexAlpha(bool32 enable);
void setPixelShader(void *ps);
void setPixelShaderConstantF(uint32 reg, const float32 *data, int32 numRegs);
void setRasterStage(uint32 stage, Raster *raster);
void setRenderState(uint32 state, uint32 value);
void setSamplerState(uint32 stage, uint32 type, uint32 value);
void setStreamSource(int n, void *buffer, uint32 offset, uint32 stride);
void setTexture(uint32 stage, Texture *tex);
void setTextureStageState(uint32 stage, uint32 type, uint32 value);
void setVertexDeclaration(void *declaration);
void setVertexShader(void *vs);
void setVertexShaderConstantF(uint32 reg, const float32 *data, int32 numRegs);
void setVertexShaderConstantI(uint32 reg, const int32 *data, int32 numRegs);
void setVirtualScreen(int32 width, int32 height);
void setVirtualScreenSamples(int32 samples);
extern Device renderdevice;

// Drawn over the finished frame at the window's resolution, after the virtual
// screen is scaled into the swap chain image and before it is presented. The
// callback returns whether it drew. nil turns it off. The GL3 device has the
// same hook.
typedef bool32 (*PresentOverlayFn)(int32 winWidth, int32 winHeight);
void setPresentOverlay(PresentOverlayFn fn);

// Only from inside the callback: alpha-blended triangles, each vertex twelve
// floats. x and y are window pixels with y down; then a position relative to
// the shape's centre, the shape's half width and height, its corner radius
// and outline width, all in pixels; then red, green, blue and alpha. The shape
// is a rounded box by its signed distance -- a circle when the corner reaches
// both half extents, filled when the outline width is 0.
void drawPresentOverlay(const float32 *vertices, int32 numVertices);
}
#endif

// Which implementation Engine::open should take, and the flags the forwarders
// read. Set them before Engine::open; nothing changes them afterwards. Neither
// set means D3D9, and useVulkan wins over useD3D11.
//
// Only a build carrying more than one implementation has them. In any other the
// answer is a constant.
#if (defined(RW_D3D9) + defined(RW_D3D11) + defined(RW_VULKAN)) > 1
#define RWD3D_MULTI
extern bool32 useD3D11;
extern bool32 useVulkan;
#endif
Device &renderDevice(void);

// The same question for code that is compiled ONCE for every implementation --
// the raster layer, the immediate mode, the pipelines. Constants in a build
// that carries only one, so the arm that cannot apply costs nothing and the arm
// that always applies is not a branch.
//
// The #ifdef around such an arm is still needed: only one of these has d3d9.h
// in scope, only one has d3d11.h, and only one has vulkan.h.
#ifdef RWD3D_MULTI
#define RWD3D_ISVK (rw::d3d::useVulkan)
#define RWD3D_IS11 (!rw::d3d::useVulkan && rw::d3d::useD3D11)
#define RWD3D_IS9 (!rw::d3d::useVulkan && !rw::d3d::useD3D11)
#else
#ifdef RW_D3D9
#define RWD3D_IS9 1
#else
#define RWD3D_IS9 0
#endif
#ifdef RW_D3D11
#define RWD3D_IS11 1
#else
#define RWD3D_IS11 0
#endif
#ifdef RW_VULKAN
#define RWD3D_ISVK 1
#else
#define RWD3D_ISVK 0
#endif
#endif

// A compiled shader by name, from the tree of the implementation that is
// running.
//
// shaders/make_shaders.cmd compiles every source into shaders/ for D3D9,
// shaders11/ for D3D11 and shadersvk/ for Vulkan under one array name. A file
// that creates shaders includes the first tree's headers inside a namespace
// sm2, the second's inside a namespace sm4 and the third's inside a namespace
// spv, so a shader missing from any tree does not compile in a build that
// carries it.
#ifdef RW_D3D9
#define RWD3D_SHADER9(name) ((void*)sm2::name)
#else
#define RWD3D_SHADER9(name) ((void*)0)
#endif
#ifdef RW_D3D11
#define RWD3D_SHADER11(name) ((void*)sm4::name)
#else
#define RWD3D_SHADER11(name) ((void*)0)
#endif
#ifdef RW_VULKAN
#define RWD3D_SHADERVK(name) ((void*)spv::name)
#else
#define RWD3D_SHADERVK(name) ((void*)0)
#endif
#define RWD3D_SHADER(name) \
	(RWD3D_ISVK ? RWD3D_SHADERVK(name) : RWD3D_IS11 ? RWD3D_SHADER11(name) : RWD3D_SHADER9(name))

}
}
