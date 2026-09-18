// The one translation unit that compiles VMA's implementation.

#include "../rwbase.h"

#ifdef RW_VULKAN
#include <volk.h>
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
#endif
