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

// --- the arena --------------------------------------------------------------

// Host-visible buffers carved up front to back and given back all at once when
// the next frame opens. Everything that is only needed for one frame lives
// here: vertices written this frame, shader constants, and the staging copy of
// anything on its way to device memory.
struct ArenaChunk
{
	VkBuffer buffer;
	VmaAllocation allocation;
	uint8 *data;
	VkDeviceSize size;
	VkDeviceSize used;
};

#define ARENACHUNKSIZE (8<<20)
// Past this in one frame the frame is submitted and waited for, and the arena
// starts again. A level load stages every texture and buffer it makes before
// anything presents, and would otherwise hold all of it in host memory at once.
#define ARENAFLUSHBYTES (256<<20)

static ArenaChunk *chunks;
static int32 numChunks;
static int32 maxChunks;
static int32 currentChunk;
static VkDeviceSize arenaBytes;

static bool32
addChunk(VkDeviceSize size)
{
	if(numChunks == maxChunks){
		int32 n = maxChunks ? maxChunks*2 : 8;
		ArenaChunk *c = rwNewT(ArenaChunk, n, MEMDUR_EVENT | ID_DRIVER);
		if(chunks){
			memcpy(c, chunks, numChunks*sizeof(ArenaChunk));
			rwFree(chunks);
		}
		chunks = c;
		maxChunks = n;
	}

	VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bi.size = size;
	bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
	           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VmaAllocationCreateInfo ai = {};
	ai.usage = VMA_MEMORY_USAGE_AUTO;
	ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VmaAllocationInfo info;
	ArenaChunk *c = &chunks[numChunks];
	if(vmaCreateBuffer(vkGlobals.allocator, &bi, &ai, &c->buffer, &c->allocation, &info) != VK_SUCCESS)
		return 0;
	c->data = (uint8*)info.pMappedData;
	c->size = size;
	c->used = 0;
	numChunks++;
	return 1;
}

bool32
arenaAlloc(VkDeviceSize size, VkDeviceSize align, ArenaSpan *span)
{
	if(arenaBytes > ARENAFLUSHBYTES && flushAllowed())
		flushFrame();
	// The arena is reset when a frame opens, so one has to be open before any
	// of it is handed out.
	frameCommands();

	for(; currentChunk < numChunks; currentChunk++){
		ArenaChunk *c = &chunks[currentChunk];
		VkDeviceSize off = (c->used + align-1) & ~(align-1);
		if(off + size <= c->size){
			c->used = off + size;
			span->buffer = c->buffer;
			span->offset = off;
			span->data = c->data + off;
			arenaBytes += size;
			return 1;
		}
	}
	if(!addChunk(size > ARENACHUNKSIZE ? size : ARENACHUNKSIZE))
		return 0;
	currentChunk = numChunks-1;
	ArenaChunk *c = &chunks[currentChunk];
	c->used = size;
	span->buffer = c->buffer;
	span->offset = 0;
	span->data = c->data;
	arenaBytes += size;
	return 1;
}

void
arenaReset(void)
{
	for(int32 i = 0; i < numChunks; i++)
		chunks[i].used = 0;
	currentChunk = 0;
	arenaBytes = 0;
}

void
arenaDestroy(void)
{
	for(int32 i = 0; i < numChunks; i++)
		vmaDestroyBuffer(vkGlobals.allocator, chunks[i].buffer, chunks[i].allocation);
	rwFree(chunks);
	chunks = nil;
	numChunks = maxChunks = currentChunk = 0;
	arenaBytes = 0;
}

// --- deferred destruction ---------------------------------------------------

// A D3D resource is reference counted and outlives its last Release for as
// long as a queued command needs it. A Vulkan one is gone when destroyed, and
// the command buffer being recorded -- or the frame still on the GPU -- may
// name it. So destruction is queued, and carried out once the frame that could
// have used it has finished.
enum GarbageType
{
	GARBAGE_BUFFER,
	GARBAGE_IMAGE,
	GARBAGE_PIPELINE,
	GARBAGE_SHADERMODULE
};

struct Garbage
{
	int32 type;
	VkBuffer buffer;
	VkImage image;
	VkImageView views[2];
	VmaAllocation allocation;
	VkPipeline pipeline;
	VkShaderModule module;
};

static Garbage *garbage;
static int32 numGarbage;
static int32 maxGarbage;

static void
destroyNow(Garbage *g)
{
	VkDevice dev = vkGlobals.device;
	switch(g->type){
	case GARBAGE_BUFFER:
		vmaDestroyBuffer(vkGlobals.allocator, g->buffer, g->allocation);
		break;
	case GARBAGE_IMAGE:
		if(g->views[1] != VK_NULL_HANDLE)
			vkDestroyImageView(dev, g->views[1], nil);
		if(g->views[0] != VK_NULL_HANDLE)
			vkDestroyImageView(dev, g->views[0], nil);
		vmaDestroyImage(vkGlobals.allocator, g->image, g->allocation);
		break;
	case GARBAGE_PIPELINE:
		vkDestroyPipeline(dev, g->pipeline, nil);
		break;
	case GARBAGE_SHADERMODULE:
		vkDestroyShaderModule(dev, g->module, nil);
		break;
	}
}

static void
defer(const Garbage *g)
{
	if(!gpuBusy()){
		Garbage copy = *g;
		destroyNow(&copy);
		return;
	}
	if(numGarbage == maxGarbage){
		int32 n = maxGarbage ? maxGarbage*2 : 256;
		Garbage *a = rwNewT(Garbage, n, MEMDUR_EVENT | ID_DRIVER);
		if(garbage){
			memcpy(a, garbage, numGarbage*sizeof(Garbage));
			rwFree(garbage);
		}
		garbage = a;
		maxGarbage = n;
	}
	garbage[numGarbage++] = *g;
}

void
deferDestroyBuffer(VkBuffer buffer, VmaAllocation allocation)
{
	Garbage g;
	memset(&g, 0, sizeof(g));
	g.type = GARBAGE_BUFFER;
	g.buffer = buffer;
	g.allocation = allocation;
	defer(&g);
}

void
deferDestroyPipeline(VkPipeline pipeline)
{
	Garbage g;
	memset(&g, 0, sizeof(g));
	g.type = GARBAGE_PIPELINE;
	g.pipeline = pipeline;
	defer(&g);
}

void
deferDestroyShaderModule(VkShaderModule module)
{
	Garbage g;
	memset(&g, 0, sizeof(g));
	g.type = GARBAGE_SHADERMODULE;
	g.module = module;
	defer(&g);
}

// Called with nothing in flight: when a frame opens, after waiting for the last.
void
collectGarbage(void)
{
	for(int32 i = 0; i < numGarbage; i++)
		destroyNow(&garbage[i]);
	numGarbage = 0;
	if(!gpuBusy()){
		rwFree(garbage);
		garbage = nil;
		maxGarbage = 0;
	}
}

// --- images -----------------------------------------------------------------

static bool32
isDepthFormat(VkFormat fmt)
{
	switch(fmt){
	case VK_FORMAT_D16_UNORM:
	case VK_FORMAT_D32_SFLOAT:
	case VK_FORMAT_D16_UNORM_S8_UINT:
	case VK_FORMAT_D24_UNORM_S8_UINT:
	case VK_FORMAT_D32_SFLOAT_S8_UINT:
		return 1;
	default:
		return 0;
	}
}

static bool32
hasStencil(VkFormat fmt)
{
	return fmt == VK_FORMAT_D16_UNORM_S8_UINT || fmt == VK_FORMAT_D24_UNORM_S8_UINT ||
	       fmt == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

static Image *liveImages;

Image*
createImage(int32 width, int32 height, int32 levels, VkFormat format,
	VkSampleCountFlagBits samples, VkImageUsageFlags usage, bool32 opaqueAlpha)
{
	if(vkGlobals.device == VK_NULL_HANDLE)
		return nil;
	VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	ci.imageType = VK_IMAGE_TYPE_2D;
	ci.format = format;
	ci.extent.width = width;
	ci.extent.height = height;
	ci.extent.depth = 1;
	ci.mipLevels = levels;
	ci.arrayLayers = 1;
	ci.samples = samples;
	ci.tiling = VK_IMAGE_TILING_OPTIMAL;
	ci.usage = usage;
	ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VmaAllocationCreateInfo ai = {};
	ai.usage = VMA_MEMORY_USAGE_AUTO;

	Image *img = rwNewT(Image, 1, MEMDUR_EVENT | ID_DRIVER);
	memset(img, 0, sizeof(*img));
	if(vmaCreateImage(vkGlobals.allocator, &ci, &ai, &img->image, &img->allocation, nil) != VK_SUCCESS){
		rwFree(img);
		return nil;
	}
	img->format = format;
	img->layout = VK_IMAGE_LAYOUT_UNDEFINED;
	img->width = width;
	img->height = height;
	img->levels = levels;
	img->samples = samples;
	img->depth = isDepthFormat(format);

	VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	vi.image = img->image;
	vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vi.format = format;
	vi.subresourceRange.aspectMask = img->depth ?
		(VK_IMAGE_ASPECT_DEPTH_BIT | (hasStencil(format) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)) :
		VK_IMAGE_ASPECT_COLOR_BIT;
	vi.subresourceRange.levelCount = levels;
	vi.subresourceRange.layerCount = 1;
	vkCreateImageView(vkGlobals.device, &vi, nil, &img->view);
	img->sampleView = img->view;
	if(opaqueAlpha && (usage & VK_IMAGE_USAGE_SAMPLED_BIT)){
		vi.components.a = VK_COMPONENT_SWIZZLE_ONE;
		vkCreateImageView(vkGlobals.device, &vi, nil, &img->sampleView);
	}
	img->next = liveImages;
	if(liveImages)
		liveImages->prev = img;
	liveImages = img;
	vkGlobals.numTextures++;
	return img;
}

void
destroyImage(Image *img)
{
	if(img == nil)
		return;
	if(img->image != VK_NULL_HANDLE){
		forgetImage(img);
		Garbage g;
		memset(&g, 0, sizeof(g));
		g.type = GARBAGE_IMAGE;
		g.image = img->image;
		g.allocation = img->allocation;
		g.views[0] = img->view;
		g.views[1] = img->sampleView != img->view ? img->sampleView : VK_NULL_HANDLE;
		defer(&g);
		if(img->prev)
			img->prev->next = img->next;
		else
			liveImages = img->next;
		if(img->next)
			img->next->prev = img->prev;
		vkGlobals.numTextures--;
	}
	rwFree(img);
}

static void
layoutAccess(VkImageLayout layout, VkAccessFlags *access, VkPipelineStageFlags *stage)
{
	switch(layout){
	case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
		*access = VK_ACCESS_TRANSFER_WRITE_BIT;
		*stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		break;
	case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
		*access = VK_ACCESS_TRANSFER_READ_BIT;
		*stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		break;
	case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
		*access = VK_ACCESS_SHADER_READ_BIT;
		*stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		break;
	case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
		*access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		*stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		break;
	case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
		*access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		*stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		break;
	case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
		*access = 0;
		*stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
		break;
	default:
		*access = 0;
		*stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		break;
	}
}

void
transitionImage(VkCommandBuffer cmd, Image *img, VkImageLayout layout)
{
	if(img->layout == layout)
		return;
	VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	VkPipelineStageFlags srcStage, dstStage;
	layoutAccess(img->layout, &b.srcAccessMask, &srcStage);
	layoutAccess(layout, &b.dstAccessMask, &dstStage);
	b.oldLayout = img->layout;
	b.newLayout = layout;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = img->image;
	b.subresourceRange.aspectMask = img->depth ?
		(VK_IMAGE_ASPECT_DEPTH_BIT | (hasStencil(img->format) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)) :
		VK_IMAGE_ASPECT_COLOR_BIT;
	b.subresourceRange.levelCount = img->levels;
	b.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nil, 0, nil, 1, &b);
	img->layout = layout;
}

// --- vertex and index buffers -----------------------------------------------

// A buffer plus the system copy callers lock against, as D3D11's.
//
// A static buffer lives in device memory, and an unlock stages the written
// range into it from the upload command buffer. A dynamic one has no buffer of
// its own at all: every unlock copies it into the arena, so each primitive the
// immediate mode draws in a frame keeps its own vertices. Writing one place
// over and over would hand every draw recorded so far the last primitive's,
// because nothing runs until the frame is submitted.
struct Buffer
{
	VkBuffer buffer;
	VmaAllocation allocation;
	uint8 *sys;
	uint32 length;
	uint32 lockOffset;
	uint32 lockSize;
	bool dynamic;

	// Dynamic only: where the bytes up to `written` are this frame.
	VkBuffer arenaBuffer;
	VkDeviceSize arenaOffset;
	uint32 arenaSerial;
	uint32 written;

	Buffer *prev;
	Buffer *next;
};

static Buffer *liveBuffers;

static void*
createBuffer(uint32 length, bool dynamic, VkBufferUsageFlags usage)
{
	Buffer *b = rwNewT(Buffer, 1, MEMDUR_EVENT | ID_DRIVER);
	memset(b, 0, sizeof(*b));
	b->length = length;
	b->dynamic = dynamic;
	b->sys = rwNewT(uint8, length, MEMDUR_EVENT | ID_DRIVER);
	memset(b->sys, 0, length);
	if(dynamic)
		return b;
	if(vkGlobals.device == VK_NULL_HANDLE){
		rwFree(b->sys);
		rwFree(b);
		return nil;
	}

	VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bi.size = length;
	bi.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VmaAllocationCreateInfo ai = {};
	ai.usage = VMA_MEMORY_USAGE_AUTO;
	if(vmaCreateBuffer(vkGlobals.allocator, &bi, &ai, &b->buffer, &b->allocation, nil) != VK_SUCCESS){
		rwFree(b->sys);
		rwFree(b);
		return nil;
	}
	b->next = liveBuffers;
	if(liveBuffers)
		liveBuffers->prev = b;
	liveBuffers = b;
	return b;
}

static void
destroyBuffer(void *buffer)
{
	Buffer *b = (Buffer*)buffer;
	forgetBuffer(buffer);
	if(b->buffer != VK_NULL_HANDLE){
		deferDestroyBuffer(b->buffer, b->allocation);
		if(b->prev)
			b->prev->next = b->next;
		else
			liveBuffers = b->next;
		if(b->next)
			b->next->prev = b->prev;
	}
	rwFree(b->sys);
	rwFree(b);
}

static bool32
copyToArena(Buffer *b)
{
	ArenaSpan span;
	if(b->written == 0 || !arenaAlloc(b->written, 4, &span))
		return 0;
	memcpy(span.data, b->sys, b->written);
	b->arenaBuffer = span.buffer;
	b->arenaOffset = span.offset;
	b->arenaSerial = frameSerial();
	return 1;
}

uint8*
lockBufferVk(void *buffer, uint32 offset, uint32 size)
{
	Buffer *b = (Buffer*)buffer;
	if(size == 0)
		size = b->length - offset;
	b->lockOffset = offset;
	b->lockSize = size;
	return b->sys + offset;
}

void
unlockBufferVk(void *buffer)
{
	Buffer *b = (Buffer*)buffer;
	if(b == nil || b->lockSize == 0)
		return;

	if(b->dynamic){
		b->written = b->lockOffset + b->lockSize;
		copyToArena(b);
	}else{
		ArenaSpan span;
		if(arenaAlloc(b->lockSize, 4, &span)){
			memcpy(span.data, b->sys + b->lockOffset, b->lockSize);
			VkBufferCopy copy;
			copy.srcOffset = span.offset;
			copy.dstOffset = b->lockOffset;
			copy.size = b->lockSize;
			vkCmdCopyBuffer(uploadCommands(), span.buffer, b->buffer, 1, &copy);
		}
	}
	b->lockSize = 0;
}

bool32
bufferBinding(void *buffer, VkBuffer *vkbuf, VkDeviceSize *offset)
{
	Buffer *b = (Buffer*)buffer;
	if(b == nil)
		return 0;
	if(!b->dynamic){
		*vkbuf = b->buffer;
		*offset = 0;
		return 1;
	}
	// Written in a frame that has since been submitted -- a readback flushed
	// it between the unlock and the draw. The arena it was copied into has
	// been handed out again.
	if(b->arenaSerial != frameSerial() && !copyToArena(b))
		return 0;
	*vkbuf = b->arenaBuffer;
	*offset = b->arenaOffset;
	return 1;
}

void*
createVertexBufferVk(uint32 length, bool dynamic)
{
	void *b = createBuffer(length, dynamic, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
	if(b)
		vkGlobals.numVertexBuffers++;
	return b;
}

void
destroyVertexBufferVk(void *buffer)
{
	if(buffer){
		destroyBuffer(buffer);
		vkGlobals.numVertexBuffers--;
	}
}

void*
createIndexBufferVk(uint32 length, bool dynamic)
{
	void *b = createBuffer(length, dynamic, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
	if(b)
		vkGlobals.numIndexBuffers++;
	return b;
}

void
destroyIndexBufferVk(void *buffer)
{
	if(buffer){
		destroyBuffer(buffer);
		vkGlobals.numIndexBuffers--;
	}
}

// --- textures ---------------------------------------------------------------

static bool
isBlockCompressed(uint32 format)
{
	return format == D3DFMT_DXT1 || format == D3DFMT_DXT2 || format == D3DFMT_DXT3 ||
	       format == D3DFMT_DXT4 || format == D3DFMT_DXT5;
}

// D3DFMT_ is what the whole d3d driver speaks, so it stays the currency here and
// is translated where an image is made. Everything with no 8888 counterpart is
// widened on the way up rather than mapped: D3D11 does the same for fewer
// formats, and the game's own textures are all 32-bit.
static VkFormat
formatToVk(uint32 format)
{
	switch(format){
	case D3DFMT_A8R8G8B8:
	case D3DFMT_X8R8G8B8:	return VK_FORMAT_B8G8R8A8_UNORM;
	case D3DFMT_A8B8G8R8:	return VK_FORMAT_R8G8B8A8_UNORM;
	case D3DFMT_DXT1:	return vkGlobals.bcTextures ? VK_FORMAT_BC1_RGBA_UNORM_BLOCK : VK_FORMAT_UNDEFINED;
	case D3DFMT_DXT2:
	case D3DFMT_DXT3:	return vkGlobals.bcTextures ? VK_FORMAT_BC2_UNORM_BLOCK : VK_FORMAT_UNDEFINED;
	case D3DFMT_DXT4:
	case D3DFMT_DXT5:	return vkGlobals.bcTextures ? VK_FORMAT_BC3_UNORM_BLOCK : VK_FORMAT_UNDEFINED;
	}
	return VK_FORMAT_UNDEFINED;
}

static bool32
widensToBGRA(uint32 format)
{
	switch(format){
	case D3DFMT_L8:
	case D3DFMT_A8L8:
	case D3DFMT_A8:
	case D3DFMT_R5G6B5:
	case D3DFMT_X1R5G5B5:
	case D3DFMT_A1R5G5B5:
	case D3DFMT_A4R4G4B4:
		return 1;
	}
	return 0;
}

// One level's texels as B8G8R8A8, the uint32 written here on a little-endian
// machine.
static void
widenLevel(uint32 format, const uint8 *src, uint32 srcPitch,
	int32 width, int32 height, uint32 *dst)
{
	for(int32 y = 0; y < height; y++){
		const uint8 *row = src + y*srcPitch;
		for(int32 x = 0; x < width; x++){
			uint32 r, g, b, a;
			uint32 v = 0;
			if(format != D3DFMT_L8 && format != D3DFMT_A8)
				v = row[x*2] | (row[x*2+1] << 8);
			switch(format){
			case D3DFMT_L8:
				r = g = b = row[x];
				a = 0xFF;
				break;
			case D3DFMT_A8:
				r = g = b = 0;
				a = row[x];
				break;
			case D3DFMT_A8L8:
				r = g = b = row[x*2];
				a = row[x*2+1];
				break;
			case D3DFMT_R5G6B5:
				r = (v >> 11) & 0x1F; r = (r << 3) | (r >> 2);
				g = (v >> 5) & 0x3F;  g = (g << 2) | (g >> 4);
				b = v & 0x1F;         b = (b << 3) | (b >> 2);
				a = 0xFF;
				break;
			case D3DFMT_A4R4G4B4:
				a = ((v >> 12) & 0xF) * 0x11;
				r = ((v >> 8) & 0xF) * 0x11;
				g = ((v >> 4) & 0xF) * 0x11;
				b = (v & 0xF) * 0x11;
				break;
			default:	// X1R5G5B5 and A1R5G5B5
				r = (v >> 10) & 0x1F; r = (r << 3) | (r >> 2);
				g = (v >> 5) & 0x1F;  g = (g << 3) | (g >> 2);
				b = v & 0x1F;         b = (b << 3) | (b >> 2);
				a = format == D3DFMT_A1R5G5B5 && !(v & 0x8000) ? 0 : 0xFF;
				break;
			}
			dst[y*width + x] = (a << 24) | (r << 16) | (g << 8) | b;
		}
	}
}

static uint32
levelRowPitch(uint32 format, RasterLevels::Level *level)
{
	int32 rows = isBlockCompressed(format) ? (level->height + 3)/4 : level->height;
	if(rows <= 0)
		rows = 1;
	return (uint32)(level->size / rows);
}

static void
uploadRaster(Raster *raster, D3dRaster *natras)
{
	RasterLevels *levels = (RasterLevels*)natras->texture;
	if(levels == nil)
		return;
	bool32 widen = widensToBGRA(natras->format);
	VkFormat fmt = widen ? VK_FORMAT_B8G8R8A8_UNORM : formatToVk(natras->format);
	if(fmt == VK_FORMAT_UNDEFINED){
		static bool32 said;
		if(!said){
			fprintf(stderr, "librw: Vulkan has no format for D3D format %u; drawn untextured\n",
				natras->format);
			said = 1;
		}
		return;
	}

	Image *img = (Image*)natras->vk;
	if(img == nil){
		img = createImage(levels->levels[0].width, levels->levels[0].height, levels->numlevels,
			fmt, VK_SAMPLE_COUNT_1_BIT,
			VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			natras->format == D3DFMT_X8R8G8B8);
		if(img == nil)
			return;
		natras->vk = img;
	}

	VkCommandBuffer cmd = uploadCommands();
	transitionImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	for(int32 i = 0; i < levels->numlevels && i < img->levels; i++){
		RasterLevels::Level *level = &levels->levels[i];
		uint32 bytes = widen ? level->width*level->height*4 : level->size;
		ArenaSpan span;
		if(!arenaAlloc(bytes, 4, &span))
			break;
		if(widen)
			widenLevel(natras->format, level->data, levelRowPitch(natras->format, level),
				level->width, level->height, (uint32*)span.data);
		else
			memcpy(span.data, level->data, bytes);

		VkBufferImageCopy copy;
		memset(&copy, 0, sizeof(copy));
		copy.bufferOffset = span.offset;
		copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copy.imageSubresource.mipLevel = i;
		copy.imageSubresource.layerCount = 1;
		copy.imageExtent.width = level->width;
		copy.imageExtent.height = level->height;
		copy.imageExtent.depth = 1;
		vkCmdCopyBufferToImage(cmd, span.buffer, img->image,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	}
	// Only the upload buffer ever moves a texture, so this is the layout the
	// frame's own commands find it in.
	transitionImage(cmd, img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

Image*
rasterImage(Raster *raster)
{
	D3dRaster *natras = GETD3DRASTEREXT(raster);
	if(natras->dirty){
		natras->dirty = 0;
		uploadRaster(raster, natras);
	}
	return (Image*)natras->vk;
}

static Image *white;

void
createWhiteTexture(void)
{
	white = createImage(1, 1, 1, VK_FORMAT_B8G8R8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0);
	if(white == nil)
		return;
	ArenaSpan span;
	if(!arenaAlloc(4, 4, &span))
		return;
	memset(span.data, 0xFF, 4);
	VkCommandBuffer cmd = uploadCommands();
	transitionImage(cmd, white, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VkBufferImageCopy copy;
	memset(&copy, 0, sizeof(copy));
	copy.bufferOffset = span.offset;
	copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copy.imageSubresource.layerCount = 1;
	copy.imageExtent.width = 1;
	copy.imageExtent.height = 1;
	copy.imageExtent.depth = 1;
	vkCmdCopyBufferToImage(cmd, span.buffer, white->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	transitionImage(cmd, white, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void
destroyWhiteTexture(void)
{
	destroyImage(white);
	white = nil;
}

Image *whiteTexture(void) { return white; }

// --- camera targets and z buffers -------------------------------------------

Raster*
rasterCreateCameraTexture(Raster *raster)
{
	D3dRaster *natras = GETD3DRASTEREXT(raster);
	VkFormat fmt = formatToVk(natras->format);
	if(fmt == VK_FORMAT_UNDEFINED || isBlockCompressed(natras->format)){
		RWERROR((ERR_NOTEXTURE));
		return nil;
	}
	Image *img = createImage(raster->width, raster->height, 1, fmt, VK_SAMPLE_COUNT_1_BIT,
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
		VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		natras->format == D3DFMT_X8R8G8B8);
	if(img == nil){
		RWERROR((ERR_NOTEXTURE));
		return nil;
	}
	natras->vk = img;
	// No system copy: a camera texture is written by the GPU, and reading it
	// back goes through rasterLockTarget.
	natras->texture = nil;
	return raster;
}

Raster*
rasterCreateCamera(Raster *raster)
{
	D3dRaster *natras = GETD3DRASTEREXT(raster);
	natras->autogenMipmap = 0;
	natras->format = D3DFMT_A8R8G8B8;
	raster->depth = 32;
	// nil means the scene target, the convention both D3D backends use.
	natras->texture = nil;
	return raster;
}

Raster*
rasterCreateZbuffer(Raster *raster)
{
	D3dRaster *natras = GETD3DRASTEREXT(raster);
	natras->autogenMipmap = 0;
	natras->format = D3DFMT_D24S8;
	raster->depth = 32;
	Image *img = createImage(raster->width, raster->height, 1, vkGlobals.depthFormat,
		VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0);
	if(img == nil){
		RWERROR((ERR_NOTEXTURE));
		return nil;
	}
	natras->vk = img;
	natras->texture = nil;
	return raster;
}

Image *sceneResolvedImage(void);

// A render target read back. Everything recorded so far is submitted and waited
// for, so the copy holds what has been drawn up to this call -- which is what a
// D3D lock of the same surface gives.
uint8*
rasterLockTarget(Raster *raster, int32 level, int32 lockMode)
{
	(void)level;
	D3dRaster *natras = GETD3DRASTEREXT(raster);
	if(lockMode & Raster::LOCKWRITE){
		assert(0 && "can't lock a render target for writing");
		return nil;
	}

	Image *src = raster->type == Raster::CAMERA ? sceneResolvedImage() : (Image*)natras->vk;
	if(src == nil || src->depth)
		return nil;

	uint32 bytes = src->width*src->height*4;
	VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bi.size = bytes;
	bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VmaAllocationCreateInfo ai = {};
	ai.usage = VMA_MEMORY_USAGE_AUTO;
	ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VkBuffer readback;
	VmaAllocation alloc;
	VmaAllocationInfo info;
	if(vmaCreateBuffer(vkGlobals.allocator, &bi, &ai, &readback, &alloc, &info) != VK_SUCCESS)
		return nil;

	VkCommandBuffer cmd = frameCommands();
	endRendering();
	transitionImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	VkBufferImageCopy copy;
	memset(&copy, 0, sizeof(copy));
	copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copy.imageSubresource.layerCount = 1;
	copy.imageExtent.width = src->width;
	copy.imageExtent.height = src->height;
	copy.imageExtent.depth = 1;
	vkCmdCopyImageToBuffer(cmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
	flushFrame();

	uint8 *pixels = (uint8*)rwNew(bytes, MEMDUR_EVENT | ID_DRIVER);
	memcpy(pixels, info.pMappedData, bytes);
	vmaDestroyBuffer(vkGlobals.allocator, readback, alloc);

	// An X format's alpha is whatever the GPU left there. D3D11's staging copy
	// reads it through the X format as one.
	if(natras->format == D3DFMT_X8R8G8B8)
		for(uint32 i = 3; i < bytes; i += 4)
			pixels[i] = 0xFF;

	natras->lockedSurf = pixels;
	raster->pixels = pixels;
	raster->width = src->width;
	raster->height = src->height;
	raster->stride = src->width*4;
	return pixels;
}

void
rasterUnlockTarget(Raster *raster)
{
	D3dRaster *natras = GETD3DRASTEREXT(raster);
	rwFree(natras->lockedSurf);
	natras->lockedSurf = nil;
}

void
releaseAllResources(void)
{
	for(Image *img = liveImages; img; img = img->next){
		if(img->sampleView != img->view)
			vkDestroyImageView(vkGlobals.device, img->sampleView, nil);
		vkDestroyImageView(vkGlobals.device, img->view, nil);
		vmaDestroyImage(vkGlobals.allocator, img->image, img->allocation);
		img->image = VK_NULL_HANDLE;
		img->view = img->sampleView = VK_NULL_HANDLE;
		vkGlobals.numTextures--;
	}
	liveImages = nil;
	for(Buffer *b = liveBuffers; b; b = b->next){
		vmaDestroyBuffer(vkGlobals.allocator, b->buffer, b->allocation);
		b->buffer = VK_NULL_HANDLE;
	}
	liveBuffers = nil;
}

void
rasterDestroy(Raster *raster, D3dRaster *natras)
{
	forgetRaster(raster);
	destroyImage((Image*)natras->vk);
	natras->vk = nil;
}

#endif
}
}
}
