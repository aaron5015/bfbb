// The libraries the Vulkan backend's own files are written against. Included
// before rwd3dvk.h, whose internal half is keyed on VOLK_H_.
//
// volk loads the Vulkan loader at run time and stands in for its prototypes, so
// the build needs headers and no import library -- which is what lets a machine
// without the Vulkan SDK build this. VMA is pointed at volk's function table
// rather than at the loader's exports for the same reason.

#include <volk.h>

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
