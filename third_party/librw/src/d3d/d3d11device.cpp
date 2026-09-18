#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define WITH_D3D
#include "../rwbase.h"
#include "../rwplg.h"
#include "../rwerror.h"
#include "../rwrender.h"
#include "../rwengine.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "rwd3d.h"
#include "rwd3d11.h"
#include "rwd3dimpl.h"

#define PLUGIN_ID 0

namespace rw {
namespace d3d {

// The two device implementations live one namespace deeper than the interface
// they implement, so that a build can carry both. rw::d3d is the interface --
// the raster layer, the immediate mode and the pipelines are written against it
// and are built once -- and d3ddispatch.cpp forwards each of its entry points
// to whichever of these is running.
//
// A call in here that passes one of rw::d3d's enumerators has to name this
// namespace: the enumerator's own namespace is rw::d3d, so argument-dependent
// lookup adds the forwarder to the candidates beside the function meant, and
// the two have the same signature.
namespace impl11 {

#ifdef RW_D3D11

D3d11Globals d3d11Globals;

ID3D11Device *d3d11device;
ID3D11DeviceContext *d3d11context;

// The attributes a shader reads that the geometry may not carry. Stream 2 holds
// one vertex of them and is bound with a stride of zero, so every vertex reads
// the same one -- which is what the pipelines' declarations point at when a
// geometry has no prelight, no texture coordinates or no normals.
void *constantVertexStream;

// The fixed-size screen. Its contract is in rwd3d.h.
//
// Without it a camera raster smaller than the window renders at its own size in
// the window's corner, because the viewport comes from the raster while the
// back buffer follows the client rect -- and worse here than on D3D9, because
// D3D11 refuses to pair a render target with a depth buffer of a different
// size, which is exactly what a 640x480 camera on a 3840x2160 window is.
//
// So every camera that renders to the frame buffer lands on a target of this
// size instead, and showRaster stretches it into the back buffer.
int32 virtualScreenWidth;
int32 virtualScreenHeight;

// What the scene is drawn into, and what anything that READS the frame gets.
// With no multisampling they are the same texture. With it they are two: the
// samples cannot be sampled, so they are collapsed into the single-sampled one
// on the way out -- see resolveVirtualScreen.
static ID3D11Texture2D *virtualScreen;
static ID3D11RenderTargetView *virtualScreenTarget;
static ID3D11ShaderResourceView *virtualScreenView;
static ID3D11Texture2D *virtualScreenMS;
static ID3D11RenderTargetView *virtualScreenMSTarget;
// Samples asked for, and the count actually granted.
static int32 virtualScreenSamples = 1;
static int32 virtualScreenGranted = 1;
// One depth buffer, at whichever sample count the scene target has: D3D11
// refuses to pair a multisampled target with a single-sampled depth buffer.
static ID3D11Texture2D *virtualScreenDepth;
static ID3D11DepthStencilView *virtualScreenDepthView;

// The target the scene lands on. The multisampled one when there is one.
static ID3D11RenderTargetView*
sceneTarget(void)
{
	return virtualScreenMSTarget ? virtualScreenMSTarget : virtualScreenTarget;
}
// The blit's own state: a fullscreen triangle needs no vertex buffer and no
// input layout, and it must not inherit the scene's blending or depth test.
static void *blitVS;
static void *blitPS;
static ID3D11BlendState *blitBlend;
static ID3D11DepthStencilState *blitDepth;
static ID3D11RasterizerState *blitRaster;
static ID3D11SamplerState *blitSampler;

static void forgetBindings(void);

// D3D11 dropped the triangle fan, and the game draws them: an NPC's light cone
// at zNPCSupport.cpp:598 and the robot's disco light at zNPCTypeRobot.cpp:2647.
//
// A fan of n vertices is the triangle list (0, i+1, i+2), and those indices
// depend on nothing but the vertex count -- so one buffer built at start-up
// covers every fan up to the largest the immediate-mode paths can hold.
#define MAXFANVERTICES 10000
static ID3D11Buffer *fanIndices;

static void
createFanIndices(void)
{
	uint16 *idx = rwNewT(uint16, (MAXFANVERTICES-2)*3, MEMDUR_FUNCTION | ID_DRIVER);
	for(int32 i = 0; i < MAXFANVERTICES-2; i++){
		idx[i*3+0] = 0;
		idx[i*3+1] = (uint16)(i+1);
		idx[i*3+2] = (uint16)(i+2);
	}

	D3D11_BUFFER_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.ByteWidth = (MAXFANVERTICES-2)*3*sizeof(uint16);
	desc.Usage = D3D11_USAGE_IMMUTABLE;
	desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
	D3D11_SUBRESOURCE_DATA init;
	memset(&init, 0, sizeof(init));
	init.pSysMem = idx;
	d3d11device->CreateBuffer(&desc, &init, &fanIndices);
	rwFree(idx);
}

// What OMSetRenderTargets was last given. Kept so a clear lands on the camera's
// own surfaces rather than on the window's.
static ID3D11RenderTargetView *currentTarget;
static ID3D11DepthStencilView *currentDepth;

void
getScreenExtent(int32 *width, int32 *height)
{
	if(virtualScreenWidth && virtualScreenHeight){
		*width = virtualScreenWidth;
		*height = virtualScreenHeight;
		return;
	}
	RECT rect;
	GetClientRect(d3d11Globals.window, &rect);
	*width = rect.right;
	*height = rect.bottom;
}

static void
releaseVirtualScreen(void)
{
	if(virtualScreenDepthView){ virtualScreenDepthView->Release(); virtualScreenDepthView = nil; }
	if(virtualScreenDepth){ virtualScreenDepth->Release(); virtualScreenDepth = nil; }
	if(virtualScreenMSTarget){ virtualScreenMSTarget->Release(); virtualScreenMSTarget = nil; }
	if(virtualScreenMS){ virtualScreenMS->Release(); virtualScreenMS = nil; }
	if(virtualScreenView){ virtualScreenView->Release(); virtualScreenView = nil; }
	if(virtualScreenTarget){ virtualScreenTarget->Release(); virtualScreenTarget = nil; }
	if(virtualScreen){ virtualScreen->Release(); virtualScreen = nil; }
	virtualScreenGranted = 1;
}

// Made when the device comes up, not when the size is set: the application
// picks the size before there is anything to make it with.
static void
acquireVirtualScreen(void)
{
	releaseVirtualScreen();
	if(virtualScreenWidth == 0 || virtualScreenHeight == 0 || d3d11device == nil)
		return;

	D3D11_TEXTURE2D_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.Width = virtualScreenWidth;
	desc.Height = virtualScreenHeight;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	if(FAILED(d3d11device->CreateTexture2D(&desc, nil, &virtualScreen)))
		return;
	d3d11device->CreateRenderTargetView(virtualScreen, nil, &virtualScreenTarget);
	d3d11device->CreateShaderResourceView(virtualScreen, nil, &virtualScreenView);

	// The multisampled target the scene draws into, when it was asked for and
	// the device will grant it. Falling back to one sample is not a failure:
	// the picture is the same one, drawn without the extra samples.
	//
	// D3D11 grants a count or it does not -- there is no rounding down -- so
	// the ask is walked down to the next power of two rather than refused
	// outright, the way 8x on a card that only does 4x would be.
	virtualScreenGranted = 1;
	for(int32 want = virtualScreenSamples; want > 1; want /= 2){
		UINT colorq, depthq;
		if(FAILED(d3d11device->CheckMultisampleQualityLevels(DXGI_FORMAT_B8G8R8A8_UNORM, want, &colorq)) ||
		   FAILED(d3d11device->CheckMultisampleQualityLevels(DXGI_FORMAT_D24_UNORM_S8_UINT, want, &depthq)) ||
		   colorq == 0 || depthq == 0)
			continue;

		desc.SampleDesc.Count = want;
		desc.SampleDesc.Quality = 0;
		// Nothing samples this one; the resolve destination is what is read.
		desc.BindFlags = D3D11_BIND_RENDER_TARGET;
		if(FAILED(d3d11device->CreateTexture2D(&desc, nil, &virtualScreenMS)))
			continue;
		if(FAILED(d3d11device->CreateRenderTargetView(virtualScreenMS, nil, &virtualScreenMSTarget))){
			virtualScreenMS->Release();
			virtualScreenMS = nil;
			continue;
		}
		virtualScreenGranted = want;
		break;
	}

	memset(&desc, 0, sizeof(desc));
	desc.Width = virtualScreenWidth;
	desc.Height = virtualScreenHeight;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	desc.SampleDesc.Count = virtualScreenGranted;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
	if(SUCCEEDED(d3d11device->CreateTexture2D(&desc, nil, &virtualScreenDepth)))
		d3d11device->CreateDepthStencilView(virtualScreenDepth, nil, &virtualScreenDepthView);
}

void
setVirtualScreen(int32 width, int32 height)
{
	if(virtualScreenWidth == width && virtualScreenHeight == height)
		return;
	virtualScreenWidth = width;
	virtualScreenHeight = height;
	acquireVirtualScreen();
}

// The single-sampled picture, for anything that needs to READ what has been
// drawn: the present-time blit, and the effects that take a copy of the frame
// to sample it as a texture.
//
// Not cached. The effects read the frame and then keep drawing into it, so a
// resolve held from earlier in the frame would hand back a stale picture.
ID3D11Texture2D*
virtualScreenTexture(void)
{
	if(virtualScreenMS && virtualScreen){
		// The effects read the frame in the middle of drawing it, so the
		// source of this resolve is the render target that is bound right
		// now. Unbound for the length of the call and put back after: a
		// resource that is an output and an input at once is a hazard the
		// runtime is entitled to answer by doing nothing at all.
		d3d11context->OMSetRenderTargets(0, nil, nil);
		d3d11context->ResolveSubresource(virtualScreen, 0, virtualScreenMS, 0,
			DXGI_FORMAT_B8G8R8A8_UNORM);
		d3d11context->OMSetRenderTargets(1, &currentTarget, currentDepth);
	}
	return virtualScreen;
}

void
getVirtualScreen(int32 *width, int32 *height)
{
	*width = virtualScreenWidth;
	*height = virtualScreenHeight;
}

void setVirtualScreenSamples(int32 samples) { virtualScreenSamples = samples < 1 ? 1 : samples; }
int32 getVirtualScreenSamples(void) { return virtualScreenGranted; }

// --- the swap chain ---------------------------------------------------------

static void
releaseBackBuffer(void)
{
	if(d3d11Globals.depthBufferView){
		d3d11Globals.depthBufferView->Release();
		d3d11Globals.depthBufferView = nil;
	}
	if(d3d11Globals.depthBuffer){
		d3d11Globals.depthBuffer->Release();
		d3d11Globals.depthBuffer = nil;
	}
	if(d3d11Globals.backBufferTarget){
		d3d11Globals.backBufferTarget->Release();
		d3d11Globals.backBufferTarget = nil;
	}
}

// Make the views over whatever the swap chain's buffers currently are. Called
// after creating the chain and again after every resize.
static bool32
acquireBackBuffer(void)
{
	ID3D11Texture2D *backBuffer = nil;
	if(FAILED(d3d11Globals.swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer)))
		return 0;

	D3D11_TEXTURE2D_DESC desc;
	backBuffer->GetDesc(&desc);
	d3d11Globals.backBufferWidth = desc.Width;
	d3d11Globals.backBufferHeight = desc.Height;

	HRESULT hr = d3d11device->CreateRenderTargetView(backBuffer, nil, &d3d11Globals.backBufferTarget);
	backBuffer->Release();
	if(FAILED(hr))
		return 0;

	D3D11_TEXTURE2D_DESC dd;
	memset(&dd, 0, sizeof(dd));
	dd.Width = desc.Width;
	dd.Height = desc.Height;
	dd.MipLevels = 1;
	dd.ArraySize = 1;
	dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	dd.SampleDesc = desc.SampleDesc;
	dd.Usage = D3D11_USAGE_DEFAULT;
	dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
	if(FAILED(d3d11device->CreateTexture2D(&dd, nil, &d3d11Globals.depthBuffer)))
		return 0;
	if(FAILED(d3d11device->CreateDepthStencilView(d3d11Globals.depthBuffer, nil, &d3d11Globals.depthBufferView)))
		return 0;
	return 1;
}

// Follow the window. The swap chain does not resize itself, and a chain whose
// buffers are a different size than the window is stretched by the presenter
// rather than redrawn -- which reads as a blurry frame, not as an error.
static void
resizeToWindow(void)
{
	RECT rect;
	GetClientRect(d3d11Globals.window, &rect);
	if(rect.right == 0 || rect.bottom == 0)
		return;
	if(rect.right == d3d11Globals.backBufferWidth &&
	   rect.bottom == d3d11Globals.backBufferHeight)
		return;

	d3d11context->OMSetRenderTargets(0, nil, nil);
	currentTarget = nil;
	currentDepth = nil;
	releaseBackBuffer();
	d3d11Globals.swapChain->ResizeBuffers(0, rect.right, rect.bottom, DXGI_FORMAT_UNKNOWN, 0);
	acquireBackBuffer();
}

// --- video modes ------------------------------------------------------------

static void
makeVideoModeList(void)
{
	IDXGIAdapter1 *adapter = nil;
	IDXGIOutput *output = nil;
	UINT num = 0;

	rwFree(d3d11Globals.modes);
	d3d11Globals.modes = nil;
	d3d11Globals.numModes = 0;

	if(FAILED(d3d11Globals.factory->EnumAdapters1(d3d11Globals.adapter, &adapter)))
		return;
	if(FAILED(adapter->EnumOutputs(0, &output))){
		adapter->Release();
		return;
	}

	// The windowed mode goes in first, as entry zero, so that an application
	// that never picks one gets a window rather than a display mode change.
	output->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, 0, &num, nil);
	d3d11Globals.modes = rwNewT(DisplayMode, num+1, MEMDUR_EVENT | ID_DRIVER);
	memset(d3d11Globals.modes, 0, sizeof(DisplayMode)*(num+1));

	RECT rect;
	GetClientRect(d3d11Globals.window, &rect);
	d3d11Globals.modes[0].mode.Width = rect.right;
	d3d11Globals.modes[0].mode.Height = rect.bottom;
	d3d11Globals.modes[0].mode.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	d3d11Globals.modes[0].flags = 0;

	if(num){
		DXGI_MODE_DESC *descs = rwNewT(DXGI_MODE_DESC, num, MEMDUR_FUNCTION | ID_DRIVER);
		output->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, 0, &num, descs);
		for(UINT i = 0; i < num; i++){
			d3d11Globals.modes[i+1].mode = descs[i];
			d3d11Globals.modes[i+1].flags = VIDEOMODEEXCLUSIVE;
		}
		rwFree(descs);
	}
	d3d11Globals.numModes = num+1;
	d3d11Globals.currentMode = 0;
	d3d11Globals.startMode = d3d11Globals.modes[0];

	output->Release();
	adapter->Release();
}

// --- open, start, stop, close -----------------------------------------------

static int
openD3D11(EngineOpenParams *params)
{
	d3d11Globals.window = params->window;
	d3d11Globals.adapter = 0;

	if(FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&d3d11Globals.factory))){
		RWERROR((ERR_GENERAL, "CreateDXGIFactory1 failed"));
		return 0;
	}

	IDXGIAdapter1 *adapter = nil;
	d3d11Globals.numAdapters = 0;
	while(d3d11Globals.factory->EnumAdapters1(d3d11Globals.numAdapters, &adapter) != DXGI_ERROR_NOT_FOUND){
		adapter->Release();
		d3d11Globals.numAdapters++;
	}
	makeVideoModeList();
	return 1;
}

static int
closeD3D11(void)
{
	rwFree(d3d11Globals.modes);
	d3d11Globals.modes = nil;
	d3d11Globals.numModes = 0;
	if(d3d11Globals.factory){
		d3d11Globals.factory->Release();
		d3d11Globals.factory = nil;
	}
	return 1;
}

static int
startD3D11(void)
{
	DXGI_SWAP_CHAIN_DESC scd;
	DisplayMode *mode = &d3d11Globals.modes[d3d11Globals.currentMode];

	memset(&scd, 0, sizeof(scd));
	scd.BufferCount = 2;
	scd.BufferDesc = mode->mode;
	scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	scd.OutputWindow = d3d11Globals.window;
	scd.SampleDesc.Count = 1;
	scd.SampleDesc.Quality = 0;
	scd.Windowed = !(mode->flags & VIDEOMODEEXCLUSIVE);
	scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	UINT flags = 0;
#ifdef DEBUG
	flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
	static const D3D_FEATURE_LEVEL levels[] = {
		D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1,
		D3D_FEATURE_LEVEL_10_0,
	};

	HRESULT hr = D3D11CreateDeviceAndSwapChain(nil, D3D_DRIVER_TYPE_HARDWARE, nil, flags,
		levels, sizeof(levels)/sizeof(levels[0]), D3D11_SDK_VERSION,
		&scd, &d3d11Globals.swapChain, &d3d11device, &d3d11Globals.featureLevel, &d3d11context);
	if(FAILED(hr)){
		RWERROR((ERR_GENERAL, "D3D11CreateDeviceAndSwapChain failed"));
		return 0;
	}

	// The presenter's own alt-enter would swap the chain to a display mode
	// behind the engine's back. Video modes are the engine's to set.
	d3d11Globals.factory->MakeWindowAssociation(d3d11Globals.window, DXGI_MWA_NO_ALT_ENTER);

	if(!acquireBackBuffer()){
		RWERROR((ERR_GENERAL, "could not view the back buffer"));
		return 0;
	}
	return 1;
}

static int
stopD3D11(void)
{
	releaseBackBuffer();
	currentTarget = nil;
	currentDepth = nil;
	if(d3d11Globals.swapChain){
		d3d11Globals.swapChain->SetFullscreenState(FALSE, nil);
		d3d11Globals.swapChain->Release();
		d3d11Globals.swapChain = nil;
	}
	if(d3d11context){
		d3d11context->ClearState();
		d3d11context->Release();
		d3d11context = nil;
	}
	if(d3d11device){
		d3d11device->Release();
		d3d11device = nil;
	}
	return 1;
}

static int
initD3D11(void)
{
	// D3D11 has no paletted texture format at all, so librw expands a palette
	// into true colour before a raster is created rather than after.
	isP8supported = 0;

	forgetBindings();
	resetRenderState();
	openShaderConstants();
	acquireVirtualScreen();
	createFanIndices();
	createWhiteTexture();

	{
		static
#include "shaders11/blit_VS.h"
		blitVS = createVertexShader((void*)blit_VS);
	}
	{
		static
#include "shaders11/blit_PS.h"
		blitPS = createPixelShader((void*)blit_PS);
	}

	D3D11_BLEND_DESC bd;
	memset(&bd, 0, sizeof(bd));
	bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	d3d11device->CreateBlendState(&bd, &blitBlend);

	D3D11_DEPTH_STENCIL_DESC dsd;
	memset(&dsd, 0, sizeof(dsd));
	d3d11device->CreateDepthStencilState(&dsd, &blitDepth);

	D3D11_RASTERIZER_DESC rd;
	memset(&rd, 0, sizeof(rd));
	rd.FillMode = D3D11_FILL_SOLID;
	rd.CullMode = D3D11_CULL_NONE;
	rd.DepthClipEnable = TRUE;
	d3d11device->CreateRasterizerState(&rd, &blitRaster);

	D3D11_SAMPLER_DESC sd;
	memset(&sd, 0, sizeof(sd));
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	d3d11device->CreateSamplerState(&sd, &blitSampler);

	VertexConstantData constants;
	memset(&constants, 0, sizeof(constants));
	uint8 base = constantVertexColorWhite ? 255 : 0;
	constants.color.red = base;
	constants.color.green = base;
	constants.color.blue = base;
	constants.color.alpha = 255;
	constantVertexStream = createVertexBuffer(sizeof(constants), 0, false);
	if(constantVertexStream){
		uint8 *locked = lockVertices(constantVertexStream, 0, sizeof(constants), 0);
		memcpy(locked, &constants, sizeof(constants));
		unlockVertices(constantVertexStream);
		setStreamSource(2, constantVertexStream, 0, 0);
	}

	openIm2D();
	openIm3D();
	return 1;
}
static int
termD3D11(void)
{
	closeIm3D();
	closeIm2D();
	destroyVertexBuffer(constantVertexStream);
	constantVertexStream = nil;
	destroyVertexShader(blitVS);
	blitVS = nil;
	destroyPixelShader(blitPS);
	blitPS = nil;
	if(blitBlend){ blitBlend->Release(); blitBlend = nil; }
	if(blitDepth){ blitDepth->Release(); blitDepth = nil; }
	if(blitRaster){ blitRaster->Release(); blitRaster = nil; }
	if(blitSampler){ blitSampler->Release(); blitSampler = nil; }
	if(fanIndices){ fanIndices->Release(); fanIndices = nil; }
	destroyWhiteTexture();
	releaseVirtualScreen();
	closeShaderConstants();
	releaseInputLayouts();
	releaseStateObjects();
	forgetBindings();
	return stopD3D11();
}
static int finalizeD3D11(void) { return 1; }

// --- the camera -------------------------------------------------------------

bool32
deviceOpen(void)
{
	return d3d11device != nil;
}

// The frame so far, into a texture. Where D3D9 has to stretch between two
// surfaces this is a straight resource copy: the virtual screen and a camera
// texture the caller sized from getScreenExtent agree on both.
bool32
captureFrame(Raster *dst)
{
	if(d3d11device == nil || dst == nil)
		return 0;

	D3dRaster *natras = GETD3DRASTEREXT(dst);
	ID3D11Texture2D *tex = (ID3D11Texture2D*)natras->tex11;
	if(tex == nil)
		return 0;

	ID3D11Texture2D *src = virtualScreenTexture();
	bool32 borrowed = 0;
	if(src == nil){
		if(FAILED(d3d11Globals.swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&src)))
			return 0;
		borrowed = 1;
	}

	D3D11_TEXTURE2D_DESC sd, dd;
	src->GetDesc(&sd);
	tex->GetDesc(&dd);
	bool32 ok = 0;
	if(sd.Width == dd.Width && sd.Height == dd.Height && sd.Format == dd.Format){
		d3d11context->CopyResource(tex, src);
		ok = 1;
	}
	if(borrowed)
		src->Release();
	return ok;
}

static void
setRenderSurfaces(Camera *cam)
{
	ID3D11RenderTargetView *rtv;
	ID3D11DepthStencilView *dsv;
	int32 width, height;

	if(virtualScreenTarget){
		rtv = sceneTarget();
		dsv = virtualScreenDepthView;
		width = virtualScreenWidth;
		height = virtualScreenHeight;
	}else{
		rtv = d3d11Globals.backBufferTarget;
		dsv = d3d11Globals.depthBufferView;
		width = d3d11Globals.backBufferWidth;
		height = d3d11Globals.backBufferHeight;
	}

	// A camera texture brings its own pair, and they are the same size as each
	// other by construction. The frame buffer's own z buffer is ignored when
	// the virtual screen is standing in: D3D11 will not pair views of
	// different sizes, and the game's z buffer raster follows the window
	// rather than the picture.
	if(cam->frameBuffer){
		D3dRaster *natras = GETD3DRASTEREXT(cam->frameBuffer);
		if(natras->rtv){
			rtv = (ID3D11RenderTargetView*)natras->rtv;
			width = cam->frameBuffer->width;
			height = cam->frameBuffer->height;
			dsv = nil;
			if(cam->zBuffer){
				D3dRaster *z = GETD3DRASTEREXT(cam->zBuffer);
				if(z->dsv && cam->zBuffer->width == cam->frameBuffer->width &&
				   cam->zBuffer->height == cam->frameBuffer->height)
					dsv = (ID3D11DepthStencilView*)z->dsv;
			}
		}
	}

	unbindTextures();
	currentTarget = rtv;
	currentDepth = dsv;
	d3d11context->OMSetRenderTargets(1, &rtv, dsv);

	D3D11_VIEWPORT vp;
	vp.TopLeftX = 0.0f;
	vp.TopLeftY = 0.0f;
	vp.Width = (float)width;
	vp.Height = (float)height;
	vp.MinDepth = 0.0f;
	vp.MaxDepth = 1.0f;
	d3d11context->RSSetViewports(1, &vp);
}

// The same view and projection as the D3D9 backend builds. D3D11 keeps D3D9's
// clip space -- left handed, z in [0,1] -- so the matrices are unchanged.
static void
beginUpdate(Camera *cam)
{
	float view[16], proj[16];

	Matrix inv;
	Matrix::invert(&inv, cam->getFrame()->getLTM());
	view[0]  = -inv.right.x;
	view[1]  =  inv.right.y;
	view[2]  =  inv.right.z;
	view[3]  =  0.0f;
	view[4]  = -inv.up.x;
	view[5]  =  inv.up.y;
	view[6]  =  inv.up.z;
	view[7]  =  0.0f;
	view[8]  = -inv.at.x;
	view[9]  =  inv.at.y;
	view[10] =  inv.at.z;
	view[11] =  0.0f;
	view[12] = -inv.pos.x;
	view[13] =  inv.pos.y;
	view[14] =  inv.pos.z;
	view[15] =  1.0f;
	memcpy(&cam->devView, view, sizeof(RawMatrix));

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
		proj[10] = cam->farPlane*invz;
		proj[11] = 1.0f;
		proj[15] = 0.0f;
	}else{
		proj[10] = invz;
		proj[11] = 0.0f;
		proj[15] = 1.0f;
	}
	proj[14] = -cam->nearPlane*proj[10];
	memcpy(&cam->devProj, proj, sizeof(RawMatrix));

	// The fog range the vertex shader turns into a per-vertex factor. Not
	// optional: the pixel shader lerps towards the fog colour by it, so a
	// range left at zero fogs every pixel completely and the picture comes out
	// the fog colour, which is black until something sets one.
	d3dShaderState.fogData.start = cam->fogPlane;
	d3dShaderState.fogData.end = cam->farPlane;
	d3dShaderState.fogData.range = 1.0f/(cam->fogPlane - cam->farPlane);
	d3dShaderState.fogData.disable = getRwRenderState(FOGENABLE) ? 0.0f : 1.0f;
	d3dShaderState.fogDisable.start = 0.0f;
	d3dShaderState.fogDisable.end = 0.0f;
	d3dShaderState.fogDisable.range = 0.0f;
	d3dShaderState.fogDisable.disable = 1.0f;
	d3dShaderState.fogDirty = true;

	// The swap chain does not follow the window on its own, and the letterbox
	// is measured against the back buffer -- so a resized window would be
	// stretched from the old size until something else remade the chain.
	resizeToWindow();

	setRenderSurfaces(cam);
}

static void
endUpdate(Camera *cam)
{
}

static void
clearCamera(Camera *cam, RGBA *col, uint32 mode)
{
	// Not only for beginUpdate's sake: a clear before the scene opens is how
	// the game wipes a camera texture it is about to render into.
	setRenderSurfaces(cam);
	if(currentTarget == nil)
		return;

	if(mode & Camera::CLEARIMAGE){
		float c[4];
		c[0] = col->red/255.0f;
		c[1] = col->green/255.0f;
		c[2] = col->blue/255.0f;
		c[3] = col->alpha/255.0f;
		d3d11context->ClearRenderTargetView(currentTarget, c);
	}
	uint32 depthFlags = 0;
	if(mode & Camera::CLEARZ)
		depthFlags |= D3D11_CLEAR_DEPTH;
	if(mode & Camera::CLEARSTENCIL)
		depthFlags |= D3D11_CLEAR_STENCIL;
	if(depthFlags && currentDepth)
		d3d11context->ClearDepthStencilView(currentDepth, depthFlags, 1.0f, 0);
}

// Stretch the virtual screen into the back buffer, keeping its aspect ratio.
//
// The destination is the largest rectangle of the virtual screen's shape that
// fits the window, centred, and the rest is cleared to black. Clearing every
// frame rather than only when the window changes costs one fill of a surface
// about to be overwritten anyway, and means nothing has to track when the bars
// last moved.
static void
blitVirtualScreen(void)
{
	if(virtualScreenView == nil || d3d11Globals.backBufferTarget == nil)
		return;

	// The samples are collapsed BEFORE the render target changes: a resolve is
	// a copy between resources and does not care what is bound, but reading the
	// destination as a texture in the same call would.
	virtualScreenTexture();

	float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	d3d11context->OMSetRenderTargets(1, &d3d11Globals.backBufferTarget, nil);
	d3d11context->ClearRenderTargetView(d3d11Globals.backBufferTarget, black);

	float scale = d3d11Globals.backBufferWidth/(float)virtualScreenWidth;
	float other = d3d11Globals.backBufferHeight/(float)virtualScreenHeight;
	if(other < scale)
		scale = other;

	D3D11_VIEWPORT vp;
	vp.Width = virtualScreenWidth*scale;
	vp.Height = virtualScreenHeight*scale;
	vp.TopLeftX = (d3d11Globals.backBufferWidth - vp.Width)/2.0f;
	vp.TopLeftY = (d3d11Globals.backBufferHeight - vp.Height)/2.0f;
	vp.MinDepth = 0.0f;
	vp.MaxDepth = 1.0f;
	d3d11context->RSSetViewports(1, &vp);

	float blendFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	d3d11context->OMSetBlendState(blitBlend, blendFactor, 0xFFFFFFFF);
	d3d11context->OMSetDepthStencilState(blitDepth, 0);
	d3d11context->RSSetState(blitRaster);
	d3d11context->IASetInputLayout(nil);
	d3d11context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	d3d11context->VSSetShader(vertexShaderResource(blitVS), nil, 0);
	d3d11context->PSSetShader((ID3D11PixelShader*)blitPS, nil, 0);
	d3d11context->PSSetShaderResources(0, 1, &virtualScreenView);
	d3d11context->PSSetSamplers(0, 1, &blitSampler);
	d3d11context->Draw(3, 0);

	// Nothing the scene binds survives this, so say so rather than let the
	// next frame draw against a shadow that no longer matches the context.
	ID3D11ShaderResourceView *none = nil;
	d3d11context->PSSetShaderResources(0, 1, &none);
	forgetBindings();
	invalidateDeviceState();
}

// One place where D3D11 is simply better: the presentation interval is an
// argument to Present, so asking for vsync between one frame and the next costs
// nothing. Under D3D9 it lives in the present parameters and changing it resets
// the device.
// A device that has gone away -- a driver reset, a driver update, a GPU pulled
// out from under the process. Everything made from it is gone with it, and
// D3D11 does not report that on the calls that would have used it: they return
// success and draw nothing. So it is checked here, where Present does report
// it, and said out loud once.
//
// Recovery is not implemented. It would mean a second device and every
// resource made again from the system copies the driver already keeps, and a
// hook for the application to hand back what it built. Reporting it is what
// keeps a dead device from reading as a frozen picture.
static bool32 deviceGone;

static void
checkDeviceGone(HRESULT hr)
{
	if(deviceGone || (hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET))
		return;
	deviceGone = 1;
	HRESULT reason = d3d11device->GetDeviceRemovedReason();
	fprintf(stderr, "librw: the D3D11 device is gone (0x%08lx, reason 0x%08lx); "
		"nothing will be drawn from here on\n",
		(unsigned long)hr, (unsigned long)reason);
	fflush(stderr);
}

static void
showRaster(Raster *raster, uint32 flags)
{
	if(deviceGone)
		return;
	blitVirtualScreen();
	checkDeviceGone(d3d11Globals.swapChain->Present(flags & Raster::FLIPWAITVSYNCH ? 1 : 0, 0));
}

// The frame into a camera texture at an offset, the one case the D3D9 backend
// answers with StretchRect. Here it is a resource copy: the two agree on format
// and neither is multisampled once the virtual screen has been resolved.
static bool32
rasterRenderFast(Raster *raster, int32 x, int32 y)
{
	Raster *dst = Raster::getCurrentContext();
	if(dst == nil || dst->type != Raster::CAMERATEXTURE ||
	   raster->type != Raster::CAMERA)
		return 0;

	ID3D11Texture2D *dsttex = (ID3D11Texture2D*)GETD3DRASTEREXT(dst)->tex11;
	ID3D11Texture2D *src = virtualScreenTexture();
	if(dsttex == nil || src == nil)
		return 0;

	D3D11_TEXTURE2D_DESC sd, dd;
	src->GetDesc(&sd);
	dsttex->GetDesc(&dd);
	if(sd.Format != dd.Format)
		return 0;

	// CopySubresourceRegion refuses a box that leaves the source, and both
	// ends of the copy start at the same corner.
	D3D11_BOX box;
	box.left = x < 0 ? 0 : x;
	box.top = y < 0 ? 0 : y;
	box.right = sd.Width;
	box.bottom = sd.Height;
	box.front = 0;
	box.back = 1;
	if(box.right > dd.Width) box.right = dd.Width;
	if(box.bottom > dd.Height) box.bottom = dd.Height;
	if(box.left >= box.right || box.top >= box.bottom)
		return 0;

	d3d11context->CopySubresourceRegion(dsttex, 0, box.left, box.top, 0,
		src, 0, &box);
	return 1;
}

// --- what is bound to draw with ---------------------------------------------

// The input assembler and the shader stages. An input layout depends on both
// the declaration and the vertex shader, so the two are remembered and the
// layout is settled at draw time rather than when either is set.
static struct {
	void *vertexShader;
	void *pixelShader;
	void *declaration;
	void *indexBuffer;
	struct {
		void *buffer;
		uint32 offset;
		uint32 stride;
	} streams[3];
	ID3D11InputLayout *layout;
	uint32 primType;
} bound;

void
forgetVertexDeclaration(void *declaration)
{
	forgetInputLayouts(declaration);
	if(bound.declaration == declaration){
		bound.declaration = nil;
		bound.layout = nil;
	}
}

void
forgetBuffer(void *buffer)
{
	if(bound.indexBuffer == buffer)
		bound.indexBuffer = nil;
	for(int i = 0; i < 3; i++)
		if(bound.streams[i].buffer == buffer)
			bound.streams[i].buffer = nil;
}

void
setVertexShader(void *vs)
{
	if(bound.vertexShader != vs){
		bound.vertexShader = vs;
		bound.layout = nil;
		d3d11context->VSSetShader(vertexShaderResource(vs), nil, 0);
	}
}

void
setPixelShader(void *ps)
{
	if(bound.pixelShader != ps){
		bound.pixelShader = ps;
		d3d11context->PSSetShader((ID3D11PixelShader*)ps, nil, 0);
	}
}

void
setVertexDeclaration(void *declaration)
{
	if(bound.declaration != declaration){
		bound.declaration = declaration;
		bound.layout = nil;
	}
}

void
setIndices(void *indexBuffer)
{
	if(bound.indexBuffer != indexBuffer){
		bound.indexBuffer = indexBuffer;
		d3d11context->IASetIndexBuffer(bufferResource(indexBuffer), DXGI_FORMAT_R16_UINT, 0);
	}
}

void
setStreamSource(int n, void *buffer, uint32 offset, uint32 stride)
{
	if(n < 0 || n > 2)
		return;
	if(bound.streams[n].buffer == buffer &&
	   bound.streams[n].offset == offset &&
	   bound.streams[n].stride == stride)
		return;
	bound.streams[n].buffer = buffer;
	bound.streams[n].offset = offset;
	bound.streams[n].stride = stride;

	ID3D11Buffer *buf = bufferResource(buffer);
	UINT strides = stride, offsets = offset;
	d3d11context->IASetVertexBuffers(n, 1, &buf, &strides, &offsets);
}

// D3D9 counts primitives, D3D11 counts vertices, and the two disagree by the
// primitive's shape rather than by a factor.
static uint32
indexCount(uint32 primType, uint32 numPrimitives)
{
	switch(primType){
	case D3DPT_LINELIST:		return numPrimitives*2;
	case D3DPT_LINESTRIP:		return numPrimitives+1;
	case D3DPT_TRIANGLELIST:	return numPrimitives*3;
	case D3DPT_TRIANGLESTRIP:	return numPrimitives+2;
	case D3DPT_POINTLIST:		return numPrimitives;
	}
	return 0;
}

static D3D11_PRIMITIVE_TOPOLOGY
topology(uint32 primType)
{
	switch(primType){
	case D3DPT_POINTLIST:		return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
	case D3DPT_LINELIST:		return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
	case D3DPT_LINESTRIP:		return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
	case D3DPT_TRIANGLELIST:	return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	case D3DPT_TRIANGLESTRIP:	return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
	}
	// The fan has no topology here; drawPrimitive turns it into a list
	// instead. Anything else is a primitive type this driver never emits.
	return D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
}

// Settle what depends on more than one binding: the input layout on the
// declaration and the vertex shader, the topology on the draw.
static bool32
readyToDraw(uint32 primType)
{
	D3D11_PRIMITIVE_TOPOLOGY topo = topology(primType);
	if(topo == D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED)
		return 0;
	if(bound.primType != primType){
		bound.primType = primType;
		d3d11context->IASetPrimitiveTopology(topo);
	}
	if(bound.layout == nil){
		bound.layout = inputLayoutFor(bound.declaration, bound.vertexShader);
		if(bound.layout == nil)
			return 0;
		d3d11context->IASetInputLayout(bound.layout);
	}
	return 1;
}

// A fan, as the triangle list D3D11 will take. The index buffer it borrows is
// not the one the caller had bound, so that binding is put back.
static void
drawFan(uint32 numPrimitives)
{
	if(fanIndices == nil || numPrimitives > MAXFANVERTICES-2)
		return;
	if(!readyToDraw(D3DPT_TRIANGLELIST))
		return;
	d3d11context->IASetIndexBuffer(fanIndices, DXGI_FORMAT_R16_UINT, 0);
	d3d11context->DrawIndexed(numPrimitives*3, 0, 0);
	d3d11context->IASetIndexBuffer(bufferResource(bound.indexBuffer), DXGI_FORMAT_R16_UINT, 0);
}

void
drawPrimitive(uint32 primType, uint32 startVertex, uint32 numPrimitives)
{
	if(numPrimitives == 0)
		return;
	if(primType == D3DPT_TRIANGLEFAN){
		drawFan(numPrimitives);
		return;
	}
	if(!readyToDraw(primType))
		return;
	d3d11context->Draw(indexCount(primType, numPrimitives), startVertex);
}

void
drawIndexedPrimitive(uint32 primType, int32 baseVertex, uint32 minVertex,
	uint32 numVertices, uint32 startIndex, uint32 numPrimitives)
{
	(void)minVertex;
	(void)numVertices;
	if(numPrimitives == 0 || !readyToDraw(primType))
		return;
	d3d11context->DrawIndexed(indexCount(primType, numPrimitives), startIndex, baseVertex);
}

// Nothing the context holds survives a device teardown, and the shadow above
// would otherwise claim it did.
static void
forgetBindings(void)
{
	memset(&bound, 0, sizeof(bound));
	bound.primType = 0xFFFFFFFF;
}

// --- the device interface ---------------------------------------------------

static int
deviceSystem(DeviceReq req, void *arg, int32 n)
{
	VideoMode *rwmode;
	IDXGIAdapter1 *adapter;
	DXGI_ADAPTER_DESC1 desc;

	switch(req){
	case DEVICEOPEN:
		return openD3D11((EngineOpenParams*)arg);
	case DEVICECLOSE:
		return closeD3D11();

	case DEVICEINIT:
		return startD3D11() && initD3D11();
	case DEVICETERM:
		return termD3D11();

	case DEVICEFINALIZE:
		return finalizeD3D11();

	case DEVICEGETNUMSUBSYSTEMS:
		return d3d11Globals.numAdapters;

	case DEVICEGETCURRENTSUBSYSTEM:
		return d3d11Globals.adapter;

	case DEVICESETSUBSYSTEM:
		if(n >= d3d11Globals.numAdapters)
			return 0;
		d3d11Globals.adapter = n;
		makeVideoModeList();
		return 1;

	case DEVICEGETSUBSSYSTEMINFO:
		if(n >= d3d11Globals.numAdapters)
			return 0;
		if(FAILED(d3d11Globals.factory->EnumAdapters1(n, &adapter)))
			return 0;
		adapter->GetDesc1(&desc);
		adapter->Release();
		WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
			((SubSystemInfo*)arg)->name, sizeof(SubSystemInfo::name), nil, nil);
		return 1;

	case DEVICEGETNUMVIDEOMODES:
		return d3d11Globals.numModes;

	case DEVICEGETCURRENTVIDEOMODE:
		return d3d11Globals.currentMode;

	case DEVICESETVIDEOMODE:
		if(n >= d3d11Globals.numModes)
			return 0;
		d3d11Globals.currentMode = n;
		return 1;

	case DEVICEGETVIDEOMODEINFO:
		rwmode = (VideoMode*)arg;
		rwmode->width = d3d11Globals.modes[n].mode.Width;
		rwmode->height = d3d11Globals.modes[n].mode.Height;
		rwmode->depth = 32;
		rwmode->flags = d3d11Globals.modes[n].flags;
		return 1;

	case DEVICEGETMAXMULTISAMPLINGLEVELS:
		return 1;
	case DEVICEGETMULTISAMPLINGLEVELS:
		return d3d11Globals.msLevel == 0 ? 1 : d3d11Globals.msLevel;
	case DEVICESETMULTISAMPLINGLEVELS:
		d3d11Globals.msLevel = (uint32)n;
		return 1;
	}
	return 1;
}

Device renderdevice = {
	0.0f, 1.0f,
	beginUpdate,
	endUpdate,
	clearCamera,
	showRaster,
	rasterRenderFast,
	setRwRenderState,
	getRwRenderState,
	im2DRenderLine,
	im2DRenderTriangle,
	im2DRenderPrimitive,
	im2DRenderIndexedPrimitive,
	im3DTransform,
	im3DRenderPrimitive,
	im3DRenderIndexedPrimitive,
	im3DEnd,
	deviceSystem,
};

#endif
}
}
}
