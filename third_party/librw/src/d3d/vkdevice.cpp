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
#include "rwd3dimpl.h"
#ifdef RW_VULKAN
#include "vkincludes.h"
#endif
#include "rwd3dvk.h"

#define PLUGIN_ID 0

namespace rw {
namespace d3d {

// See d3d11device.cpp: a call in here that passes one of rw::d3d's enumerators
// has to name this namespace, or argument-dependent lookup finds the forwarder
// beside the function meant.
namespace implvk {

#ifdef RW_VULKAN

// The Vulkan device, answering to the same interface as D3D9 and D3D11 and
// modelled on D3D11's. What is different is almost all timing: D3D executes a
// call when it is made, Vulkan when the frame's command buffer is submitted.
// rwd3dvk.h describes the frame; the rest of this file is the D3D11 device's
// shape with that in mind.
//
// Requires Vulkan 1.3, for dynamic rendering and the dynamic depth, stencil and
// cull state. Every desktop driver of the last several years has it, and so do
// Android 14 devices and MoltenVK.

VkGlobals vkGlobals;

// The attributes a shader reads that the geometry may not carry; see
// d3d11device.cpp.
void *constantVertexStream;

int32 maxAnisotropy(void) { return (int32)vkGlobals.maxAnisotropy; }

// --- the frame --------------------------------------------------------------

static struct {
	VkCommandPool pool;
	VkCommandBuffer cmd;
	VkCommandBuffer upload;
	VkFence fence;
	bool32 open;
	bool32 pending;
	bool32 uploadUsed;
	uint32 serial;
} frame;

static void
openFrame(void)
{
	if(frame.open)
		return;
	if(frame.pending){
		vkWaitForFences(vkGlobals.device, 1, &frame.fence, VK_TRUE, UINT64_MAX);
		frame.pending = 0;
	}
	collectGarbage();
	vkResetFences(vkGlobals.device, 1, &frame.fence);
	vkResetCommandPool(vkGlobals.device, frame.pool, 0);
	arenaReset();

	VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(frame.cmd, &bi);
	vkBeginCommandBuffer(frame.upload, &bi);
	frame.open = 1;
	frame.uploadUsed = 0;
	frame.serial++;
	commandBufferBegun();
}

VkCommandBuffer
frameCommands(void)
{
	openFrame();
	return frame.cmd;
}

VkCommandBuffer
uploadCommands(void)
{
	openFrame();
	frame.uploadUsed = 1;
	return frame.upload;
}

uint32 frameSerial(void) { return frame.serial; }
bool32 gpuBusy(void) { return frame.open || frame.pending; }

// --- targets ----------------------------------------------------------------

// What a camera draws into. The scene is the virtual screen; a camera texture
// brings its own colour image and, when its z raster matches, its depth.
struct Target
{
	Image *color;
	Image *resolve;
	Image *depth;
	int32 width;
	int32 height;
};

static Target target;
static bool32 rendering;
// The present-time blit's pass, which `rendering` does not describe.
static bool32 presenting;

bool32 renderingOpen(void) { return rendering; }
bool32 flushAllowed(void) { return !rendering && !presenting; }

void
forgetImage(Image *img)
{
	if(target.color == img || target.resolve == img || target.depth == img){
		endRendering();
		memset(&target, 0, sizeof(target));
	}
}

// The fixed-size screen, as rwd3d.h describes it. Here it is not optional in any
// case: the swap chain's images are only touched at present time, so a scene
// with no virtual screen draws into one the size of the window.
int32 virtualScreenWidth;
int32 virtualScreenHeight;
static int32 virtualScreenSamples = 1;
static int32 virtualScreenGranted = 1;
static Image *sceneColor;	// single-sampled, what is read
static Image *sceneMS;		// multisampled, what is drawn into when there is one
static Image *sceneDepth;
static int32 sceneWidth;
static int32 sceneHeight;

static void
windowPixels(int32 *width, int32 *height)
{
	int w = 0, h = 0;
	if(vkGlobals.window)
		SDL_GetWindowSizeInPixels(vkGlobals.window, &w, &h);
	*width = w;
	*height = h;
}

void
getScreenExtent(int32 *width, int32 *height)
{
	if(virtualScreenWidth && virtualScreenHeight){
		*width = virtualScreenWidth;
		*height = virtualScreenHeight;
		return;
	}
	windowPixels(width, height);
}

static void
releaseScene(void)
{
	if(rendering)
		endRendering();
	destroyImage(sceneColor);
	destroyImage(sceneMS);
	destroyImage(sceneDepth);
	sceneColor = sceneMS = sceneDepth = nil;
	sceneWidth = sceneHeight = 0;
	virtualScreenGranted = 1;
}

static void
acquireScene(void)
{
	int32 w, h;
	getScreenExtent(&w, &h);
	if(vkGlobals.device == VK_NULL_HANDLE || w <= 0 || h <= 0)
		return;
	if(sceneColor && sceneWidth == w && sceneHeight == h)
		return;
	releaseScene();

	sceneColor = createImage(w, h, 1, VK_FORMAT_B8G8R8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
		VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0);
	if(sceneColor == nil)
		return;

	// Walked down to the next count the device has, as D3D11 walks down.
	VkSampleCountFlags have = vkGlobals.properties.limits.framebufferColorSampleCounts &
	                          vkGlobals.properties.limits.framebufferDepthSampleCounts;
	for(int32 want = virtualScreenSamples; want > 1; want /= 2){
		if(!(have & want))
			continue;
		sceneMS = createImage(w, h, 1, VK_FORMAT_B8G8R8A8_UNORM, (VkSampleCountFlagBits)want,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0);
		if(sceneMS){
			virtualScreenGranted = want;
			break;
		}
	}
	sceneDepth = createImage(w, h, 1, vkGlobals.depthFormat,
		(VkSampleCountFlagBits)virtualScreenGranted,
		VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0);
	sceneWidth = w;
	sceneHeight = h;
}

void
setVirtualScreen(int32 width, int32 height)
{
	if(virtualScreenWidth == width && virtualScreenHeight == height)
		return;
	virtualScreenWidth = width;
	virtualScreenHeight = height;
	acquireScene();
}

void
getVirtualScreen(int32 *width, int32 *height)
{
	*width = virtualScreenWidth;
	*height = virtualScreenHeight;
}

void setVirtualScreenSamples(int32 samples) { virtualScreenSamples = samples < 1 ? 1 : samples; }
int32 getVirtualScreenSamples(void) { return virtualScreenGranted; }

// The single-sampled picture, for anything that reads the frame. A multisampled
// scene is resolved into it when a render pass on it ends, so ending the one
// that is open is all it takes to bring it up to date.
Image*
sceneResolvedImage(void)
{
	if(rendering && target.resolve)
		endRendering();
	return sceneColor;
}

TargetFormat
currentTargetFormat(void)
{
	TargetFormat f;
	f.color = target.color ? target.color->format : VK_FORMAT_UNDEFINED;
	f.depth = target.depth ? target.depth->format : VK_FORMAT_UNDEFINED;
	f.samples = target.color ? target.color->samples : VK_SAMPLE_COUNT_1_BIT;
	return f;
}

Image*
currentColorTarget(void)
{
	return target.resolve ? target.resolve : target.color;
}

void
endRendering(void)
{
	if(!rendering)
		return;
	vkCmdEndRendering(frame.cmd);
	rendering = 0;
}

static void
beginRendering(bool32 clearColor, RGBA *color, bool32 clearDepth, bool32 clearStencil)
{
	VkCommandBuffer cmd = frameCommands();
	if(rendering)
		endRendering();
	if(target.color == nil)
		return;

	bool32 colorFresh = target.color->layout == VK_IMAGE_LAYOUT_UNDEFINED;
	transitionImage(cmd, target.color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	ca.imageView = target.color->view;
	ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	ca.loadOp = clearColor ? VK_ATTACHMENT_LOAD_OP_CLEAR :
	            colorFresh ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD;
	ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	if(clearColor){
		ca.clearValue.color.float32[0] = color->red/255.0f;
		ca.clearValue.color.float32[1] = color->green/255.0f;
		ca.clearValue.color.float32[2] = color->blue/255.0f;
		ca.clearValue.color.float32[3] = color->alpha/255.0f;
	}
	if(target.resolve){
		transitionImage(cmd, target.resolve, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		ca.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
		ca.resolveImageView = target.resolve->view;
		ca.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	}

	VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	VkRenderingAttachmentInfo sa = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	bool32 stencil = 0;
	if(target.depth){
		bool32 depthFresh = target.depth->layout == VK_IMAGE_LAYOUT_UNDEFINED;
		transitionImage(cmd, target.depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
		stencil = target.depth->format != VK_FORMAT_D32_SFLOAT && target.depth->format != VK_FORMAT_D16_UNORM;
		da.imageView = target.depth->view;
		da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		da.loadOp = clearDepth ? VK_ATTACHMENT_LOAD_OP_CLEAR :
		            depthFresh ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD;
		da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		da.clearValue.depthStencil.depth = 1.0f;
		sa = da;
		sa.loadOp = clearStencil ? VK_ATTACHMENT_LOAD_OP_CLEAR :
		            depthFresh ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD;
		sa.clearValue.depthStencil.stencil = 0;
	}

	VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
	ri.renderArea.extent.width = target.width;
	ri.renderArea.extent.height = target.height;
	ri.layerCount = 1;
	ri.colorAttachmentCount = 1;
	ri.pColorAttachments = &ca;
	ri.pDepthAttachment = target.depth ? &da : nil;
	ri.pStencilAttachment = stencil ? &sa : nil;
	vkCmdBeginRendering(cmd, &ri);
	rendering = 1;

	// D3D's clip space puts +y at the top of the target and Vulkan's puts it at
	// the bottom. A negative height flips it back, so the shaders, the
	// projection matrix and every camera texture's orientation stay D3D's.
	VkViewport vp;
	vp.x = 0.0f;
	vp.y = (float)target.height;
	vp.width = (float)target.width;
	vp.height = -(float)target.height;
	vp.minDepth = 0.0f;
	vp.maxDepth = 1.0f;
	vkCmdSetViewport(cmd, 0, 1, &vp);
	VkRect2D scissor;
	scissor.offset.x = 0;
	scissor.offset.y = 0;
	scissor.extent.width = target.width;
	scissor.extent.height = target.height;
	vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void
ensureRendering(void)
{
	if(!rendering)
		beginRendering(0, nil, 0, 0);
}

static void
setRenderSurfaces(Camera *cam)
{
	acquireScene();

	Target t;
	memset(&t, 0, sizeof(t));
	t.color = sceneMS ? sceneMS : sceneColor;
	t.resolve = sceneMS ? sceneColor : nil;
	t.depth = sceneDepth;
	t.width = sceneWidth;
	t.height = sceneHeight;

	if(cam->frameBuffer && cam->frameBuffer->type == Raster::CAMERATEXTURE){
		Image *img = (Image*)GETD3DRASTEREXT(cam->frameBuffer)->vk;
		if(img){
			t.color = img;
			t.resolve = nil;
			t.width = img->width;
			t.height = img->height;
			t.depth = nil;
			if(cam->zBuffer){
				Image *z = (Image*)GETD3DRASTEREXT(cam->zBuffer)->vk;
				if(z && z->width == img->width && z->height == img->height)
					t.depth = z;
			}
		}
	}

	if(memcmp(&t, &target, sizeof(Target)) != 0){
		endRendering();
		target = t;
	}
}

// --- the swap chain ---------------------------------------------------------

static struct {
	VkSwapchainKHR swapchain;
	VkFormat format;
	VkExtent2D extent;
	uint32 numImages;
	VkImage *images;
	VkImageView *views;
	VkSemaphore *renderDone;
	VkSemaphore acquired;
	bool32 vsync;
	bool32 dirty;
} swap;

static void
releaseSwapchainViews(void)
{
	for(uint32 i = 0; i < swap.numImages; i++){
		vkDestroyImageView(vkGlobals.device, swap.views[i], nil);
		vkDestroySemaphore(vkGlobals.device, swap.renderDone[i], nil);
	}
	rwFree(swap.images);
	rwFree(swap.views);
	rwFree(swap.renderDone);
	swap.images = nil;
	swap.views = nil;
	swap.renderDone = nil;
	swap.numImages = 0;
}

static bool32
createSwapchain(void)
{
	VkPhysicalDevice pd = vkGlobals.physicalDevice;
	VkSurfaceCapabilitiesKHR caps;
	if(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, vkGlobals.surface, &caps) != VK_SUCCESS)
		return 0;

	VkExtent2D extent = caps.currentExtent;
	if(extent.width == 0xFFFFFFFF){
		int32 w, h;
		windowPixels(&w, &h);
		extent.width = w;
		extent.height = h;
	}
	if(extent.width == 0 || extent.height == 0)
		return 0;

	uint32 n = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(pd, vkGlobals.surface, &n, nil);
	VkSurfaceFormatKHR *formats = rwNewT(VkSurfaceFormatKHR, n ? n : 1, MEMDUR_FUNCTION | ID_DRIVER);
	vkGetPhysicalDeviceSurfaceFormatsKHR(pd, vkGlobals.surface, &n, formats);
	VkSurfaceFormatKHR fmt = formats[0];
	// UNORM, not SRGB: the game's colours are already what the screen should
	// show, the way both D3D backends present them.
	for(uint32 i = 0; i < n; i++)
		if((formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) &&
		   formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR){
			fmt = formats[i];
			break;
		}
	rwFree(formats);

	VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
	if(!swap.vsync){
		vkGetPhysicalDeviceSurfacePresentModesKHR(pd, vkGlobals.surface, &n, nil);
		VkPresentModeKHR *modes = rwNewT(VkPresentModeKHR, n ? n : 1, MEMDUR_FUNCTION | ID_DRIVER);
		vkGetPhysicalDeviceSurfacePresentModesKHR(pd, vkGlobals.surface, &n, modes);
		for(uint32 i = 0; i < n; i++){
			if(modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR)
				mode = modes[i];
			if(modes[i] == VK_PRESENT_MODE_MAILBOX_KHR && mode != VK_PRESENT_MODE_IMMEDIATE_KHR)
				mode = modes[i];
		}
		rwFree(modes);
	}

	uint32 count = caps.minImageCount + 1;
	if(caps.maxImageCount && count > caps.maxImageCount)
		count = caps.maxImageCount;

	VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
	ci.surface = vkGlobals.surface;
	ci.minImageCount = count;
	ci.imageFormat = fmt.format;
	ci.imageColorSpace = fmt.colorSpace;
	ci.imageExtent = extent;
	ci.imageArrayLayers = 1;
	ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ci.preTransform = caps.currentTransform;
	ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	if(!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR))
		ci.compositeAlpha = (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha &
			(~caps.supportedCompositeAlpha + 1));
	ci.presentMode = mode;
	ci.clipped = VK_TRUE;
	ci.oldSwapchain = swap.swapchain;

	VkSwapchainKHR sc;
	if(vkCreateSwapchainKHR(vkGlobals.device, &ci, nil, &sc) != VK_SUCCESS)
		return 0;
	releaseSwapchainViews();
	if(swap.swapchain != VK_NULL_HANDLE)
		vkDestroySwapchainKHR(vkGlobals.device, swap.swapchain, nil);
	swap.swapchain = sc;
	swap.format = fmt.format;
	swap.extent = extent;

	vkGetSwapchainImagesKHR(vkGlobals.device, sc, &n, nil);
	swap.numImages = n;
	swap.images = rwNewT(VkImage, n, MEMDUR_EVENT | ID_DRIVER);
	swap.views = rwNewT(VkImageView, n, MEMDUR_EVENT | ID_DRIVER);
	swap.renderDone = rwNewT(VkSemaphore, n, MEMDUR_EVENT | ID_DRIVER);
	vkGetSwapchainImagesKHR(vkGlobals.device, sc, &n, swap.images);
	for(uint32 i = 0; i < n; i++){
		VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		vi.image = swap.images[i];
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = fmt.format;
		vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		vi.subresourceRange.levelCount = 1;
		vi.subresourceRange.layerCount = 1;
		vkCreateImageView(vkGlobals.device, &vi, nil, &swap.views[i]);
		VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		vkCreateSemaphore(vkGlobals.device, &si, nil, &swap.renderDone[i]);
	}
	swap.dirty = 0;
	return 1;
}

static void
destroySwapchain(void)
{
	releaseSwapchainViews();
	if(swap.swapchain != VK_NULL_HANDLE){
		vkDestroySwapchainKHR(vkGlobals.device, swap.swapchain, nil);
		swap.swapchain = VK_NULL_HANDLE;
	}
}

// --- submission -------------------------------------------------------------

static void
submitFrame(bool32 present, uint32 imageIndex)
{
	if(!frame.open)
		return;
	endRendering();
	vkEndCommandBuffer(frame.upload);
	vkEndCommandBuffer(frame.cmd);

	VkCommandBuffer bufs[2];
	uint32 numBufs = 0;
	if(frame.uploadUsed)
		bufs[numBufs++] = frame.upload;
	bufs[numBufs++] = frame.cmd;

	VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	si.commandBufferCount = numBufs;
	si.pCommandBuffers = bufs;
	if(present){
		si.waitSemaphoreCount = 1;
		si.pWaitSemaphores = &swap.acquired;
		si.pWaitDstStageMask = &waitStage;
		si.signalSemaphoreCount = 1;
		si.pSignalSemaphores = &swap.renderDone[imageIndex];
	}
	VkResult r = vkQueueSubmit(vkGlobals.queue, 1, &si, frame.fence);
	frame.open = 0;
	frame.pending = r == VK_SUCCESS;
	if(r == VK_ERROR_DEVICE_LOST){
		static bool32 said;
		if(!said){
			fprintf(stderr, "librw: the Vulkan device is lost; nothing will be drawn from here on\n");
			fflush(stderr);
			said = 1;
		}
	}

	if(present && r == VK_SUCCESS){
		VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
		pi.waitSemaphoreCount = 1;
		pi.pWaitSemaphores = &swap.renderDone[imageIndex];
		pi.swapchainCount = 1;
		pi.pSwapchains = &swap.swapchain;
		pi.pImageIndices = &imageIndex;
		VkResult pr = vkQueuePresentKHR(vkGlobals.queue, &pi);
		if(pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR)
			swap.dirty = 1;
	}
}

void
flushFrame(void)
{
	if(!frame.open)
		return;
	submitFrame(0, 0);
	if(frame.pending){
		vkWaitForFences(vkGlobals.device, 1, &frame.fence, VK_TRUE, UINT64_MAX);
		frame.pending = 0;
	}
}

// --- the camera -------------------------------------------------------------

bool32
deviceOpen(void)
{
	return vkGlobals.device != VK_NULL_HANDLE;
}

// The same view and projection D3D11 builds: its clip space is D3D9's, and the
// viewport flip in beginRendering makes it Vulkan's.
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

	// Not optional; see d3d11device.cpp.
	d3dShaderState.fogData.start = cam->fogPlane;
	d3dShaderState.fogData.end = cam->farPlane;
	d3dShaderState.fogData.range = 1.0f/(cam->fogPlane - cam->farPlane);
	d3dShaderState.fogData.disable = getRwRenderState(FOGENABLE) ? 0.0f : 1.0f;
	d3dShaderState.fogDisable.start = 0.0f;
	d3dShaderState.fogDisable.end = 0.0f;
	d3dShaderState.fogDisable.range = 0.0f;
	d3dShaderState.fogDisable.disable = 1.0f;
	d3dShaderState.fogDirty = true;

	setRenderSurfaces(cam);
}

static void
endUpdate(Camera *cam)
{
	(void)cam;
}

static void
clearCamera(Camera *cam, RGBA *col, uint32 mode)
{
	// A clear before the scene opens is how the game wipes a camera texture it
	// is about to render into, so the camera's surfaces are settled here too.
	setRenderSurfaces(cam);
	if(target.color == nil)
		return;

	bool32 color = (mode & Camera::CLEARIMAGE) != 0;
	bool32 depth = (mode & Camera::CLEARZ) && target.depth;
	bool32 stencil = (mode & Camera::CLEARSTENCIL) && target.depth;
	if(!color && !depth && !stencil)
		return;

	// With no pass open the clear is the load operation of the one it opens,
	// which is also what gives an image it knows nothing about its first layout.
	if(!rendering){
		beginRendering(color, col, depth, stencil);
		return;
	}

	VkClearAttachment atts[2];
	uint32 n = 0;
	if(color){
		atts[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		atts[n].colorAttachment = 0;
		atts[n].clearValue.color.float32[0] = col->red/255.0f;
		atts[n].clearValue.color.float32[1] = col->green/255.0f;
		atts[n].clearValue.color.float32[2] = col->blue/255.0f;
		atts[n].clearValue.color.float32[3] = col->alpha/255.0f;
		n++;
	}
	if(depth || stencil){
		atts[n].aspectMask = 0;
		if(depth)
			atts[n].aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
		if(stencil && target.depth->format != VK_FORMAT_D32_SFLOAT)
			atts[n].aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
		atts[n].colorAttachment = 0;
		atts[n].clearValue.depthStencil.depth = 1.0f;
		atts[n].clearValue.depthStencil.stencil = 0;
		if(atts[n].aspectMask)
			n++;
	}
	VkClearRect rect;
	rect.rect.offset.x = 0;
	rect.rect.offset.y = 0;
	rect.rect.extent.width = target.width;
	rect.rect.extent.height = target.height;
	rect.baseArrayLayer = 0;
	rect.layerCount = 1;
	vkCmdClearAttachments(frame.cmd, n, atts, 1, &rect);
}

// The frame so far, into a camera texture the caller sized from
// getScreenExtent. Called with no scene open.
bool32
captureFrame(Raster *dst)
{
	if(vkGlobals.device == VK_NULL_HANDLE || dst == nil)
		return 0;
	Image *img = (Image*)GETD3DRASTEREXT(dst)->vk;
	VkCommandBuffer cmd = frameCommands();
	Image *src = sceneResolvedImage();
	if(img == nil || src == nil || img == src)
		return 0;
	if(img->width != src->width || img->height != src->height || img->format != src->format)
		return 0;

	endRendering();
	transitionImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	transitionImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VkImageCopy copy;
	memset(&copy, 0, sizeof(copy));
	copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copy.srcSubresource.layerCount = 1;
	copy.dstSubresource = copy.srcSubresource;
	copy.extent.width = src->width;
	copy.extent.height = src->height;
	copy.extent.depth = 1;
	vkCmdCopyImage(cmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	return 1;
}

// The frame into a camera texture at an offset; see d3d11device.cpp.
static bool32
rasterRenderFast(Raster *raster, int32 x, int32 y)
{
	Raster *dst = Raster::getCurrentContext();
	if(dst == nil || dst->type != Raster::CAMERATEXTURE || raster->type != Raster::CAMERA)
		return 0;
	Image *img = (Image*)GETD3DRASTEREXT(dst)->vk;
	VkCommandBuffer cmd = frameCommands();
	Image *src = sceneResolvedImage();
	if(img == nil || src == nil || img->format != src->format)
		return 0;

	int32 left = x < 0 ? 0 : x;
	int32 top = y < 0 ? 0 : y;
	int32 right = src->width < img->width ? src->width : img->width;
	int32 bottom = src->height < img->height ? src->height : img->height;
	if(left >= right || top >= bottom)
		return 0;

	endRendering();
	transitionImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	transitionImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VkImageCopy copy;
	memset(&copy, 0, sizeof(copy));
	copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copy.srcSubresource.layerCount = 1;
	copy.srcOffset.x = left;
	copy.srcOffset.y = top;
	copy.dstSubresource = copy.srcSubresource;
	copy.dstOffset = copy.srcOffset;
	copy.extent.width = right - left;
	copy.extent.height = bottom - top;
	copy.extent.depth = 1;
	vkCmdCopyImage(cmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	return 1;
}

// Stretch the scene into the swap chain image with its aspect ratio kept, the
// rest cleared to black, and present it.
static void
showRaster(Raster *raster, uint32 flags)
{
	(void)raster;
	if(vkGlobals.device == VK_NULL_HANDLE)
		return;
	VkCommandBuffer cmd = frameCommands();
	endRendering();

	bool32 vsync = (flags & Raster::FLIPWAITVSYNCH) != 0;
	if(vsync != swap.vsync){
		swap.vsync = vsync;
		swap.dirty = 1;
	}
	int32 w, h;
	windowPixels(&w, &h);
	if((uint32)w != swap.extent.width || (uint32)h != swap.extent.height)
		swap.dirty = 1;
	if(swap.dirty && w > 0 && h > 0){
		// The frame being recorded does not name the swap chain yet, so what
		// has to finish first is only the one before it.
		if(frame.pending){
			vkWaitForFences(vkGlobals.device, 1, &frame.fence, VK_TRUE, UINT64_MAX);
			frame.pending = 0;
		}
		vkQueueWaitIdle(vkGlobals.queue);
		createSwapchain();
	}

	Image *src = sceneResolvedImage();
	uint32 index = 0;
	VkResult r = VK_ERROR_OUT_OF_DATE_KHR;
	if(swap.swapchain != VK_NULL_HANDLE && src && !swap.dirty)
		r = vkAcquireNextImageKHR(vkGlobals.device, swap.swapchain, UINT64_MAX,
			swap.acquired, VK_NULL_HANDLE, &index);
	if(r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR){
		if(r == VK_ERROR_OUT_OF_DATE_KHR)
			swap.dirty = 1;
		submitFrame(0, 0);
		return;
	}
	if(r == VK_SUBOPTIMAL_KHR)
		swap.dirty = 1;

	transitionImage(cmd, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = swap.images[index];
	b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	b.subresourceRange.levelCount = 1;
	b.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nil, 0, nil, 1, &b);

	VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	ca.imageView = swap.views[index];
	ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	ca.clearValue.color.float32[3] = 1.0f;
	VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
	ri.renderArea.extent = swap.extent;
	ri.layerCount = 1;
	ri.colorAttachmentCount = 1;
	ri.pColorAttachments = &ca;
	vkCmdBeginRendering(cmd, &ri);

	float scale = swap.extent.width/(float)src->width;
	float other = swap.extent.height/(float)src->height;
	if(other < scale)
		scale = other;
	VkViewport vp;
	vp.width = src->width*scale;
	vp.height = src->height*scale;
	vp.x = (swap.extent.width - vp.width)/2.0f;
	vp.y = (swap.extent.height - vp.height)/2.0f + vp.height;
	vp.height = -vp.height;
	vp.minDepth = 0.0f;
	vp.maxDepth = 1.0f;
	vkCmdSetViewport(cmd, 0, 1, &vp);
	VkRect2D scissor;
	scissor.offset.x = 0;
	scissor.offset.y = 0;
	scissor.extent = swap.extent;
	vkCmdSetScissor(cmd, 0, 1, &scissor);
	presenting = 1;
	drawBlit(swap.format, src);
	presenting = 0;
	vkCmdEndRendering(cmd);

	b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	b.dstAccessMask = 0;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nil, 0, nil, 1, &b);

	submitFrame(1, index);
}

// --- drawing ----------------------------------------------------------------

// Vulkan has a triangle fan, but MoltenVK and the portability subset do not, so
// fans are drawn as lists the way D3D11 draws them.
#define MAXFANVERTICES 10000
static void *fanIndices;

static void
createFanIndices(void)
{
	fanIndices = createIndexBufferVk((MAXFANVERTICES-2)*3*sizeof(uint16), false);
	if(fanIndices == nil)
		return;
	uint16 *idx = (uint16*)lockBufferVk(fanIndices, 0, 0);
	for(int32 i = 0; i < MAXFANVERTICES-2; i++){
		idx[i*3+0] = 0;
		idx[i*3+1] = (uint16)(i+1);
		idx[i*3+2] = (uint16)(i+2);
	}
	unlockBufferVk(fanIndices);
}

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

void
drawPrimitive(uint32 primType, uint32 startVertex, uint32 numPrimitives)
{
	if(numPrimitives == 0)
		return;
	if(primType == D3DPT_TRIANGLEFAN){
		if(fanIndices == nil || numPrimitives > MAXFANVERTICES-2)
			return;
		if(!prepareDraw(D3DPT_TRIANGLELIST))
			return;
		VkBuffer buf;
		VkDeviceSize off;
		if(!bufferBinding(fanIndices, &buf, &off))
			return;
		bindFanIndices(buf);
		vkCmdDrawIndexed(frame.cmd, numPrimitives*3, 1, 0, (int32)startVertex, 0);
		return;
	}
	uint32 count = indexCount(primType, numPrimitives);
	if(count == 0 || !prepareDraw(primType))
		return;
	vkCmdDraw(frame.cmd, count, 1, startVertex, 0);
}

void
drawIndexedPrimitive(uint32 primType, int32 baseVertex, uint32 minVertex,
	uint32 numVertices, uint32 startIndex, uint32 numPrimitives)
{
	(void)minVertex;
	(void)numVertices;
	uint32 count = indexCount(primType, numPrimitives);
	if(count == 0 || !prepareDraw(primType))
		return;
	void *ib = boundIndexBuffer();
	if(ib == nil)
		return;
	bindIndexBuffer(ib);
	vkCmdDrawIndexed(frame.cmd, count, 1, startIndex, baseVertex, 0);
}

// --- open, start, stop, close -----------------------------------------------

static bool32
wantValidation(void)
{
#ifdef DEBUG
	const char *env = getenv("RW_VULKAN_VALIDATION");
	return env == nil || env[0] != '0';
#else
	const char *env = getenv("RW_VULKAN_VALIDATION");
	return env && env[0] == '1';
#endif
}

static VkBool32 VKAPI_PTR
debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
	const VkDebugUtilsMessengerCallbackDataEXT *data, void *user)
{
	(void)types;
	(void)user;
	// Enough to see what is wrong, not so many that a per-draw error buries
	// the run.
	static int32 printed;
	if(printed++ < 200){
		fprintf(stderr, "vulkan %s: %s\n",
			severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" : "warning",
			data->pMessage);
		fflush(stderr);
	}
	return VK_FALSE;
}

static bool32
hasExtension(VkExtensionProperties *exts, uint32 n, const char *name)
{
	for(uint32 i = 0; i < n; i++)
		if(strcmp(exts[i].extensionName, name) == 0)
			return 1;
	return 0;
}

static int
openVulkan(EngineOpenParams *params)
{
	memset(&vkGlobals, 0, sizeof(vkGlobals));
	vkGlobals.window = params->sdlWindow;
	if(vkGlobals.window == nil){
		RWERROR((ERR_GENERAL, "Vulkan needs an SDL window"));
		return 0;
	}
	if(volkInitialize() != VK_SUCCESS){
		RWERROR((ERR_GENERAL, "no Vulkan loader on this machine"));
		return 0;
	}
	if(volkGetInstanceVersion() < VK_API_VERSION_1_3){
		RWERROR((ERR_GENERAL, "the Vulkan loader is older than 1.3"));
		return 0;
	}

	const char *extensions[16];
	uint32 numExtensions = 0;
	Uint32 numSdl = 0;
	const char * const *sdlExts = SDL_Vulkan_GetInstanceExtensions(&numSdl);
	for(Uint32 i = 0; i < numSdl && numExtensions < 12; i++)
		extensions[numExtensions++] = sdlExts[i];

	uint32 n = 0;
	vkEnumerateInstanceExtensionProperties(nil, &n, nil);
	VkExtensionProperties *exts = rwNewT(VkExtensionProperties, n ? n : 1, MEMDUR_FUNCTION | ID_DRIVER);
	vkEnumerateInstanceExtensionProperties(nil, &n, exts);
	bool32 validation = 0;
	const char *layers[1] = { "VK_LAYER_KHRONOS_validation" };
	if(wantValidation()){
		uint32 nl = 0;
		vkEnumerateInstanceLayerProperties(&nl, nil);
		VkLayerProperties *lp = rwNewT(VkLayerProperties, nl ? nl : 1, MEMDUR_FUNCTION | ID_DRIVER);
		vkEnumerateInstanceLayerProperties(&nl, lp);
		for(uint32 i = 0; i < nl; i++)
			if(strcmp(lp[i].layerName, layers[0]) == 0)
				validation = 1;
		rwFree(lp);
		if(!validation)
			fprintf(stderr, "librw: Vulkan validation asked for, and the layer is not installed\n");
	}
	bool32 debugUtils = validation && hasExtension(exts, n, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	if(debugUtils)
		extensions[numExtensions++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
	VkInstanceCreateFlags flags = 0;
	if(hasExtension(exts, n, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)){
		extensions[numExtensions++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
		flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
	}
	rwFree(exts);

	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app.pEngineName = "librw";
	app.apiVersion = VK_API_VERSION_1_3;
	VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	ci.flags = flags;
	ci.pApplicationInfo = &app;
	ci.enabledExtensionCount = numExtensions;
	ci.ppEnabledExtensionNames = extensions;
	ci.enabledLayerCount = validation ? 1 : 0;
	ci.ppEnabledLayerNames = layers;
	if(vkCreateInstance(&ci, nil, &vkGlobals.instance) != VK_SUCCESS){
		RWERROR((ERR_GENERAL, "vkCreateInstance failed"));
		return 0;
	}
	volkLoadInstance(vkGlobals.instance);

	if(debugUtils){
		VkDebugUtilsMessengerCreateInfoEXT di = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
		di.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
		                     VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
		di.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
		                 VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
		                 VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
		di.pfnUserCallback = debugCallback;
		vkCreateDebugUtilsMessengerEXT(vkGlobals.instance, &di, nil, &vkGlobals.messenger);
	}

	if(!SDL_Vulkan_CreateSurface(vkGlobals.window, vkGlobals.instance, nil, &vkGlobals.surface)){
		RWERROR((ERR_GENERAL, "SDL_Vulkan_CreateSurface failed"));
		return 0;
	}

	vkEnumeratePhysicalDevices(vkGlobals.instance, &n, nil);
	vkGlobals.adapters = rwNewT(VkPhysicalDevice, n ? n : 1, MEMDUR_EVENT | ID_DRIVER);
	vkEnumeratePhysicalDevices(vkGlobals.instance, &n, vkGlobals.adapters);
	vkGlobals.numAdapters = n;

	// The first discrete GPU that can do the job, else the first that can.
	vkGlobals.adapter = -1;
	for(int32 pass = 0; pass < 2 && vkGlobals.adapter < 0; pass++)
		for(uint32 i = 0; i < n; i++){
			VkPhysicalDeviceProperties p;
			vkGetPhysicalDeviceProperties(vkGlobals.adapters[i], &p);
			if(p.apiVersion < VK_API_VERSION_1_3)
				continue;
			if(pass == 0 && p.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
				continue;
			vkGlobals.adapter = i;
			break;
		}
	if(vkGlobals.adapter < 0){
		RWERROR((ERR_GENERAL, "no Vulkan 1.3 device"));
		return 0;
	}
	return 1;
}

static int
closeVulkan(void)
{
	if(vkGlobals.surface != VK_NULL_HANDLE)
		vkDestroySurfaceKHR(vkGlobals.instance, vkGlobals.surface, nil);
	if(vkGlobals.messenger != VK_NULL_HANDLE)
		vkDestroyDebugUtilsMessengerEXT(vkGlobals.instance, vkGlobals.messenger, nil);
	if(vkGlobals.instance != VK_NULL_HANDLE)
		vkDestroyInstance(vkGlobals.instance, nil);
	rwFree(vkGlobals.adapters);
	memset(&vkGlobals, 0, sizeof(vkGlobals));
	return 1;
}

static int
startVulkan(void)
{
	VkPhysicalDevice pd = vkGlobals.adapters[vkGlobals.adapter];
	vkGlobals.physicalDevice = pd;
	vkGetPhysicalDeviceProperties(pd, &vkGlobals.properties);

	uint32 n = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, nil);
	VkQueueFamilyProperties *qf = rwNewT(VkQueueFamilyProperties, n ? n : 1, MEMDUR_FUNCTION | ID_DRIVER);
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, qf);
	int32 family = -1;
	for(uint32 i = 0; i < n && family < 0; i++){
		VkBool32 present = VK_FALSE;
		vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, vkGlobals.surface, &present);
		if((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present)
			family = i;
	}
	rwFree(qf);
	if(family < 0){
		RWERROR((ERR_GENERAL, "no Vulkan queue can both draw and present to this window"));
		return 0;
	}
	vkGlobals.queueFamily = family;

	VkPhysicalDeviceFeatures have;
	vkGetPhysicalDeviceFeatures(pd, &have);
	VkPhysicalDeviceVulkan13Features have13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
	VkPhysicalDeviceFeatures2 have2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
	have2.pNext = &have13;
	vkGetPhysicalDeviceFeatures2(pd, &have2);
	if(!have13.dynamicRendering){
		RWERROR((ERR_GENERAL, "the Vulkan device has no dynamic rendering"));
		return 0;
	}

	VkPhysicalDeviceVulkan13Features want13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
	want13.dynamicRendering = VK_TRUE;
	VkPhysicalDeviceFeatures2 want2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
	want2.pNext = &want13;
	want2.features.samplerAnisotropy = have.samplerAnisotropy;
	want2.features.textureCompressionBC = have.textureCompressionBC;

	float priority = 1.0f;
	VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	qi.queueFamilyIndex = family;
	qi.queueCount = 1;
	qi.pQueuePriorities = &priority;

	const char *extensions[2] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	uint32 numExtensions = 1;
	vkEnumerateDeviceExtensionProperties(pd, nil, &n, nil);
	VkExtensionProperties *exts = rwNewT(VkExtensionProperties, n ? n : 1, MEMDUR_FUNCTION | ID_DRIVER);
	vkEnumerateDeviceExtensionProperties(pd, nil, &n, exts);
	// A portability device must say it knows it is one.
	if(hasExtension(exts, n, "VK_KHR_portability_subset"))
		extensions[numExtensions++] = "VK_KHR_portability_subset";
	rwFree(exts);

	VkDeviceCreateInfo di = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	di.pNext = &want2;
	di.queueCreateInfoCount = 1;
	di.pQueueCreateInfos = &qi;
	di.enabledExtensionCount = numExtensions;
	di.ppEnabledExtensionNames = extensions;
	if(vkCreateDevice(pd, &di, nil, &vkGlobals.device) != VK_SUCCESS){
		RWERROR((ERR_GENERAL, "vkCreateDevice failed"));
		return 0;
	}
	volkLoadDevice(vkGlobals.device);
	vkGetDeviceQueue(vkGlobals.device, family, 0, &vkGlobals.queue);

	VmaAllocatorCreateInfo ai = {};
	ai.vulkanApiVersion = VK_API_VERSION_1_3;
	ai.physicalDevice = pd;
	ai.device = vkGlobals.device;
	ai.instance = vkGlobals.instance;
	VmaVulkanFunctions funcs;
	vmaImportVulkanFunctionsFromVolk(&ai, &funcs);
	ai.pVulkanFunctions = &funcs;
	if(vmaCreateAllocator(&ai, &vkGlobals.allocator) != VK_SUCCESS){
		RWERROR((ERR_GENERAL, "vmaCreateAllocator failed"));
		return 0;
	}

	VkPipelineCacheCreateInfo pci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
	vkCreatePipelineCache(vkGlobals.device, &pci, nil, &vkGlobals.pipelineCache);

	static const VkFormat depthFormats[] = {
		VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D32_SFLOAT
	};
	vkGlobals.depthFormat = VK_FORMAT_UNDEFINED;
	for(int i = 0; i < 3 && vkGlobals.depthFormat == VK_FORMAT_UNDEFINED; i++){
		VkFormatProperties fp;
		vkGetPhysicalDeviceFormatProperties(pd, depthFormats[i], &fp);
		if(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
			vkGlobals.depthFormat = depthFormats[i];
	}
	VkFormatProperties fp;
	vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_B8G8R8A8_UNORM, &fp);
	vkGlobals.bgraVertexColor = (fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0;
	vkGlobals.bcTextures = have.textureCompressionBC;
	vkGlobals.maxAnisotropy = have.samplerAnisotropy ? vkGlobals.properties.limits.maxSamplerAnisotropy : 1.0f;

	VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	cpi.queueFamilyIndex = family;
	vkCreateCommandPool(vkGlobals.device, &cpi, nil, &frame.pool);
	VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	cai.commandPool = frame.pool;
	cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cai.commandBufferCount = 1;
	vkAllocateCommandBuffers(vkGlobals.device, &cai, &frame.cmd);
	vkAllocateCommandBuffers(vkGlobals.device, &cai, &frame.upload);
	VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	vkCreateFence(vkGlobals.device, &fi, nil, &frame.fence);
	VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	vkCreateSemaphore(vkGlobals.device, &si, nil, &swap.acquired);
	frame.open = 0;
	frame.pending = 0;

	swap.vsync = 1;
	if(!createSwapchain())
		swap.dirty = 1;
	return 1;
}

static int
initVulkan(void)
{
	// No paletted texture format; librw expands a palette before a raster is
	// created, as it does for D3D11.
	isP8supported = 0;
	memset(&target, 0, sizeof(target));
	rendering = 0;

	openPipelines();
	createWhiteTexture();
	createFanIndices();
	acquireScene();

	VertexConstantData constants;
	memset(&constants, 0, sizeof(constants));
	uint8 base = constantVertexColorWhite ? 255 : 0;
	constants.color.red = base;
	constants.color.green = base;
	constants.color.blue = base;
	constants.color.alpha = 255;
	constantVertexStream = createVertexBufferVk(sizeof(constants), false);
	if(constantVertexStream){
		uint8 *locked = lockBufferVk(constantVertexStream, 0, sizeof(constants));
		memcpy(locked, &constants, sizeof(constants));
		unlockBufferVk(constantVertexStream);
		implvk::setStreamSource(2, constantVertexStream, 0, 0);
	}

	openIm2D();
	openIm3D();
	return 1;
}

static int
termVulkan(void)
{
	if(vkGlobals.device == VK_NULL_HANDLE)
		return 1;
	closeIm3D();
	closeIm2D();
	destroyVertexBufferVk(constantVertexStream);
	constantVertexStream = nil;
	destroyIndexBufferVk(fanIndices);
	fanIndices = nil;

	flushFrame();
	vkDeviceWaitIdle(vkGlobals.device);
	releaseScene();
	destroyWhiteTexture();
	closePipelines();
	collectGarbage();
	releaseAllResources();
	arenaDestroy();

	destroySwapchain();
	vkDestroySemaphore(vkGlobals.device, swap.acquired, nil);
	vkDestroyFence(vkGlobals.device, frame.fence, nil);
	vkDestroyCommandPool(vkGlobals.device, frame.pool, nil);
	memset(&frame, 0, sizeof(frame));
	memset(&swap, 0, sizeof(swap));
	vkDestroyPipelineCache(vkGlobals.device, vkGlobals.pipelineCache, nil);
	vmaDestroyAllocator(vkGlobals.allocator);
	vkDestroyDevice(vkGlobals.device, nil);
	vkGlobals.device = VK_NULL_HANDLE;
	vkGlobals.allocator = nil;
	return 1;
}

// --- the device interface ---------------------------------------------------

static int
deviceSystem(DeviceReq req, void *arg, int32 n)
{
	VideoMode *rwmode;
	VkPhysicalDeviceProperties props;
	int32 w, h;

	switch(req){
	case DEVICEOPEN:
		return openVulkan((EngineOpenParams*)arg);
	case DEVICECLOSE:
		return closeVulkan();

	case DEVICEINIT:
		return startVulkan() && initVulkan();
	case DEVICETERM:
		return termVulkan();

	case DEVICEFINALIZE:
		return 1;

	case DEVICEGETNUMSUBSYSTEMS:
		return vkGlobals.numAdapters;
	case DEVICEGETCURRENTSUBSYSTEM:
		return vkGlobals.adapter;
	case DEVICESETSUBSYSTEM:
		if(n < 0 || n >= vkGlobals.numAdapters)
			return 0;
		vkGlobals.adapter = n;
		return 1;
	case DEVICEGETSUBSSYSTEMINFO:
		if(n < 0 || n >= vkGlobals.numAdapters)
			return 0;
		vkGetPhysicalDeviceProperties(vkGlobals.adapters[n], &props);
		strncpy(((SubSystemInfo*)arg)->name, props.deviceName, sizeof(SubSystemInfo::name));
		((SubSystemInfo*)arg)->name[sizeof(SubSystemInfo::name)-1] = '\0';
		return 1;

	// One mode: the window. The application owns the window and makes it
	// fullscreen through SDL, so there is no display mode for this to change.
	case DEVICEGETNUMVIDEOMODES:
		return 1;
	case DEVICEGETCURRENTVIDEOMODE:
		return 0;
	case DEVICESETVIDEOMODE:
		return n == 0;
	case DEVICEGETVIDEOMODEINFO:
		rwmode = (VideoMode*)arg;
		windowPixels(&w, &h);
		rwmode->width = w;
		rwmode->height = h;
		rwmode->depth = 32;
		rwmode->flags = 0;
		return 1;

	case DEVICEGETMAXMULTISAMPLINGLEVELS:
		return 1;
	case DEVICEGETMULTISAMPLINGLEVELS:
		return 1;
	case DEVICESETMULTISAMPLINGLEVELS:
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
