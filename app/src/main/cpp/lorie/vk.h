#pragma once
/*
 * The renderer's Vulkan backend (the activity's process only). libvulkan.so is opened at run time, so the app
 * still starts where there is none, and every entry point is resolved through vkGetInstanceProcAddr and
 * vkGetDeviceProcAddr into struct lorie_vk.
 *
 * What it needs, all of it or nothing: a Vulkan 1.1 loader and device; VK_KHR_surface and
 * VK_KHR_android_surface; a graphics queue; VK_KHR_swapchain, VK_ANDROID_external_memory_android_hardware_buffer
 * and VK_EXT_queue_family_foreign (the X server's AHardwareBuffers are taken over from, and given back to,
 * the foreign queue family - there is no fallback to VK_QUEUE_FAMILY_EXTERNAL). Every failure is recorded in
 * the shared diagnostics with its class and reason (lorieVkFallback) before the caller falls back.
 */
#define VK_NO_PROTOTYPES
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR 1
#endif
#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <stdint.h>

struct lorie_shared_server_state;

#define LORIE_VK_GLOBAL_FNS(X) X(EnumerateInstanceVersion) X(EnumerateInstanceExtensionProperties) X(CreateInstance)
#define LORIE_VK_INSTANCE_FNS(X) \
    X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) \
    X(GetPhysicalDeviceQueueFamilyProperties) X(EnumerateDeviceExtensionProperties) \
    X(GetPhysicalDeviceMemoryProperties) X(CreateDevice) X(GetDeviceProcAddr) X(GetPhysicalDeviceImageFormatProperties2)
#define LORIE_VK_DEVICE_FNS(X) X(DestroyDevice) X(GetDeviceQueue) X(DeviceWaitIdle) \
    X(GetAndroidHardwareBufferPropertiesANDROID) X(CreateImage) X(DestroyImage) X(AllocateMemory) X(FreeMemory) \
    X(BindImageMemory) X(CreateImageView) X(DestroyImageView)

struct lorie_vk {
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
#define LORIE_VK_FN(n) PFN_vk ## n n;
    LORIE_VK_GLOBAL_FNS(LORIE_VK_FN)
    LORIE_VK_INSTANCE_FNS(LORIE_VK_FN)
    LORIE_VK_DEVICE_FNS(LORIE_VK_FN)
#undef LORIE_VK_FN
    VkInstance instance;
    VkPhysicalDevice physicalDevice;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceMemoryProperties memoryProperties;
    uint32_t queueFamily;
    VkDevice device;
    VkQueue queue;
};

/*
 * An X server AHardwareBuffer as a Vulkan image: the renderer's own record of it, keyed by the LorieBuffer's id
 * (LorieBuffer itself carries no Vulkan handle - it travels over the socket as raw bytes). The memory import
 * holds its own reference to the AHardwareBuffer until the image is removed.
 *
 * Sampling goes through sampleView: for an RGBX buffer its alpha reads as one, as GLES samples RGBX. Rendering
 * goes through attachView (identity, as a colour attachment's view must be), present only if the buffer
 * allows rendering to it. The data is in the X server's byte order whatever the buffer's declared format, so
 * the swizzle rule is GLES's: rgbaOrder says the declared format is not BGRA (LorieBuffer_isRgba).
 *
 * The image is created with initialLayout VK_IMAGE_LAYOUT_UNDEFINED, as the specification requires of an image
 * with external memory. How it is taken over from, and given back to, the X server (ownership and layouts) is
 * not decided here.
 */
struct lorie_vk_image {
    uint64_t id;
    VkImage image;
    VkDeviceMemory memory;
    VkImageView sampleView, attachView;
    VkFormat format;
    VkImageUsageFlags usage;
    uint32_t width, height;
    uint32_t ahbFormat;
    uint64_t ahbUsage;
    bool rgbaOrder, rgbx;
};

struct lorie_vk_images {
    struct lorie_vk_image *items;
    uint32_t count, capacity;
};

// libvulkan.so's vkGetInstanceProcAddr, or NULL with the fallback recorded (VK_LOADER).
PFN_vkGetInstanceProcAddr lorieVkLoader(volatile struct lorie_shared_server_state *st);
// Instance and device, through `gipa`. On failure nothing is left behind and the fallback is recorded.
bool lorieVkCreateDevice(struct lorie_vk *vk, PFN_vkGetInstanceProcAddr gipa, volatile struct lorie_shared_server_state *st);
void lorieVkDestroyDevice(struct lorie_vk *vk);
// Records one fallback: its class (LORIE_FALLBACK_CLASS_*), reason (LORIE_FALLBACK_*) and the VkResult that
// caused it (VK_SUCCESS when no call failed, as for an injected one).
void lorieVkFallback(volatile struct lorie_shared_server_state *st, uint8_t cls, uint8_t reason, VkResult result);
// The image usages the renderer can have for an AHB of this usage: sampled where the AHB is GPU-sampleable,
// colour attachment where it allows rendering into it.
VkImageUsageFlags lorieVkAhbImageUsage(uint64_t ahbUsage);
// The image of `ahb` (LorieBuffer id `id`): the one already made, or a new import with lorieVkAhbImageUsage
// plus `extraUsage`. NULL, with the fallback recorded as RESOURCE_LOCAL and the buffer described, when it cannot
// be imported; importing must stay possible for every other buffer.
struct lorie_vk_image *lorieVkImport(struct lorie_vk *vk, struct lorie_vk_images *images, uint64_t id,
                                     struct AHardwareBuffer *ahb, VkImageUsageFlags extraUsage,
                                     volatile struct lorie_shared_server_state *st);
struct lorie_vk_image *lorieVkImageFind(struct lorie_vk_images *images, uint64_t id);
// Destroys the image of `id`, if there is one. No submitted work may still use it.
void lorieVkImageRemove(struct lorie_vk *vk, struct lorie_vk_images *images, uint64_t id);
void lorieVkImagesClear(struct lorie_vk *vk, struct lorie_vk_images *images);
// Whether TERMUX_X11_RENDERER_VK_FAIL_AT names `point` (LORIE_VK_FAIL_AT_*); if so, the fallback is recorded
// as INJECTED with `reason`, and the caller must not make the call.
bool lorieVkInjected(volatile struct lorie_shared_server_state *st, uint8_t point, uint8_t reason);
