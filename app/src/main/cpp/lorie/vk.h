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
    X(GetPhysicalDeviceMemoryProperties) X(CreateDevice) X(GetDeviceProcAddr)
#define LORIE_VK_DEVICE_FNS(X) X(DestroyDevice) X(GetDeviceQueue) X(DeviceWaitIdle)

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

// libvulkan.so's vkGetInstanceProcAddr, or NULL with the fallback recorded (VK_LOADER).
PFN_vkGetInstanceProcAddr lorieVkLoader(volatile struct lorie_shared_server_state *st);
// Instance and device, through `gipa`. On failure nothing is left behind and the fallback is recorded.
bool lorieVkCreateDevice(struct lorie_vk *vk, PFN_vkGetInstanceProcAddr gipa, volatile struct lorie_shared_server_state *st);
void lorieVkDestroyDevice(struct lorie_vk *vk);
// Records one fallback: its class (LORIE_FALLBACK_CLASS_*), reason (LORIE_FALLBACK_*) and the VkResult that
// caused it (VK_SUCCESS when no call failed, as for an injected one).
void lorieVkFallback(volatile struct lorie_shared_server_state *st, uint8_t cls, uint8_t reason, VkResult result);
// Whether TERMUX_X11_RENDERER_VK_FAIL_AT names `point` (LORIE_VK_FAIL_AT_*); if so, the fallback is recorded
// as INJECTED with `reason`, and the caller must not make the call.
bool lorieVkInjected(volatile struct lorie_shared_server_state *st, uint8_t point, uint8_t reason);
