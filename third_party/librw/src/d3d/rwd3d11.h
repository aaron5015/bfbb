namespace rw {
namespace d3d {

#ifdef RW_D3D11

// D3D11's own half, declared where it is DEFINED. rw::d3d is the interface
// both Direct3D backends implement; see d3ddispatch.cpp.
namespace impl11 {

#ifdef __d3d11_h__

extern ID3D11Device *d3d11device;
extern ID3D11DeviceContext *d3d11context;

struct DisplayMode
{
	DXGI_MODE_DESC mode;
	uint32 flags;
};

struct D3d11Globals
{
	HWND window;

	IDXGIFactory1 *factory;
	int numAdapters;
	int adapter;
	D3D_FEATURE_LEVEL featureLevel;

	DisplayMode *modes;
	int numModes;
	int currentMode;
	DisplayMode startMode;

	uint32 msLevel;

	IDXGISwapChain *swapChain;
	// The back buffer's view, and the depth buffer created alongside it. Both
	// are remade when the window changes size.
	ID3D11RenderTargetView *backBufferTarget;
	ID3D11Texture2D *depthBuffer;
	ID3D11DepthStencilView *depthBufferView;
	int32 backBufferWidth;
	int32 backBufferHeight;

	int numTextures;
	int numVertexShaders;
	int numPixelShaders;
	int numVertexBuffers;
	int numIndexBuffers;
	int numVertexDeclarations;
};

extern D3d11Globals d3d11Globals;

DXGI_FORMAT formatToDXGI(uint32 format);
// The resource behind one of d3d11buffer.cpp's buffers.
ID3D11Buffer *bufferResource(void *buffer);
ID3D11VertexShader *vertexShaderResource(void *shader);
ID3D11InputLayout *inputLayoutFor(void *declaration, void *vertexShader);
// What the scene is drawn into: the virtual screen, or nil when there is none
// and the back buffer is the picture.
ID3D11Texture2D *virtualScreenTexture(void);

#endif

// The raster layer, in d3d11raster.cpp. The system-memory copy and the lock
// against it are d3d.cpp's non-D3D9 arms, unchanged; these are the GPU side.
Raster *rasterCreateCameraTexture(Raster *raster);
Raster *rasterCreateCamera(Raster *raster);
Raster *rasterCreateZbuffer(Raster *raster);
// Send the system copy to the GPU if it has been written since last time.
void rasterUpload(Raster *raster);
// The view to bind, uploading first if needed. nil when there is nothing to
// sample -- an unallocated raster, or a format with no D3D11 equivalent.
void *rasterShaderResource(Raster *raster);
uint8 *rasterLockTarget(Raster *raster, int32 level, int32 lockMode);
void rasterUnlockTarget(Raster *raster);
void rasterDestroy(Raster *raster, D3dRaster *natras);

// Vertex and index buffers, in d3d11buffer.cpp. A locked buffer hands out a
// pointer into a system copy; unlock is what sends it.
void *createVertexBuffer11(uint32 length, bool dynamic);
void destroyVertexBuffer11(void *buffer);
void *createIndexBuffer11(uint32 length, bool dynamic);
void destroyIndexBuffer11(void *buffer);
uint8 *lockBuffer11(void *buffer, uint32 offset, uint32 size);
void unlockBuffer11(void *buffer);

// The render state, in d3d11state.cpp. setRwRenderState and getRwRenderState
// are the Device's; the rest is what the backend's own files need.
void resetRenderState(void);
void invalidateDeviceState(void);
void unbindTextures(void);
// Drop a destroyed object from the device shadow. Without it the next object
// to be allocated at that address is taken for the one still bound.
void forgetRaster(Raster *raster);
void forgetVertexDeclaration(void *declaration);
void forgetBuffer(void *buffer);
void setRwRenderState(int32 state, void *pvalue);
void *getRwRenderState(int32 state);
void setRasterStage(uint32 stage, Raster *raster);
bool32 getIm2DActive(void);
void releaseStateObjects(void);
void createWhiteTexture(void);
void destroyWhiteTexture(void);

// Shader constants, shaders and input layouts, in d3d11shader.cpp.
void openShaderConstants(void);
void closeShaderConstants(void);
void uploadShaderConstants(void);
void setAlphaTestConstants(uint32 func, uint32 ref);
void releaseInputLayouts(void);
void forgetInputLayouts(void *declaration);
}

// Named into the interface, for the parts an application asks about: whether
// the device came up, and what it reports about itself.
#ifdef __d3d11_h__
using impl11::d3d11device;
using impl11::d3d11context;
using impl11::d3d11Globals;
#endif

#endif

}
}
