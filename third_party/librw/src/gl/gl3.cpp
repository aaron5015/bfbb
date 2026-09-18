#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "../rwrender.h"

#include "rwgl3.h"
#include "rwgl3shader.h"

#include "rwgl3impl.h"

namespace rw {
namespace gl3 {

// TODO: make some of these things platform-independent

static void*
driverOpen(void *o, int32, int32)
{
	// Only the platform that is RUNNING. Engine::start constructs every
	// platform's driver plugin, and a build may carry several backends -- so
	// without this a D3D9 run would reach GL3's driverOpen and compile its
	// shaders with no GL context, and a GL3 run would reach D3D's with no
	// device. What is skipped is left as the null driver Engine::open
	// installed, which asserts if anything asks a dead backend to do work.
	if(rw::platform != PLATFORM_GL3)
		return o;

#ifdef RW_OPENGL
	engine->driver[PLATFORM_GL3]->defaultPipeline = makeDefaultPipeline();
	uvTransformPipelines[PLATFORM_GL3] = makeUVTransformPipeline();
#endif
	engine->driver[PLATFORM_GL3]->rasterNativeOffset = nativeRasterOffset;
	engine->driver[PLATFORM_GL3]->rasterCreate       = rasterCreate;
	engine->driver[PLATFORM_GL3]->rasterLock         = rasterLock;
	engine->driver[PLATFORM_GL3]->rasterUnlock       = rasterUnlock;
	engine->driver[PLATFORM_GL3]->rasterNumLevels    = rasterNumLevels;
	engine->driver[PLATFORM_GL3]->imageFindRasterFormat = imageFindRasterFormat;
	engine->driver[PLATFORM_GL3]->rasterFromImage    = rasterFromImage;
	engine->driver[PLATFORM_GL3]->rasterToImage      = rasterToImage;

	return o;
}

static void*
driverClose(void *o, int32, int32)
{
	// See driverOpen.
	if(rw::platform != PLATFORM_GL3)
		return o;

#ifdef RW_OPENGL
	if(uvTransformPipelines[PLATFORM_GL3]){
		uvTransformPipelines[PLATFORM_GL3]->destroy();
		uvTransformPipelines[PLATFORM_GL3] = nil;
	}
#endif
	return o;
}

void
registerPlatformPlugins(void)
{
	Driver::registerPlugin(PLATFORM_GL3, 0, PLATFORM_GL3,
	                       driverOpen, driverClose);
	registerNativeRaster();
}

}
}
