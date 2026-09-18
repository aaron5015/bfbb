namespace rw {
namespace d3d {

#ifdef RW_VULKAN

// Vulkan's own half, declared where it is DEFINED. rw::d3d is the interface
// every device implementation answers to; see d3ddispatch.cpp.
//
// The declarations outside the VOLK_H_ block are what the shared layer calls
// and need no Vulkan headers. The ones inside are the backend's own.
namespace implvk {

// The largest anisotropy a sampler may ask for; 1 when the device has none.
int32 maxAnisotropy(void);

// Vertex and index buffers, in vkresource.cpp. As D3D11's: a lock hands out a
// pointer into a system copy, and unlock is what sends it.
void *createVertexBufferVk(uint32 length, bool dynamic);
void destroyVertexBufferVk(void *buffer);
void *createIndexBufferVk(uint32 length, bool dynamic);
void destroyIndexBufferVk(void *buffer);
uint8 *lockBufferVk(void *buffer, uint32 offset, uint32 size);
void unlockBufferVk(void *buffer);

// The raster layer, in vkresource.cpp. The system copy and the lock against it
// are d3d.cpp's non-D3D9 arms, unchanged; these are the GPU side.
Raster *rasterCreateCameraTexture(Raster *raster);
Raster *rasterCreateCamera(Raster *raster);
Raster *rasterCreateZbuffer(Raster *raster);
uint8 *rasterLockTarget(Raster *raster, int32 level, int32 lockMode);
void rasterUnlockTarget(Raster *raster);
void rasterDestroy(Raster *raster, D3dRaster *natras);

// A declaration on its way out, in vkpipeline.cpp.
void forgetVertexDeclaration(void *declaration);

#ifdef VOLK_H_

struct VkGlobals
{
	::SDL_Window *window;

	VkInstance instance;
	VkDebugUtilsMessengerEXT messenger;
	VkSurfaceKHR surface;

	VkPhysicalDevice *adapters;
	int32 numAdapters;
	int32 adapter;

	VkPhysicalDevice physicalDevice;
	VkPhysicalDeviceProperties properties;
	VkDevice device;
	uint32 queueFamily;
	VkQueue queue;
	VmaAllocator allocator;
	VkPipelineCache pipelineCache;

	// The depth format every depth buffer is made in. D24S8 where the device
	// has it; AMD's desktop drivers do not, and take D32S8 instead.
	VkFormat depthFormat;
	// Whether B8G8R8A8_UNORM works as a vertex attribute, which is how a
	// D3DCOLOR reaches a shader with its channels the right way round.
	bool32 bgraVertexColor;
	bool32 bcTextures;
	float32 maxAnisotropy;

	int numTextures;
	int numVertexShaders;
	int numPixelShaders;
	int numVertexBuffers;
	int numIndexBuffers;
};
extern VkGlobals vkGlobals;

// --- the frame, in vkdevice.cpp ---------------------------------------------
//
// Everything is recorded into one command buffer a frame, submitted at
// showRaster. Uploads go into a second one that is submitted ahead of it in the
// same call, which is what lets a texture or a buffer be written in the middle
// of a scene: copies cannot be recorded while a render pass is open, and the
// upload buffer has no render pass in it.
//
// One frame is in flight at a time. Opening a frame waits for the last one, so
// nothing the CPU writes can race a GPU still reading it.

// The frame's commands, opening a frame if none is.
VkCommandBuffer frameCommands(void);
VkCommandBuffer uploadCommands(void);
// Increments with every frame opened. Anything that lives only as long as a
// frame -- arena space, descriptor sets, what a command buffer has bound --
// is stamped with this and taken as gone when it no longer matches.
uint32 frameSerial(void);
// Whether a submitted frame may still be running on the GPU, or one is being
// recorded. Destruction waits while either is true.
bool32 gpuBusy(void);
// Submit what has been recorded and wait for it, without presenting.
void flushFrame(void);
// Close the render pass, if one is open. Needed before any copy or transition.
void endRendering(void);
// Open a render pass on the camera's target, if one is not already. It may not
// manage to: a camera with nothing to draw into has no pass to open.
void ensureRendering(void);
bool32 renderingOpen(void);
// Whether flushFrame may run now. Not while a pass is being drawn into -- the
// commands bound for it would be submitted without the draw.
bool32 flushAllowed(void);

// A VkImage the backend made, and the layout it was last left in.
struct Image
{
	VkImage image;
	VmaAllocation allocation;
	// Identity: what an attachment and a copy use.
	VkImageView view;
	// What a shader samples. The same view unless alpha is forced to one, which
	// is how an X8R8G8B8 texture reads opaque: Vulkan has no X format, and an
	// attachment view has to be identity.
	VkImageView sampleView;
	VkFormat format;
	VkImageLayout layout;
	int32 width;
	int32 height;
	int32 levels;
	VkSampleCountFlagBits samples;
	bool32 depth;
	// Every live image, for the device to destroy what is left when it goes.
	Image *prev;
	Image *next;
};

struct TargetFormat
{
	VkFormat color;
	VkFormat depth;
	VkSampleCountFlagBits samples;
};
// What the open or next render pass draws into.
TargetFormat currentTargetFormat(void);
Image *currentColorTarget(void);
// An image on its way out, which the camera's target may still name.
void forgetImage(Image *img);

// --- resources, in vkresource.cpp -------------------------------------------

Image *createImage(int32 width, int32 height, int32 levels, VkFormat format,
	VkSampleCountFlagBits samples, VkImageUsageFlags usage, bool32 opaqueAlpha);
// Not at once: the frame being recorded, or the one before it, may still use it.
void destroyImage(Image *img);
void transitionImage(VkCommandBuffer cmd, Image *img, VkImageLayout layout);
// Destroy the Vulkan objects behind every image and buffer still alive. A
// raster or a geometry the application has not destroyed by the time the
// device goes keeps its record, and destroying it later frees only that.
void releaseAllResources(void);

// Per-frame space in host-visible buffers, for vertices written this frame,
// shader constants and staging copies. Reset when the next frame opens.
struct ArenaSpan
{
	VkBuffer buffer;
	VkDeviceSize offset;
	uint8 *data;
};
bool32 arenaAlloc(VkDeviceSize size, VkDeviceSize align, ArenaSpan *span);
void arenaReset(void);
void arenaDestroy(void);

void deferDestroyBuffer(VkBuffer buffer, VmaAllocation allocation);
void deferDestroyPipeline(VkPipeline pipeline);
void deferDestroyShaderModule(VkShaderModule module);
void collectGarbage(void);

// Where a vertex or index buffer's contents are for this frame.
bool32 bufferBinding(void *buffer, VkBuffer *vkbuf, VkDeviceSize *offset);

// The image to sample for a raster, uploading its texels first if they have
// changed. nil when there is nothing to sample.
Image *rasterImage(Raster *raster);
void createWhiteTexture(void);
void destroyWhiteTexture(void);
Image *whiteTexture(void);

// --- state and pipelines, in vkpipeline.cpp ---------------------------------

void openPipelines(void);
void closePipelines(void);
void resetRenderState(void);
// A new command buffer has nothing bound.
void commandBufferBegun(void);
// A pass drew with its own pipeline; the next draw rebinds everything.
void invalidateBindings(void);
bool32 prepareDraw(uint32 primType);
void *boundIndexBuffer(void);
void bindIndexBuffer(void *buffer);
void bindFanIndices(VkBuffer buffer);
void forgetRaster(Raster *raster);
void forgetBuffer(void *buffer);
void setRwRenderState(int32 state, void *pvalue);
void *getRwRenderState(int32 state);
void setAlphaTestConstants(uint32 func, uint32 ref);
// The present-time blit: a fullscreen triangle sampling `source` into a target
// of `format`, with the viewport already set.
void drawBlit(VkFormat format, Image *source);

#endif

}

#endif

}
}
