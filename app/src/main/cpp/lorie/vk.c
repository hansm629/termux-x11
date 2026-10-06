#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include "lorie.h"
#include "vk.h"

#define log(...) __android_log_print(ANDROID_LOG_DEBUG, "lorie-vulkan", __VA_ARGS__)
#define loge(...) __android_log_print(ANDROID_LOG_ERROR, "lorie-vulkan", __VA_ARGS__)

static const char *const lorieVkInstanceExtensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME };
static const char *const lorieVkDeviceExtensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME };
// The reason a device without lorieVkDeviceExtensions[i] is unusable for.
static const uint8_t lorieVkDeviceExtensionReasons[] = {
        LORIE_FALLBACK_VK_EXT_SWAPCHAIN, LORIE_FALLBACK_VK_EXT_AHB, LORIE_FALLBACK_VK_EXT_FOREIGN };
#define LORIE_VK_MAX_DEVICES 8

void lorieVkFallback(volatile struct lorie_shared_server_state *st, uint8_t cls, uint8_t reason, VkResult result) {
    loge("Vulkan fallback: class %u reason %u VkResult %d", cls, reason, result);
    if (!st)
        return;
    LORIE_DIAG_ADD(st, fallbackTotal, 1);
    LORIE_DIAG_ADD(st, fallbackByClass[cls], 1);
    LORIE_DIAG_ADD(st, fallbackByReason[reason], 1);
    st->diag.lastFallbackClass = cls;
    st->diag.lastFallbackReason = reason;
    st->diag.lastVkResult = result;
}

bool lorieVkInjected(volatile struct lorie_shared_server_state *st, uint8_t point, uint8_t reason) {
    if (!st || st->backend.vkFailAt != point)
        return false;
    lorieVkFallback(st, LORIE_FALLBACK_CLASS_INJECTED, reason, VK_SUCCESS);
    return true;
}

PFN_vkGetInstanceProcAddr lorieVkLoader(volatile struct lorie_shared_server_state *st) {
    static void *lib = NULL;
    PFN_vkGetInstanceProcAddr gipa;

    if (!lib && !(lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL))) {
        loge("dlopen libvulkan.so: %s", dlerror());
        lorieVkFallback(st, LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_LOADER, VK_ERROR_INITIALIZATION_FAILED);
        return NULL;
    }
    if (!(gipa = (PFN_vkGetInstanceProcAddr) dlsym(lib, "vkGetInstanceProcAddr")))
        lorieVkFallback(st, LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_LOADER, VK_ERROR_INITIALIZATION_FAILED);
    return gipa;
}

static bool lorieVkHasExtension(const VkExtensionProperties *ext, uint32_t count, const char *name) {
    uint32_t i;
    for (i = 0; i < count; i++)
        if (!strcmp(ext[i].extensionName, name))
            return true;
    return false;
}

// LORIE_FALLBACK_NONE if the backend can use `pd`, with the graphics queue family it would use; otherwise
// the first reason it cannot.
static uint8_t lorieVkDeviceUnsuitable(struct lorie_vk *vk, VkPhysicalDevice pd, uint32_t *queueFamily) {
    VkPhysicalDeviceProperties props;
    VkQueueFamilyProperties families[16];
    VkExtensionProperties *ext = NULL;
    uint32_t count = 16, extCount = 0, i;
    uint8_t reason = LORIE_FALLBACK_NONE;

    vk->GetPhysicalDeviceProperties(pd, &props);
    if (props.apiVersion < VK_API_VERSION_1_1)
        return LORIE_FALLBACK_VK_VERSION;

    vk->GetPhysicalDeviceQueueFamilyProperties(pd, &count, families);
    for (i = 0; i < count && !(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT); i++);
    if (i == count)
        return LORIE_FALLBACK_VK_NO_DEVICE;
    *queueFamily = i;

    if (vk->EnumerateDeviceExtensionProperties(pd, NULL, &extCount, NULL) != VK_SUCCESS ||
        !(ext = calloc(extCount ?: 1, sizeof(*ext))) ||
        vk->EnumerateDeviceExtensionProperties(pd, NULL, &extCount, ext) != VK_SUCCESS)
        extCount = 0;
    for (i = 0; i < sizeof(lorieVkDeviceExtensions) / sizeof(*lorieVkDeviceExtensions) && !reason; i++)
        if (!lorieVkHasExtension(ext, extCount, lorieVkDeviceExtensions[i]))
            reason = lorieVkDeviceExtensionReasons[i];
    free(ext);
    return reason;
}

void lorieVkDestroyDevice(struct lorie_vk *vk) {
    if (vk->device) {
        vk->DeviceWaitIdle(vk->device);
        vk->DestroyDevice(vk->device, NULL);
    }
    if (vk->instance && vk->DestroyInstance)   // without it (a broken loader) the instance is left to the process' end
        vk->DestroyInstance(vk->instance, NULL);
    memset(vk, 0, sizeof(*vk));
}

bool lorieVkCreateDevice(struct lorie_vk *vk, PFN_vkGetInstanceProcAddr gipa, volatile struct lorie_shared_server_state *st) {
    VkPhysicalDevice devices[LORIE_VK_MAX_DEVICES];
    VkExtensionProperties *ext = NULL;
    uint32_t version = VK_API_VERSION_1_0, count = 0, i, family = 0;
    uint8_t reason = LORIE_FALLBACK_NONE, firstReason = LORIE_FALLBACK_NONE;
    bool missing = false;
    float priority = 1.f;
    VkResult r;

    memset(vk, 0, sizeof(*vk));
    vk->GetInstanceProcAddr = gipa;
#define LORIE_VK_LOAD(n) vk->n = (PFN_vk ## n) gipa(NULL, "vk" #n);
    LORIE_VK_GLOBAL_FNS(LORIE_VK_LOAD)
#undef LORIE_VK_LOAD
    if (!vk->EnumerateInstanceExtensionProperties || !vk->CreateInstance) {
        lorieVkFallback(st, LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_LOADER, VK_ERROR_INITIALIZATION_FAILED);
        goto fail;
    }

    // Vulkan 1.1 (Android 9): before it there is no vkEnumerateInstanceVersion.
    if (vk->EnumerateInstanceVersion)
        vk->EnumerateInstanceVersion(&version);
    if (version < VK_API_VERSION_1_1) {
        lorieVkFallback(st, LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_VERSION, VK_ERROR_INCOMPATIBLE_DRIVER);
        goto fail;
    }

    if (vk->EnumerateInstanceExtensionProperties(NULL, &count, NULL) != VK_SUCCESS ||
        !(ext = calloc(count ?: 1, sizeof(*ext))) ||
        vk->EnumerateInstanceExtensionProperties(NULL, &count, ext) != VK_SUCCESS)
        count = 0;
    for (i = 0; i < sizeof(lorieVkInstanceExtensions) / sizeof(*lorieVkInstanceExtensions); i++)
        if (!lorieVkHasExtension(ext, count, lorieVkInstanceExtensions[i]))
            reason = LORIE_FALLBACK_VK_EXT_ANDROID_SURFACE;
    free(ext);
    if (reason) {
        lorieVkFallback(st, LORIE_FALLBACK_CLASS_FATAL_BACKEND, reason, VK_ERROR_EXTENSION_NOT_PRESENT);
        goto fail;
    }

    if (lorieVkInjected(st, LORIE_VK_FAIL_AT_INSTANCE, LORIE_FALLBACK_VK_INSTANCE))
        goto fail;
    {
        VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "Termux:X11",
                                  .apiVersion = VK_API_VERSION_1_1 };
        VkInstanceCreateInfo info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                      .enabledExtensionCount = sizeof(lorieVkInstanceExtensions) / sizeof(*lorieVkInstanceExtensions),
                                      .ppEnabledExtensionNames = lorieVkInstanceExtensions };
        if ((r = vk->CreateInstance(&info, NULL, &vk->instance)) != VK_SUCCESS) {
            vk->instance = VK_NULL_HANDLE;
            lorieVkFallback(st, LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_INSTANCE, r);
            goto fail;
        }
    }
    LORIE_DIAG_ADD(st, vkInstanceCreated, 1);

#define LORIE_VK_LOAD(n) vk->n = (PFN_vk ## n) gipa(vk->instance, "vk" #n); if (!vk->n) missing = true;
    LORIE_VK_INSTANCE_FNS(LORIE_VK_LOAD)
#undef LORIE_VK_LOAD
    if (missing) {
        lorieVkFallback(st, LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_LOADER, VK_ERROR_INITIALIZATION_FAILED);
        goto fail;
    }

    count = LORIE_VK_MAX_DEVICES;
    if ((r = vk->EnumeratePhysicalDevices(vk->instance, &count, devices)) < 0 || !count) {
        lorieVkFallback(st, LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_NO_DEVICE, r < 0 ? r : VK_ERROR_INITIALIZATION_FAILED);
        goto fail;
    }
    // The first device with everything; if there is none, the first one's reason.
    for (i = 0; i < count; i++) {
        if (!(reason = lorieVkDeviceUnsuitable(vk, devices[i], &family)))
            break;
        if (!firstReason)
            firstReason = reason;
    }
    if (i == count) {
        lorieVkFallback(st, LORIE_FALLBACK_CLASS_FATAL_BACKEND, firstReason, VK_ERROR_EXTENSION_NOT_PRESENT);
        goto fail;
    }
    vk->physicalDevice = devices[i];
    vk->queueFamily = family;
    vk->GetPhysicalDeviceProperties(vk->physicalDevice, &vk->properties);
    vk->GetPhysicalDeviceMemoryProperties(vk->physicalDevice, &vk->memoryProperties);

    if (lorieVkInjected(st, LORIE_VK_FAIL_AT_DEVICE, LORIE_FALLBACK_VK_DEVICE))
        goto fail;
    {
        VkDeviceQueueCreateInfo queue = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family,
                                          .queueCount = 1, .pQueuePriorities = &priority };
        VkDeviceCreateInfo info = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                                    .pQueueCreateInfos = &queue,
                                    .enabledExtensionCount = sizeof(lorieVkDeviceExtensions) / sizeof(*lorieVkDeviceExtensions),
                                    .ppEnabledExtensionNames = lorieVkDeviceExtensions };
        if ((r = vk->CreateDevice(vk->physicalDevice, &info, NULL, &vk->device)) != VK_SUCCESS) {
            vk->device = VK_NULL_HANDLE;
            lorieVkFallback(st, LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_DEVICE, r);
            goto fail;
        }
    }
    LORIE_DIAG_ADD(st, vkDeviceCreated, 1);

#define LORIE_VK_LOAD(n) vk->n = (PFN_vk ## n) vk->GetDeviceProcAddr(vk->device, "vk" #n); if (!vk->n) missing = true;
    LORIE_VK_DEVICE_FNS(LORIE_VK_LOAD)
#undef LORIE_VK_LOAD
    if (missing) {
        // Without vkDestroyDevice the device cannot be destroyed either: left to the process' end.
        vk->device = VK_NULL_HANDLE;
        lorieVkFallback(st, LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_LOADER, VK_ERROR_INITIALIZATION_FAILED);
        goto fail;
    }
    vk->GetDeviceQueue(vk->device, family, 0, &vk->queue);

    if (st) {
        for (i = 0; i + 1 < sizeof(st->diag.vkDevice) && vk->properties.deviceName[i]; i++)
            st->diag.vkDevice[i] = vk->properties.deviceName[i];
        st->diag.vkDevice[i] = 0;
        st->diag.vkDriverVersion = vk->properties.driverVersion;
        st->diag.vkApiVersion = vk->properties.apiVersion;
    }
    log("Vulkan device %s, API %u.%u, driver %#x, queue family %u", vk->properties.deviceName,
        VK_API_VERSION_MAJOR(vk->properties.apiVersion), VK_API_VERSION_MINOR(vk->properties.apiVersion),
        vk->properties.driverVersion, family);
    return true;

    fail:
    lorieVkDestroyDevice(vk);
    return false;
}
