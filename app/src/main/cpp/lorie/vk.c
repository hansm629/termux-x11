#include <dlfcn.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include "lorie.h"
#include "vk.h"
#include "vk_shaders.h"

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

// The global priority extension the device has, to be enabled: VK_KHR_global_priority, or before it the EXT one.
static const char *lorieVkGlobalPriority(struct lorie_vk *vk, VkPhysicalDevice pd) {
    VkExtensionProperties *ext = NULL;
    uint32_t count = 0;
    const char *name = NULL;
    if (vk->EnumerateDeviceExtensionProperties(pd, NULL, &count, NULL) == VK_SUCCESS && (ext = calloc(count ?: 1, sizeof(*ext))) &&
        vk->EnumerateDeviceExtensionProperties(pd, NULL, &count, ext) == VK_SUCCESS)
        name = lorieVkHasExtension(ext, count, "VK_KHR_global_priority") ? "VK_KHR_global_priority" :
               lorieVkHasExtension(ext, count, "VK_EXT_global_priority") ? "VK_EXT_global_priority" : NULL;
    free(ext);
    return name;
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
        // The queue at HIGH global priority where the device has the extension: a frame keeps the X server's buffers -
        // the root among them - off its CPU until the device finished it (lorie.h gpuInFlight), and what makes that
        // take long is waiting behind other processes' work on the device, a 3D client's. Refused (not permitted to
        // this process, VK_ERROR_NOT_PERMITTED_KHR, or otherwise): made again at the default priority, as before.
        const uint32_t n = sizeof(lorieVkDeviceExtensions) / sizeof(*lorieVkDeviceExtensions);
        const char *names[sizeof(lorieVkDeviceExtensions) / sizeof(*lorieVkDeviceExtensions) + 1];
        const char *globalPriority = lorieVkGlobalPriority(vk, vk->physicalDevice);
        VkDeviceQueueGlobalPriorityCreateInfoEXT high = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_EXT,
                                                          .globalPriority = VK_QUEUE_GLOBAL_PRIORITY_HIGH_EXT };
        VkDeviceQueueCreateInfo queue = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family,
                                          .queueCount = 1, .pQueuePriorities = &priority };
        VkDeviceCreateInfo info = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                                    .pQueueCreateInfos = &queue, .enabledExtensionCount = n, .ppEnabledExtensionNames = names };
        memcpy(names, lorieVkDeviceExtensions, sizeof(lorieVkDeviceExtensions));
        if (globalPriority) {
            names[info.enabledExtensionCount++] = globalPriority;
            queue.pNext = &high;
            if ((r = vk->CreateDevice(vk->physicalDevice, &info, NULL, &vk->device)) != VK_SUCCESS) {
                loge("Vulkan: HIGH queue priority refused (%d), the default one taken", r);
                vk->device = VK_NULL_HANDLE;
                globalPriority = NULL;
                info.enabledExtensionCount = n;
                queue.pNext = NULL;
            }
        }
        if (!globalPriority && (r = vk->CreateDevice(vk->physicalDevice, &info, NULL, &vk->device)) != VK_SUCCESS) {
            vk->device = VK_NULL_HANDLE;
            lorieVkFallback(st, LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_DEVICE, r);
            goto fail;
        }
        vk->highPriority = globalPriority != NULL;
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
        st->diag.vkHighPriority = vk->highPriority;
    }
    log("Vulkan device %s, API %u.%u, driver %#x, queue family %u", vk->properties.deviceName,
        VK_API_VERSION_MAJOR(vk->properties.apiVersion), VK_API_VERSION_MINOR(vk->properties.apiVersion),
        vk->properties.driverVersion, family);
    return true;

    fail:
    lorieVkDestroyDevice(vk);
    return false;
}

VkImageUsageFlags lorieVkAhbImageUsage(uint64_t ahbUsage) {
    VkImageUsageFlags usage = 0;
    if (ahbUsage & AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE)
        usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (ahbUsage & AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER)
        usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    return usage;
}

struct lorie_vk_image *lorieVkImageFind(struct lorie_vk_images *images, uint64_t id) {
    uint32_t i;
    for (i = 0; i < images->count; i++)
        if (images->items[i]->id == id)
            return images->items[i];
    return NULL;
}

static void lorieVkImageDestroy(struct lorie_vk *vk, struct lorie_vk_image *img) {
    if (img->framebuffer)
        vk->DestroyFramebuffer(vk->device, img->framebuffer, NULL);
    if (img->attachView)
        vk->DestroyImageView(vk->device, img->attachView, NULL);
    if (img->sampleView)
        vk->DestroyImageView(vk->device, img->sampleView, NULL);
    if (img->image)
        vk->DestroyImage(vk->device, img->image, NULL);
    if (img->memory)
        vk->FreeMemory(vk->device, img->memory, NULL);
    memset(img, 0, sizeof(*img));
}

// A buffer that cannot be imported: the fallback (RESOURCE_LOCAL unless injected), and which buffer it was.
static void lorieVkImportFailed(struct lorie_vk_images *images, volatile struct lorie_shared_server_state *st,
                                uint8_t reason, VkResult result, uint64_t id, const AHardwareBuffer_Desc *desc) {
    loge("cannot import AHardwareBuffer %llu (%ux%u format %u usage %#llx): reason %u", (unsigned long long) id,
         desc->width, desc->height, desc->format, (unsigned long long) desc->usage, reason);
    if (images) {
        images->lastReason = reason ? reason : LORIE_FALLBACK_AHB_IMPORT;
        images->lastResult = result;
    }
    if (!st)
        return;
    st->diag.lastResourceId = id;
    st->diag.lastResourceUsage = desc->usage;
    st->diag.lastResourceFormat = desc->format;
    if (reason)
        lorieVkFallback(st, LORIE_FALLBACK_CLASS_RESOURCE_LOCAL, reason, result);
}

struct lorie_vk_image *lorieVkImport(struct lorie_vk *vk, struct lorie_vk_images *images, uint64_t id,
                                     struct AHardwareBuffer *ahb, VkImageUsageFlags extraUsage,
                                     volatile struct lorie_shared_server_state *st) {
    struct lorie_vk_image *found = lorieVkImageFind(images, id), img = {0};
    AHardwareBuffer_Desc desc = {0};
    VkAndroidHardwareBufferFormatPropertiesANDROID formatProps = {
            .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID };
    VkAndroidHardwareBufferPropertiesANDROID props = {
            .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID, .pNext = &formatProps };
    VkPhysicalDeviceExternalImageFormatInfo externalInfo = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID };
    VkExternalImageFormatProperties externalProps = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
    VkImageFormatProperties2 formatOut = { .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &externalProps };
    uint32_t i, memoryType = UINT32_MAX;
    uint8_t reason = LORIE_FALLBACK_NONE;
    VkResult r = VK_SUCCESS;

    if (found)
        return found;
    if (!ahb) {
        lorieVkImportFailed(images, st, LORIE_FALLBACK_AHB_PROPERTIES, VK_ERROR_INVALID_EXTERNAL_HANDLE, id, &desc);
        return NULL;
    }
    LORIE_DIAG_ADD(st, vkAhbImportAttempts, 1);
    AHardwareBuffer_describe(ahb, &desc);
    if (lorieVkInjected(st, LORIE_VK_FAIL_AT_IMPORT, LORIE_FALLBACK_AHB_IMPORT)) {
        lorieVkImportFailed(images, st, LORIE_FALLBACK_NONE, VK_SUCCESS, id, &desc);
        return NULL;
    }

    img.id = id;
    img.width = desc.width;
    img.height = desc.height;
    img.ahbFormat = desc.format;
    img.ahbUsage = desc.usage;
    img.rgbaOrder = desc.format != AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM;
    img.rgbx = desc.format == AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM;
    img.usage = lorieVkAhbImageUsage(desc.usage) | extraUsage;
    if (!(img.usage & VK_IMAGE_USAGE_SAMPLED_BIT) || desc.layers != 1) {
        reason = LORIE_FALLBACK_AHB_IMAGE_USAGE_UNSUPPORTED;
        r = VK_ERROR_FORMAT_NOT_SUPPORTED;
        goto fail;
    }

    if ((r = vk->GetAndroidHardwareBufferPropertiesANDROID(vk->device, ahb, &props)) != VK_SUCCESS) {
        reason = LORIE_FALLBACK_AHB_PROPERTIES;
        goto fail;
    }
    if (formatProps.format == VK_FORMAT_UNDEFINED) {
        reason = LORIE_FALLBACK_AHB_EXTERNAL_FORMAT_UNSUPPORTED_BY_STAGE1;
        r = VK_ERROR_FORMAT_NOT_SUPPORTED;
        goto fail;
    }
    if (formatProps.format != VK_FORMAT_R8G8B8A8_UNORM && formatProps.format != VK_FORMAT_B8G8R8A8_UNORM) {
        reason = LORIE_FALLBACK_AHB_FORMAT_UNSUPPORTED_BY_STAGE1;
        r = VK_ERROR_FORMAT_NOT_SUPPORTED;
        goto fail;
    }
    img.format = formatProps.format;
    // Only formats the ownership self-test passed on this device (BGRA may have been unavailable to it).
    if (!(vk->verifiedFormats & (desc.format == AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM ? 1 :
                                 desc.format == AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM ? 2 :
                                 desc.format == AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM ? 4 : 0))) {
        reason = LORIE_FALLBACK_AHB_FORMAT_UNSUPPORTED_BY_STAGE1;
        r = VK_ERROR_FORMAT_NOT_SUPPORTED;
        goto fail;
    }

    // The image exactly as it will be made, asked about as an import of an AHardwareBuffer.
    {
        VkPhysicalDeviceImageFormatInfo2 info = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, .pNext = &externalInfo,
                .format = img.format, .type = VK_IMAGE_TYPE_2D, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = img.usage };
        r = vk->GetPhysicalDeviceImageFormatProperties2(vk->physicalDevice, &info, &formatOut);
        if (r != VK_SUCCESS ||
            !(externalProps.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ||
            formatOut.imageFormatProperties.maxExtent.width < desc.width ||
            formatOut.imageFormatProperties.maxExtent.height < desc.height) {
            reason = LORIE_FALLBACK_AHB_IMAGE_USAGE_UNSUPPORTED;
            r = r != VK_SUCCESS ? r : VK_ERROR_FORMAT_NOT_SUPPORTED;
            goto fail;
        }
    }

    // Any memory type the buffer allows, device-local first.
    for (i = 0; i < vk->memoryProperties.memoryTypeCount; i++)
        if ((props.memoryTypeBits & (1u << i)) && (memoryType == UINT32_MAX ||
            (!(vk->memoryProperties.memoryTypes[memoryType].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
             (vk->memoryProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))))
            memoryType = i;
    if (memoryType == UINT32_MAX) {
        reason = LORIE_FALLBACK_AHB_MEMORY_TYPE;
        r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        goto fail;
    }

    {
        VkExternalMemoryImageCreateInfo external = {
                .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
                .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID };
        VkImageCreateInfo info = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &external, .imageType = VK_IMAGE_TYPE_2D,
                .format = img.format, .extent = { desc.width, desc.height, 1 }, .mipLevels = 1, .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = img.usage,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        if ((r = vk->CreateImage(vk->device, &info, NULL, &img.image)) != VK_SUCCESS) {
            img.image = VK_NULL_HANDLE;
            reason = LORIE_FALLBACK_AHB_IMPORT;
            goto fail;
        }
    }
    {
        VkMemoryDedicatedAllocateInfo dedicated = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = img.image };
        VkImportAndroidHardwareBufferInfoANDROID import = {
                .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, .pNext = &dedicated, .buffer = ahb };
        VkMemoryAllocateInfo info = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &import,
                .allocationSize = props.allocationSize, .memoryTypeIndex = memoryType };
        if ((r = vk->AllocateMemory(vk->device, &info, NULL, &img.memory)) != VK_SUCCESS) {
            img.memory = VK_NULL_HANDLE;
            reason = LORIE_FALLBACK_AHB_IMPORT;
            goto fail;
        }
    }
    if ((r = vk->BindImageMemory(vk->device, img.image, img.memory, 0)) != VK_SUCCESS) {
        reason = LORIE_FALLBACK_AHB_IMPORT;
        goto fail;
    }
    {
        VkImageViewCreateInfo info = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img.image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = img.format,
                .components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                img.rgbx ? VK_COMPONENT_SWIZZLE_ONE : VK_COMPONENT_SWIZZLE_IDENTITY },
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        if ((r = vk->CreateImageView(vk->device, &info, NULL, &img.sampleView)) != VK_SUCCESS) {
            img.sampleView = VK_NULL_HANDLE;
            reason = LORIE_FALLBACK_AHB_IMPORT;
            goto fail;
        }
        info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        if ((img.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) &&
            (r = vk->CreateImageView(vk->device, &info, NULL, &img.attachView)) != VK_SUCCESS) {
            img.attachView = VK_NULL_HANDLE;
            reason = LORIE_FALLBACK_AHB_IMPORT;
            goto fail;
        }
    }

    if (images->count == images->capacity) {
        uint32_t capacity = images->capacity ? images->capacity * 2 : 16;
        struct lorie_vk_image **items = realloc(images->items, capacity * sizeof(*items));
        if (!items) {
            reason = LORIE_FALLBACK_AHB_IMPORT;
            r = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto fail;
        }
        images->items = items;
        images->capacity = capacity;
    }
    if (!(found = malloc(sizeof(*found)))) {
        reason = LORIE_FALLBACK_AHB_IMPORT;
        r = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto fail;
    }
    *found = img;
    images->items[images->count++] = found;
    LORIE_DIAG_ADD(st, vkAhbImportSuccess, 1);
    return found;

    fail:
    lorieVkImageDestroy(vk, &img);
    lorieVkImportFailed(images, st, reason, r, id, &desc);
    return NULL;
}

void lorieVkImageRemove(struct lorie_vk *vk, struct lorie_vk_images *images, uint64_t id) {
    uint32_t i;
    for (i = 0; i < images->count; i++)
        if (images->items[i]->id == id) {
            lorieVkImageDestroy(vk, images->items[i]);
            free(images->items[i]);
            images->items[i] = images->items[--images->count];
            return;
        }
}

void lorieVkImagesClear(struct lorie_vk *vk, struct lorie_vk_images *images) {
    while (images->count)
        lorieVkImageRemove(vk, images, images->items[0]->id);
    free(images->items);
    memset(images, 0, sizeof(*images));
}

// The layout, pipeline stage and accesses of each use, and those of its accesses that write.
static void lorieVkUseScope(enum lorie_vk_use use, VkImageLayout *layout, VkPipelineStageFlags *stage,
                            VkAccessFlags *access, VkAccessFlags *writes) {
    switch (use) {
        case LORIE_VK_USE_SAMPLE:
            *layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            *stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            *access = VK_ACCESS_SHADER_READ_BIT;
            *writes = 0;
            return;
        case LORIE_VK_USE_RENDER:
            *layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            *stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            *access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            *writes = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            return;
        case LORIE_VK_USE_TRANSFER_SRC:
            *layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            *stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            *access = VK_ACCESS_TRANSFER_READ_BIT;
            *writes = 0;
            return;
        case LORIE_VK_USE_TRANSFER_DST:
        default:
            *layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            *stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            *access = VK_ACCESS_TRANSFER_WRITE_BIT;
            *writes = VK_ACCESS_TRANSFER_WRITE_BIT;
            return;
    }
}

static struct lorie_vk_barrier lorieVkBarrier(VkImage image) {
    return (struct lorie_vk_barrier) { .barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .image = image,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } } };
}

/*
 * An acquire operation: its source access mask is ignored, and should be 0, and the first synchronization scope
 * does not apply to it (Queue Family Ownership Transfer); the foreign release it follows - AHardwareBuffer_unlock
 * by the X server - happened before this submission, which orders it (Host Write Ordering Guarantees). The
 * layout transition from GENERAL happens between the two, and the image is then visible to `use`.
 */
struct lorie_vk_barrier lorieVkAcquireBarrier(const struct lorie_vk *vk, VkImage image, enum lorie_vk_use use) {
    struct lorie_vk_barrier b = lorieVkBarrier(image);
    VkAccessFlags writes;
    lorieVkUseScope(use, &b.barrier.newLayout, &b.dstStage, &b.barrier.dstAccessMask, &writes);
    b.barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.barrier.srcAccessMask = 0;
    b.barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    b.barrier.dstQueueFamilyIndex = vk->queueFamily;
    b.srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    return b;
}

/*
 * A release operation: it happens after the availability operation, so the writes of `use` are made available
 * through the source access mask; its destination access mask is ignored, and should be 0, and the second
 * synchronization scope does not apply to it. The caller waits for the submission's fence before the X server
 * may lock the buffer.
 */
struct lorie_vk_barrier lorieVkReleaseBarrier(const struct lorie_vk *vk, VkImage image, enum lorie_vk_use use) {
    struct lorie_vk_barrier b = lorieVkBarrier(image);
    VkAccessFlags access;
    lorieVkUseScope(use, &b.barrier.oldLayout, &b.srcStage, &access, &b.barrier.srcAccessMask);
    b.barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.barrier.dstAccessMask = 0;
    b.barrier.srcQueueFamilyIndex = vk->queueFamily;
    b.barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    b.dstStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    return b;
}

struct lorie_vk_barrier lorieVkUseBarrier(VkImage image, enum lorie_vk_use from, enum lorie_vk_use to) {
    struct lorie_vk_barrier b = lorieVkBarrier(image);
    VkAccessFlags access, writes;
    lorieVkUseScope(from, &b.barrier.oldLayout, &b.srcStage, &access, &b.barrier.srcAccessMask);
    lorieVkUseScope(to, &b.barrier.newLayout, &b.dstStage, &b.barrier.dstAccessMask, &writes);
    return b;
}

#define LORIE_VK_SELFTEST_WIDTH 67      // a width no row alignment divides: the buffers' rows have padding
#define LORIE_VK_SELFTEST_HEIGHT 45
#define LORIE_VK_SELFTEST_TIMEOUT_NS 1000000000ULL

// alphaDefined: whether the 4th byte is compared (not for RGBX, see vk.h)
static const struct { uint32_t format; uint8_t bit; bool required, alphaDefined; } lorieVkSelftestFormats[] = {
        { AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, 1, true, true },
        { AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, 2, true, false },
        { AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM, 4, false, true },
};

struct lorie_vk_selftest {
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkBuffer buffer[2];         // [0] uploaded from, [1] read back into
    VkDeviceMemory memory;
    uint8_t *mapped;
    bool coherent;
    VkDeviceSize size, stride;  // one buffer's size; buffer[1] starts at `stride`
    struct lorie_vk_images images;
    AHardwareBuffer *ahb[2];
    uint8_t *expect, *write;
};

// Iteration `it`'s pattern of kind `kind`: zero, all-one, checkerboard, row-dependent or pseudo-random, tightly
// packed, 4 bytes a pixel.
static void lorieVkSelftestPattern(uint8_t *out, uint32_t kind, uint32_t it) {
    uint32_t x, y, k, r = it * 2654435761u + 0x9e3779b9u;
    for (y = 0; y < LORIE_VK_SELFTEST_HEIGHT; y++)
        for (x = 0; x < LORIE_VK_SELFTEST_WIDTH; x++)
            for (k = 0; k < 4; k++) {
                uint8_t v;
                switch (kind % 5) {
                    case 0: v = 0; break;
                    case 1: v = 0xff; break;
                    case 2: v = (((x >> 2) ^ (y >> 2) ^ it) & 1) ? (uint8_t) (0x12 + k * 0x22) : (uint8_t) (0xed - k * 0x22); break;
                    case 3: v = (uint8_t) (y * 7 + x * 3 + k * 61 + it); break;
                    default: r ^= r << 13; r ^= r >> 17; r ^= r << 5; v = (uint8_t) r; break;
                }
                out[(y * LORIE_VK_SELFTEST_WIDTH + x) * 4 + k] = v;
            }
}

// The first byte of `got` (rows `rowBytes` apart) that differs from tightly packed `expect`, or -1; each pixel's
// 4th byte only if `alphaDefined`.
static int64_t lorieVkSelftestDiff(const uint8_t *got, size_t rowBytes, const uint8_t *expect, bool alphaDefined) {
    uint32_t y, i;
    const size_t w = LORIE_VK_SELFTEST_WIDTH * 4;
    for (y = 0; y < LORIE_VK_SELFTEST_HEIGHT; y++)
        if (memcmp(got + y * rowBytes, expect + y * w, w))
            for (i = 0; i < w; i++)
                if ((alphaDefined || i % 4 != 3) && got[y * rowBytes + i] != expect[y * w + i])
                    return (int64_t) (y * w + i);
    return -1;
}

// The CPU's turn, as the X server takes it: lock, compare with `expect` if given, write `write` if given, unlock.
static uint8_t lorieVkSelftestCpu(AHardwareBuffer *b, const uint8_t *expect, const uint8_t *write, bool alphaDefined,
                                  int64_t *diff, uint8_t *got) {
    AHardwareBuffer_Desc d;
    uint8_t *p = NULL;
    uint32_t y;

    AHardwareBuffer_describe(b, &d);
    if (AHardwareBuffer_lock(b, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, (void **) &p) || !p)
        return LORIE_VK_SELFTEST_STEP_CPU_LOCK;
    if (expect && (*diff = lorieVkSelftestDiff(p, (size_t) d.stride * 4, expect, alphaDefined)) >= 0) {
        uint32_t row = (uint32_t) (*diff / (LORIE_VK_SELFTEST_WIDTH * 4)), col = (uint32_t) (*diff % (LORIE_VK_SELFTEST_WIDTH * 4));
        *got = p[(size_t) row * d.stride * 4 + col];
        AHardwareBuffer_unlock(b, NULL);
        return LORIE_VK_SELFTEST_STEP_COMPARE;
    }
    if (write)
        for (y = 0; y < LORIE_VK_SELFTEST_HEIGHT; y++)
            memcpy(p + (size_t) y * d.stride * 4, write + (size_t) y * LORIE_VK_SELFTEST_WIDTH * 4, LORIE_VK_SELFTEST_WIDTH * 4);
    return AHardwareBuffer_unlock(b, NULL) ? LORIE_VK_SELFTEST_STEP_CPU_LOCK : LORIE_VK_SELFTEST_STEP_NONE;
}

static void lorieVkCmdBarrier(struct lorie_vk *vk, VkCommandBuffer cmd, struct lorie_vk_barrier b) {
    vk->CmdPipelineBarrier(cmd, b.srcStage, b.dstStage, 0, 0, NULL, 0, NULL, 1, &b.barrier);
}

static VkResult lorieVkSelftestBegin(struct lorie_vk *vk, struct lorie_vk_selftest *t) {
    VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VkResult r = vk->ResetCommandBuffer(t->cmd, 0);
    return r != VK_SUCCESS ? r : vk->BeginCommandBuffer(t->cmd, &begin);
}

// Ends, submits and waits for the command buffer: its releases are complete when this returns VK_SUCCESS.
static VkResult lorieVkSelftestSubmit(struct lorie_vk *vk, struct lorie_vk_selftest *t, uint8_t *step) {
    VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &t->cmd };
    VkResult r;
    *step = LORIE_VK_SELFTEST_STEP_SUBMIT;
    if ((r = vk->EndCommandBuffer(t->cmd)) != VK_SUCCESS || (r = vk->QueueSubmit(vk->queue, 1, &submit, t->fence)) != VK_SUCCESS)
        return r;
    *step = LORIE_VK_SELFTEST_STEP_FENCE;
    if ((r = vk->WaitForFences(vk->device, 1, &t->fence, VK_TRUE, LORIE_VK_SELFTEST_TIMEOUT_NS)) != VK_SUCCESS)
        return r;
    if ((r = vk->ResetFences(vk->device, 1, &t->fence)) != VK_SUCCESS)
        return r;
    *step = LORIE_VK_SELFTEST_STEP_NONE;
    return VK_SUCCESS;
}

static VkResult lorieVkSelftestSetup(struct lorie_vk *vk, struct lorie_vk_selftest *t) {
    VkCommandPoolCreateInfo poolInfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                         .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = vk->queueFamily };
    VkFenceCreateInfo fenceInfo = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkBufferCreateInfo bufferInfo = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                      .size = (VkDeviceSize) LORIE_VK_SELFTEST_WIDTH * LORIE_VK_SELFTEST_HEIGHT * 4,
                                      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkMemoryRequirements req;
    uint32_t i, type = UINT32_MAX;
    VkResult r;

    if ((r = vk->CreateCommandPool(vk->device, &poolInfo, NULL, &t->pool)) != VK_SUCCESS)
        return t->pool = VK_NULL_HANDLE, r;
    {
        VkCommandBufferAllocateInfo info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = t->pool,
                                             .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        if ((r = vk->AllocateCommandBuffers(vk->device, &info, &t->cmd)) != VK_SUCCESS)
            return r;
    }
    if ((r = vk->CreateFence(vk->device, &fenceInfo, NULL, &t->fence)) != VK_SUCCESS)
        return t->fence = VK_NULL_HANDLE, r;
    for (i = 0; i < 2; i++)
        if ((r = vk->CreateBuffer(vk->device, &bufferInfo, NULL, &t->buffer[i])) != VK_SUCCESS)
            return t->buffer[i] = VK_NULL_HANDLE, r;
    vk->GetBufferMemoryRequirements(vk->device, t->buffer[0], &req);
    t->size = bufferInfo.size;
    t->stride = (req.size + req.alignment - 1) / req.alignment * req.alignment;
    // Host-visible, coherent if there is one.
    for (i = 0; i < vk->memoryProperties.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = vk->memoryProperties.memoryTypes[i].propertyFlags;
        if (!(req.memoryTypeBits & (1u << i)) || !(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            continue;
        if (type == UINT32_MAX || (!(vk->memoryProperties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) &&
                                   (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)))
            type = i;
    }
    if (type == UINT32_MAX)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    t->coherent = vk->memoryProperties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    {
        VkMemoryAllocateInfo info = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = t->stride * 2,
                                      .memoryTypeIndex = type };
        if ((r = vk->AllocateMemory(vk->device, &info, NULL, &t->memory)) != VK_SUCCESS)
            return t->memory = VK_NULL_HANDLE, r;
    }
    if ((r = vk->BindBufferMemory(vk->device, t->buffer[0], t->memory, 0)) != VK_SUCCESS ||
        (r = vk->BindBufferMemory(vk->device, t->buffer[1], t->memory, t->stride)) != VK_SUCCESS ||
        (r = vk->MapMemory(vk->device, t->memory, 0, VK_WHOLE_SIZE, 0, (void **) &t->mapped)) != VK_SUCCESS)
        return r;
    if (!(t->expect = malloc(t->size)) || !(t->write = malloc(t->size)))
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    return VK_SUCCESS;
}

static void lorieVkSelftestFreeBuffers(struct lorie_vk *vk, struct lorie_vk_selftest *t) {
    int i;
    lorieVkImagesClear(vk, &t->images);
    for (i = 0; i < 2; i++)
        if (t->ahb[i]) {
            AHardwareBuffer_release(t->ahb[i]);
            t->ahb[i] = NULL;
        }
}

static void lorieVkSelftestCleanup(struct lorie_vk *vk, struct lorie_vk_selftest *t) {
    int i;
    vk->DeviceWaitIdle(vk->device);     // after a failure, work may still be pending on what is freed below
    lorieVkSelftestFreeBuffers(vk, t);
    if (t->mapped)
        vk->UnmapMemory(vk->device, t->memory);
    for (i = 0; i < 2; i++)
        if (t->buffer[i])
            vk->DestroyBuffer(vk->device, t->buffer[i], NULL);
    if (t->memory)
        vk->FreeMemory(vk->device, t->memory, NULL);
    if (t->fence)
        vk->DestroyFence(vk->device, t->fence, NULL);
    if (t->pool)
        vk->DestroyCommandPool(vk->device, t->pool, NULL);   // frees its command buffer
    free(t->expect);
    free(t->write);
    memset(t, 0, sizeof(*t));
}

// Non-coherent memory made visible to the device (after host writes) or to the host (after device writes); the
// whole of it, which keeps the range a multiple of nonCoherentAtomSize.
static VkResult lorieVkSelftestSyncHost(struct lorie_vk *vk, struct lorie_vk_selftest *t, bool toDevice) {
    VkMappedMemoryRange range = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = t->memory,
                                  .offset = 0, .size = VK_WHOLE_SIZE };
    if (t->coherent)
        return VK_SUCCESS;
    return toDevice ? vk->FlushMappedMemoryRanges(vk->device, 1, &range) : vk->InvalidateMappedMemoryRanges(vk->device, 1, &range);
}

// Microseconds from t0 to t1, both CLOCK_MONOTONIC: the difference in nanoseconds taken signed first, since t1's
// tv_nsec is below t0's whenever a second boundary lies between them (0 for a t1 before t0, never a wrapped value).
static uint64_t lorieVkElapsedUs(const struct timespec *t0, const struct timespec *t1) {
    int64_t ns = ((int64_t) t1->tv_sec - (int64_t) t0->tv_sec) * 1000000000 + ((int64_t) t1->tv_nsec - (int64_t) t0->tv_nsec);
    return ns > 0 ? (uint64_t) ns / 1000 : 0;
}

bool lorieVkSelftest(struct lorie_vk *vk, volatile struct lorie_shared_server_state *st, uint32_t iterations) {
    struct lorie_vk_selftest t = {0};
    struct timespec t0, t1;
    const VkImageSubresourceLayers layers = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    const VkExtent3D extent = { LORIE_VK_SELFTEST_WIDTH, LORIE_VK_SELFTEST_HEIGHT, 1 };
    uint8_t step = LORIE_VK_SELFTEST_STEP_NONE, direction = 0, expected = 0, got = 0, passed = 0, unavailable = 0;
    uint32_t f, it = 0, format = 0;
    int64_t diff = -1;
    VkResult r = VK_SUCCESS;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    vk->verifiedFormats = 7;    // its own buffers, to be verified; afterwards only the formats that passed
    if (st) {
        st->diag.vkSelftest = 0;
        st->diag.vkSelftestIterations = iterations;
    }
    if ((r = lorieVkSelftestSetup(vk, &t)) != VK_SUCCESS) {
        step = LORIE_VK_SELFTEST_STEP_SETUP;
        goto fail;
    }

    for (f = 0; f < sizeof(lorieVkSelftestFormats) / sizeof(*lorieVkSelftestFormats); f++) {
        AHardwareBuffer_Desc desc = { .width = LORIE_VK_SELFTEST_WIDTH, .height = LORIE_VK_SELFTEST_HEIGHT, .layers = 1,
                                      .format = lorieVkSelftestFormats[f].format,
                                      .usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                                               AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER };
        struct lorie_vk_image *img[2] = { NULL, NULL };
        int i;

        format = desc.format;
        direction = 0;      // where a failure is reported: not in a direction or an iteration before both start
        it = 0;
        for (i = 0; i < 2; i++) {
            if (AHardwareBuffer_allocate(&desc, &t.ahb[i]) || !t.ahb[i]) {
                t.ahb[i] = NULL;
                step = LORIE_VK_SELFTEST_STEP_ALLOCATE;
                break;
            }
            // Ids no LorieBuffer has (they count up from 1): this cache is the self-test's own anyway.
            if (!(img[i] = lorieVkImport(vk, &t.images, UINT64_MAX - i, t.ahb[i],
                                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, NULL))) {
                step = LORIE_VK_SELFTEST_STEP_IMPORT;
                r = t.images.lastResult;
                break;
            }
        }
        if (step) {
            // BGRA may be missing: not allocatable, or a format the backend does not import.
            uint8_t why = t.images.lastReason;
            if (!lorieVkSelftestFormats[f].required && (step == LORIE_VK_SELFTEST_STEP_ALLOCATE ||
                (why == LORIE_FALLBACK_AHB_EXTERNAL_FORMAT_UNSUPPORTED_BY_STAGE1 || why == LORIE_FALLBACK_AHB_FORMAT_UNSUPPORTED_BY_STAGE1 ||
                 why == LORIE_FALLBACK_AHB_IMAGE_USAGE_UNSUPPORTED))) {
                log("self-test: AHB format %u unavailable (step %u, reason %u)", format, step, why);
                unavailable |= lorieVkSelftestFormats[f].bit;
                step = LORIE_VK_SELFTEST_STEP_NONE;
                r = VK_SUCCESS;
                lorieVkSelftestFreeBuffers(vk, &t);
                continue;
            }
            goto fail;
        }

        // CPU -> Vulkan -> CPU
        direction = 1;
        for (it = 0; it < iterations; it++) {
            lorieVkSelftestPattern(t.expect, it % 5, it);
            if ((step = lorieVkSelftestCpu(t.ahb[0], NULL, t.expect, true, &diff, &got)))
                goto fail;
            if ((r = lorieVkSelftestBegin(vk, &t)) != VK_SUCCESS) {
                step = LORIE_VK_SELFTEST_STEP_SUBMIT;
                goto fail;
            }
            lorieVkCmdBarrier(vk, t.cmd, lorieVkAcquireBarrier(vk, img[0]->image, LORIE_VK_USE_TRANSFER_SRC));
            lorieVkCmdBarrier(vk, t.cmd, lorieVkAcquireBarrier(vk, img[1]->image, LORIE_VK_USE_TRANSFER_DST));
            vk->CmdCopyImage(t.cmd, img[0]->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img[1]->image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &(VkImageCopy) { layers, {0}, layers, {0}, extent });
            lorieVkCmdBarrier(vk, t.cmd, lorieVkReleaseBarrier(vk, img[0]->image, LORIE_VK_USE_TRANSFER_SRC));
            lorieVkCmdBarrier(vk, t.cmd, lorieVkReleaseBarrier(vk, img[1]->image, LORIE_VK_USE_TRANSFER_DST));
            if ((r = lorieVkSelftestSubmit(vk, &t, &step)) != VK_SUCCESS)
                goto fail;
            if ((step = lorieVkSelftestCpu(t.ahb[1], t.expect, NULL, lorieVkSelftestFormats[f].alphaDefined, &diff, &got)))
                goto fail;
            LORIE_DIAG_ADD(st, vkSelftestCompares, 1);
        }

        // Vulkan -> CPU -> Vulkan
        direction = 2;
        for (it = 0; it < iterations; it++) {
            lorieVkSelftestPattern(t.mapped, it % 5, it ^ 0x5a5a5a5au);
            memcpy(t.expect, t.mapped, t.size);
            if ((r = lorieVkSelftestSyncHost(vk, &t, true)) != VK_SUCCESS || (r = lorieVkSelftestBegin(vk, &t)) != VK_SUCCESS) {
                step = LORIE_VK_SELFTEST_STEP_SUBMIT;
                goto fail;
            }
            lorieVkCmdBarrier(vk, t.cmd, lorieVkAcquireBarrier(vk, img[0]->image, LORIE_VK_USE_TRANSFER_DST));
            vk->CmdCopyBufferToImage(t.cmd, t.buffer[0], img[0]->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                     &(VkBufferImageCopy) { 0, 0, 0, layers, {0}, extent });
            lorieVkCmdBarrier(vk, t.cmd, lorieVkReleaseBarrier(vk, img[0]->image, LORIE_VK_USE_TRANSFER_DST));
            if ((r = lorieVkSelftestSubmit(vk, &t, &step)) != VK_SUCCESS)
                goto fail;
            lorieVkSelftestPattern(t.write, (it + 2) % 5, it + 1);
            if ((step = lorieVkSelftestCpu(t.ahb[0], t.expect, t.write, lorieVkSelftestFormats[f].alphaDefined, &diff, &got)))
                goto fail;
            LORIE_DIAG_ADD(st, vkSelftestCompares, 1);

            if ((r = lorieVkSelftestBegin(vk, &t)) != VK_SUCCESS) {
                step = LORIE_VK_SELFTEST_STEP_SUBMIT;
                goto fail;
            }
            lorieVkCmdBarrier(vk, t.cmd, lorieVkAcquireBarrier(vk, img[0]->image, LORIE_VK_USE_TRANSFER_SRC));
            vk->CmdCopyImageToBuffer(t.cmd, img[0]->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.buffer[1], 1,
                                     &(VkBufferImageCopy) { 0, 0, 0, layers, {0}, extent });
            lorieVkCmdBarrier(vk, t.cmd, lorieVkReleaseBarrier(vk, img[0]->image, LORIE_VK_USE_TRANSFER_SRC));
            // A fence makes only device accesses part of its dependency: the copy is made visible to the host here.
            vk->CmdPipelineBarrier(t.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1,
                                   &(VkBufferMemoryBarrier) { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                                              .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                                              .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
                                                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                                              .buffer = t.buffer[1], .size = VK_WHOLE_SIZE }, 0, NULL);
            if ((r = lorieVkSelftestSubmit(vk, &t, &step)) != VK_SUCCESS)
                goto fail;
            if ((r = lorieVkSelftestSyncHost(vk, &t, false)) != VK_SUCCESS) {
                step = LORIE_VK_SELFTEST_STEP_FENCE;
                goto fail;
            }
            if ((diff = lorieVkSelftestDiff(t.mapped + t.stride, LORIE_VK_SELFTEST_WIDTH * 4, t.write,
                                            lorieVkSelftestFormats[f].alphaDefined)) >= 0) {
                got = t.mapped[t.stride + diff];
                memcpy(t.expect, t.write, t.size);
                step = LORIE_VK_SELFTEST_STEP_COMPARE;
                goto fail;
            }
            LORIE_DIAG_ADD(st, vkSelftestCompares, 1);
        }

        passed |= lorieVkSelftestFormats[f].bit;
        lorieVkSelftestFreeBuffers(vk, &t);
    }

    lorieVkSelftestCleanup(vk, &t);
    vk->verifiedFormats = passed;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (st) {
        st->diag.vkSelftestFormatsPassed = passed;
        st->diag.vkSelftestFormatsUnavailable = unavailable;
        st->diag.vkSelftestUs = lorieVkElapsedUs(&t0, &t1);
        st->diag.vkSelftest = 1;
    }
    log("ownership self-test passed: formats %#x, unavailable %#x, %u iterations each way", passed, unavailable, iterations);
    return true;

    fail:
    if (step == LORIE_VK_SELFTEST_STEP_COMPARE && diff >= 0)
        expected = t.expect[diff];
    loge("ownership self-test FAILED: step %u, AHB format %u, direction %u, iteration %u, byte %lld expected %#x got %#x, VkResult %d",
         step, format, direction, it, (long long) diff, expected, got, r);
    lorieVkSelftestCleanup(vk, &t);
    vk->verifiedFormats = 0;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (st) {
        st->diag.vkSelftestFormatsPassed = passed;
        st->diag.vkSelftestFormatsUnavailable = unavailable;
        st->diag.vkSelftestUs = lorieVkElapsedUs(&t0, &t1);
        st->diag.vkSelftestFailStep = step;
        st->diag.vkSelftestFailFormat = format;
        st->diag.vkSelftestFailDirection = direction;
        st->diag.vkSelftestFailIteration = it;
        st->diag.vkSelftestFailOffset = diff >= 0 ? (uint32_t) diff : 0;
        st->diag.vkSelftestFailExpected = expected;
        st->diag.vkSelftestFailActual = got;
        st->diag.vkSelftestFailResult = r;
        st->diag.vkSelftest = 2;
    }
    lorieVkFallback(st, LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_SELFTEST, r);
    return false;
}

/* ---- presenting ---- */

#define LORIE_VK_ACQUIRE_TIMEOUT_NS 50000000ULL     // 50 ms: the X server's lock is not held meanwhile
#define LORIE_VK_FENCE_TIMEOUT_NS 1000000000ULL     // 1 s, then the frame is a failure (and waited out)
#define LORIE_VK_CURSOR_BYTES (512 * 512 * 4)       // the shared state's cursor bits

static uint64_t lorieVkNowUs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000ULL + (uint64_t) ts.tv_nsec / 1000;
}

/*
 * A render pass with one colour attachment. Into an imported buffer: its contents loaded and stored, in
 * COLOR_ATTACHMENT_OPTIMAL from start to end (the frame's barriers put it there and take it back). Into a
 * swapchain image: cleared, so its previous contents are not needed - it is the renderer's own image, never an
 * X server buffer - and left for presenting.
 */
static VkRenderPass lorieVkRenderPass(struct lorie_vk *vk, VkFormat format, bool screen) {
    VkAttachmentDescription a = {
            .format = format, .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = screen ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = screen ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .finalLayout = screen ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1,
                                 .pColorAttachments = &ref };
    // The swapchain image's transition and writes after the earlier frame's (the acquire's fence was waited for).
    VkSubpassDependency dep = { VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0 };
    VkRenderPassCreateInfo info = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
                                    .pAttachments = &a, .subpassCount = 1, .pSubpasses = &sub,
                                    .dependencyCount = screen ? 1 : 0, .pDependencies = &dep };
    VkRenderPass pass = VK_NULL_HANDLE;
    return vk->CreateRenderPass(vk->device, &info, NULL, &pass) == VK_SUCCESS ? pass : VK_NULL_HANDLE;
}

/*
 * The quad pipeline (shaders/quad.*) for `pass`. Blending, for the cursor, is GLES's SRC_ALPHA,
 * ONE_MINUS_SRC_ALPHA on colour; alpha keeps the destination's (one: the screen stays opaque).
 */
static VkPipeline lorieVkPipeline(struct lorie_vk *vk, struct lorie_vk_renderer *r, VkRenderPass pass, bool blend) {
    VkPipelineShaderStageCreateInfo stages[2] = {
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = r->vert, .pName = "main" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = r->frag, .pName = "main" } };
    VkPipelineVertexInputStateCreateInfo vertexInput = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo assembly = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP };
    VkPipelineViewportStateCreateInfo viewport = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                   .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo raster = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                      .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.f };
    VkPipelineMultisampleStateCreateInfo multisample = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                         .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState attachment = {
            .blendEnable = blend, .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .colorBlendOp = VK_BLEND_OP_ADD, .srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .alphaBlendOp = VK_BLEND_OP_ADD,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };
    VkPipelineColorBlendStateCreateInfo colorBlend = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                       .attachmentCount = 1, .pAttachments = &attachment };
    VkDynamicState dynamic[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                      .dynamicStateCount = 2, .pDynamicStates = dynamic };
    VkGraphicsPipelineCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages,
            .pVertexInputState = &vertexInput, .pInputAssemblyState = &assembly, .pViewportState = &viewport,
            .pRasterizationState = &raster, .pMultisampleState = &multisample, .pColorBlendState = &colorBlend,
            .pDynamicState = &dynamicState, .layout = r->layout, .renderPass = pass, .subpass = 0 };
    VkPipeline pipeline = VK_NULL_HANDLE;
    return vk->CreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1, &info, NULL, &pipeline) == VK_SUCCESS ? pipeline : VK_NULL_HANDLE;
}

// A memory type `bits` allows with `want`, preferring `prefer` too; UINT32_MAX if none.
static uint32_t lorieVkMemoryType(struct lorie_vk *vk, uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer) {
    uint32_t i, type = UINT32_MAX;
    for (i = 0; i < vk->memoryProperties.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = vk->memoryProperties.memoryTypes[i].propertyFlags;
        if (!(bits & (1u << i)) || (f & want) != want)
            continue;
        if (type == UINT32_MAX || ((vk->memoryProperties.memoryTypes[type].propertyFlags & prefer) != prefer && (f & prefer) == prefer))
            type = i;
    }
    return type;
}

static void lorieVkCursorDestroy(struct lorie_vk *vk, struct lorie_vk_renderer *r) {
    if (r->cursorView)
        vk->DestroyImageView(vk->device, r->cursorView, NULL);
    if (r->cursor)
        vk->DestroyImage(vk->device, r->cursor, NULL);
    if (r->cursorMemory)
        vk->FreeMemory(vk->device, r->cursorMemory, NULL);
    r->cursorView = VK_NULL_HANDLE;
    r->cursor = VK_NULL_HANDLE;
    r->cursorMemory = VK_NULL_HANDLE;
    r->cursorWidth = r->cursorHeight = 0;
}

// The swapchain's images and what is made per image; the swapchain itself stays.
static void lorieVkSwapchainImagesDestroy(struct lorie_vk *vk, struct lorie_vk_renderer *r) {
    uint32_t i;
    for (i = 0; i < r->imageCount; i++) {
        if (r->framebuffers[i])
            vk->DestroyFramebuffer(vk->device, r->framebuffers[i], NULL);
        if (r->views[i])
            vk->DestroyImageView(vk->device, r->views[i], NULL);
        if (r->rendered[i])
            vk->DestroySemaphore(vk->device, r->rendered[i], NULL);
        r->framebuffers[i] = VK_NULL_HANDLE;
        r->views[i] = VK_NULL_HANDLE;
        r->rendered[i] = VK_NULL_HANDLE;
        r->images[i] = VK_NULL_HANDLE;
    }
    r->imageCount = 0;
}

static void lorieVkScreenPassDestroy(struct lorie_vk *vk, struct lorie_vk_renderer *r) {
    if (r->screenPipeline)
        vk->DestroyPipeline(vk->device, r->screenPipeline, NULL);
    if (r->cursorPipeline)
        vk->DestroyPipeline(vk->device, r->cursorPipeline, NULL);
    if (r->screenPass)
        vk->DestroyRenderPass(vk->device, r->screenPass, NULL);
    r->screenPipeline = r->cursorPipeline = VK_NULL_HANDLE;
    r->screenPass = VK_NULL_HANDLE;
    r->format = VK_FORMAT_UNDEFINED;
}

void lorieVkSwapchainDestroy(struct lorie_vk *vk, struct lorie_vk_renderer *r) {
    if (!vk->device)
        return;
    vk->DeviceWaitIdle(vk->device);     // nothing may still use what goes, the window included
    lorieVkSwapchainImagesDestroy(vk, r);
    if (r->swapchain)
        vk->DestroySwapchainKHR(vk->device, r->swapchain, NULL);
    lorieVkScreenPassDestroy(vk, r);
    if (r->surface)
        vk->DestroySurfaceKHR(vk->instance, r->surface, NULL);
    r->swapchain = VK_NULL_HANDLE;
    r->surface = VK_NULL_HANDLE;
    r->recreate = false;
}

void lorieVkRendererDestroy(struct lorie_vk *vk, struct lorie_vk_renderer *r) {
    int i;
    if (!vk->device) {
        memset(r, 0, sizeof(*r));
        return;
    }
    lorieVkSwapchainDestroy(vk, r);
    lorieVkCursorDestroy(vk, r);
    if (r->stagingMapped)
        vk->UnmapMemory(vk->device, r->stagingMemory);
    if (r->staging)
        vk->DestroyBuffer(vk->device, r->staging, NULL);
    if (r->stagingMemory)
        vk->FreeMemory(vk->device, r->stagingMemory, NULL);
    for (i = 0; i < 2; i++) {
        if (r->copyPipeline[i])
            vk->DestroyPipeline(vk->device, r->copyPipeline[i], NULL);
        if (r->copyPass[i])
            vk->DestroyRenderPass(vk->device, r->copyPass[i], NULL);
        if (r->samplers[i])
            vk->DestroySampler(vk->device, r->samplers[i], NULL);
    }
    if (r->vert)
        vk->DestroyShaderModule(vk->device, r->vert, NULL);
    if (r->frag)
        vk->DestroyShaderModule(vk->device, r->frag, NULL);
    if (r->descriptors)
        vk->DestroyDescriptorPool(vk->device, r->descriptors, NULL);
    if (r->layout)
        vk->DestroyPipelineLayout(vk->device, r->layout, NULL);
    if (r->setLayout)
        vk->DestroyDescriptorSetLayout(vk->device, r->setLayout, NULL);
    if (r->frameFence)
        vk->DestroyFence(vk->device, r->frameFence, NULL);
    if (r->acquireFence)
        vk->DestroyFence(vk->device, r->acquireFence, NULL);
    if (r->pool)
        vk->DestroyCommandPool(vk->device, r->pool, NULL);
    memset(r, 0, sizeof(*r));
}

#define LORIE_VK_TRY(call) do { if ((res = (call)) != VK_SUCCESS) goto fail; } while (0)
#define LORIE_VK_MADE(handle) do { if (!(handle)) { res = VK_ERROR_INITIALIZATION_FAILED; goto fail; } } while (0)

bool lorieVkRendererCreate(struct lorie_vk *vk, struct lorie_vk_renderer *r, volatile struct lorie_shared_server_state *st) {
    VkResult res;
    int i;

    memset(r, 0, sizeof(*r));
    {
        VkCommandPoolCreateInfo info = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                         .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = vk->queueFamily };
        VkCommandBufferAllocateInfo cmd = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        VkFenceCreateInfo fence = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        LORIE_VK_TRY(vk->CreateCommandPool(vk->device, &info, NULL, &r->pool));
        cmd.commandPool = r->pool;
        LORIE_VK_TRY(vk->AllocateCommandBuffers(vk->device, &cmd, &r->cmd));
        LORIE_VK_TRY(vk->CreateFence(vk->device, &fence, NULL, &r->frameFence));
        LORIE_VK_TRY(vk->CreateFence(vk->device, &fence, NULL, &r->acquireFence));
    }
    {
        VkDescriptorSetLayoutBinding binding = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
        VkDescriptorSetLayoutCreateInfo set = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                .bindingCount = 1, .pBindings = &binding };
        VkPushConstantRange push = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(struct lorie_vk_quad) };
        VkPipelineLayoutCreateInfo layout = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                              .pushConstantRangeCount = 1, .pPushConstantRanges = &push };
        VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, LORIE_VK_MAX_FRAME_IMAGES };
        VkDescriptorPoolCreateInfo pool = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                            .maxSets = LORIE_VK_MAX_FRAME_IMAGES, .poolSizeCount = 1, .pPoolSizes = &size };
        LORIE_VK_TRY(vk->CreateDescriptorSetLayout(vk->device, &set, NULL, &r->setLayout));
        layout.pSetLayouts = &r->setLayout;
        LORIE_VK_TRY(vk->CreatePipelineLayout(vk->device, &layout, NULL, &r->layout));
        LORIE_VK_TRY(vk->CreateDescriptorPool(vk->device, &pool, NULL, &r->descriptors));
    }
    for (i = 0; i < 2; i++) {
        VkFilter filter = i ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        VkSamplerCreateInfo info = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = filter, .minFilter = filter,
                                     .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                     .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                     .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                     .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.f };
        LORIE_VK_TRY(vk->CreateSampler(vk->device, &info, NULL, &r->samplers[i]));
    }
    {
        VkShaderModuleCreateInfo vert = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                          .codeSize = sizeof(lorieVkQuad_vert), .pCode = lorieVkQuad_vert };
        VkShaderModuleCreateInfo frag = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                          .codeSize = sizeof(lorieVkQuad_frag), .pCode = lorieVkQuad_frag };
        LORIE_VK_TRY(vk->CreateShaderModule(vk->device, &vert, NULL, &r->vert));
        LORIE_VK_TRY(vk->CreateShaderModule(vk->device, &frag, NULL, &r->frag));
    }
    for (i = 0; i < 2; i++) {
        LORIE_VK_MADE(r->copyPass[i] = lorieVkRenderPass(vk, i ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM, false));
        LORIE_VK_MADE(r->copyPipeline[i] = lorieVkPipeline(vk, r, r->copyPass[i], false));
    }
    {
        VkBufferCreateInfo info = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = LORIE_VK_CURSOR_BYTES,
                                    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
        VkMemoryRequirements req;
        VkMemoryAllocateInfo alloc = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        LORIE_VK_TRY(vk->CreateBuffer(vk->device, &info, NULL, &r->staging));
        vk->GetBufferMemoryRequirements(vk->device, r->staging, &req);
        alloc.allocationSize = req.size;
        if ((alloc.memoryTypeIndex = lorieVkMemoryType(vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == UINT32_MAX) {
            res = VK_ERROR_FEATURE_NOT_PRESENT;
            goto fail;
        }
        r->stagingCoherent = vk->memoryProperties.memoryTypes[alloc.memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        LORIE_VK_TRY(vk->AllocateMemory(vk->device, &alloc, NULL, &r->stagingMemory));
        LORIE_VK_TRY(vk->BindBufferMemory(vk->device, r->staging, r->stagingMemory, 0));
        LORIE_VK_TRY(vk->MapMemory(vk->device, r->stagingMemory, 0, VK_WHOLE_SIZE, 0, (void **) &r->stagingMapped));
    }
    return true;

    fail:
    loge("renderer objects: VkResult %d", res);
    lorieVkRendererDestroy(vk, r);
    // Part of making the device ready to draw with: recorded as the device's.
    lorieVkFallback(st, LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_DEVICE, res);
    return false;
}

VkPresentModeKHR lorieVkPresentModeFor(struct lorie_vk *vk, struct lorie_vk_renderer *r, bool smooth) {
    VkPresentModeKHR modes[16];
    uint32_t count = 16, i;
    bool immediate = false, mailbox = false;

    if (smooth || !r->surface || vk->GetPhysicalDeviceSurfacePresentModesKHR(vk->physicalDevice, r->surface, &count, modes) < 0)
        return VK_PRESENT_MODE_FIFO_KHR;
    for (i = 0; i < count; i++) {
        immediate |= modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR;
        mailbox |= modes[i] == VK_PRESENT_MODE_MAILBOX_KHR;
    }
    return immediate ? VK_PRESENT_MODE_IMMEDIATE_KHR : mailbox ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;
}

static const char *lorieVkPresentModeName(VkPresentModeKHR mode) {
    return mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE" : mode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" : "FIFO";
}

bool lorieVkSwapchainCreate(struct lorie_vk *vk, struct lorie_vk_renderer *r, struct ANativeWindow *window,
                            volatile struct lorie_shared_server_state *st) {
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR formats[64];
    uint32_t count = 64, i, imageCount;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkSwapchainKHR old = r->swapchain;
    VkBool32 supported = VK_FALSE;
    VkPresentModeKHR presentMode;
    uint8_t reason = LORIE_FALLBACK_VK_SWAPCHAIN;
    VkResult res;

    if (!r->surface) {
        VkAndroidSurfaceCreateInfoKHR info = { .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, .window = window };
        if (lorieVkInjected(st, LORIE_VK_FAIL_AT_SURFACE, LORIE_FALLBACK_VK_SURFACE))
            return false;
        if ((res = vk->CreateAndroidSurfaceKHR(vk->instance, &info, NULL, &r->surface)) != VK_SUCCESS) {
            r->surface = VK_NULL_HANDLE;
            lorieVkFallback(st, LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_SURFACE, res);
            return false;
        }
        LORIE_DIAG_ADD(st, vkSurfaceCreated, 1);
    }
    reason = LORIE_FALLBACK_VK_SURFACE;
    LORIE_VK_TRY(vk->GetPhysicalDeviceSurfaceSupportKHR(vk->physicalDevice, vk->queueFamily, r->surface, &supported));
    if (!supported) {
        res = VK_ERROR_FEATURE_NOT_PRESENT;
        goto fail;
    }
    LORIE_VK_TRY(vk->GetPhysicalDeviceSurfaceCapabilitiesKHR(vk->physicalDevice, r->surface, &caps));
    if (vk->GetPhysicalDeviceSurfaceFormatsKHR(vk->physicalDevice, r->surface, &count, formats) < 0)
        count = 0;
    // Formats GLES draws the same values into: UNORM, R8G8B8A8 first.
    reason = LORIE_FALLBACK_VK_SURFACE_FORMAT;
    for (i = 0; i < count && format != VK_FORMAT_R8G8B8A8_UNORM; i++)
        if (formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
            (formats[i].format == VK_FORMAT_R8G8B8A8_UNORM || formats[i].format == VK_FORMAT_B8G8R8A8_UNORM))
            format = formats[i].format;
    if (format == VK_FORMAT_UNDEFINED || !(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
        res = VK_ERROR_FORMAT_NOT_SUPPORTED;
        goto fail;
    }

    reason = LORIE_FALLBACK_VK_SWAPCHAIN;
    if (format != r->format)
        lorieVkScreenPassDestroy(vk, r);
    if (!r->screenPass) {
        LORIE_VK_MADE(r->screenPass = lorieVkRenderPass(vk, format, true));
        LORIE_VK_MADE(r->screenPipeline = lorieVkPipeline(vk, r, r->screenPass, false));
        LORIE_VK_MADE(r->cursorPipeline = lorieVkPipeline(vk, r, r->screenPass, true));
        r->format = format;
    }

    // The surface's current size where it gives one (currentExtent), otherwise the window's, as GLES's surface
    // has it; within what the surface allows; not pre-rotated (as GLES).
    if (caps.currentExtent.width != UINT32_MAX && caps.currentExtent.height != UINT32_MAX)
        r->extent = caps.currentExtent;
    else {
        r->extent.width = (uint32_t) ANativeWindow_getWidth(window);
        r->extent.height = (uint32_t) ANativeWindow_getHeight(window);
    }
    r->extent.width = r->extent.width < caps.minImageExtent.width ? caps.minImageExtent.width :
                      r->extent.width > caps.maxImageExtent.width ? caps.maxImageExtent.width : r->extent.width;
    r->extent.height = r->extent.height < caps.minImageExtent.height ? caps.minImageExtent.height :
                       r->extent.height > caps.maxImageExtent.height ? caps.maxImageExtent.height : r->extent.height;
    if (!r->extent.width || !r->extent.height) {
        res = VK_ERROR_OUT_OF_DATE_KHR;
        goto fail;
    }
    presentMode = lorieVkPresentModeFor(vk, r, r->smooth);
    imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount && imageCount > caps.maxImageCount)
        imageCount = caps.maxImageCount;
    if (imageCount > LORIE_VK_MAX_SWAPCHAIN_IMAGES)
        imageCount = LORIE_VK_MAX_SWAPCHAIN_IMAGES;

    if (lorieVkInjected(st, LORIE_VK_FAIL_AT_SWAPCHAIN, LORIE_FALLBACK_VK_SWAPCHAIN))
        return false;
    {
        VkSwapchainCreateInfoKHR info = {
                .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = r->surface, .minImageCount = imageCount,
                .imageFormat = format, .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, .imageExtent = r->extent,
                .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ?
                                VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform,
                // Opaque where the surface allows it; the frame writes alpha one everywhere anyway.
                .compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR :
                                  (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) ? VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR :
                                  (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) ?
                                  VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR : VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
                .presentMode = presentMode, .clipped = VK_TRUE, .oldSwapchain = old };
        vk->DeviceWaitIdle(vk->device);     // the old swapchain's images may still be in use
        lorieVkSwapchainImagesDestroy(vk, r);
        res = vk->CreateSwapchainKHR(vk->device, &info, NULL, &r->swapchain);
        if (old)
            vk->DestroySwapchainKHR(vk->device, old, NULL);
        if (res != VK_SUCCESS) {
            r->swapchain = VK_NULL_HANDLE;
            goto fail;
        }
    }
    count = LORIE_VK_MAX_SWAPCHAIN_IMAGES;
    LORIE_VK_TRY(vk->GetSwapchainImagesKHR(vk->device, r->swapchain, &count, r->images));
    r->imageCount = count;
    for (i = 0; i < count; i++) {
        VkImageViewCreateInfo view = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = r->images[i],
                                       .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
                                       .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        VkFramebufferCreateInfo fb = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = r->screenPass,
                                       .attachmentCount = 1, .width = r->extent.width, .height = r->extent.height, .layers = 1 };
        VkSemaphoreCreateInfo sem = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        LORIE_VK_TRY(vk->CreateImageView(vk->device, &view, NULL, &r->views[i]));
        fb.pAttachments = &r->views[i];
        LORIE_VK_TRY(vk->CreateFramebuffer(vk->device, &fb, NULL, &r->framebuffers[i]));
        LORIE_VK_TRY(vk->CreateSemaphore(vk->device, &sem, NULL, &r->rendered[i]));
    }
    r->recreate = false;
    r->presentMode = presentMode;
    LORIE_DIAG_ADD(st, vkSwapchainCreated, 1);
    if (st)
        strncpy((char *) st->diag.vkPresentMode, lorieVkPresentModeName(presentMode), sizeof(st->diag.vkPresentMode) - 1);
    log("swapchain %ux%u format %d, %u images, %s", r->extent.width, r->extent.height, format, count,
        lorieVkPresentModeName(presentMode));
    return true;

    fail:
    loge("swapchain: reason %u VkResult %d", reason, res);
    lorieVkFallback(st, LORIE_FALLBACK_CLASS_FATAL_BACKEND, reason, res);
    return false;
}

// OUT_OF_DATE: the swapchain is made again, unless that already failed to help LORIE_VK_MAX_OUT_OF_DATE times.
static int lorieVkOutOfDate(struct lorie_vk_renderer *r, volatile struct lorie_shared_server_state *st) {
    if (++r->outOfDateStreak > LORIE_VK_MAX_OUT_OF_DATE) {
        lorieVkFallback(st, LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_SWAPCHAIN, VK_ERROR_OUT_OF_DATE_KHR);
        return LORIE_VK_FRAME_FATAL;
    }
    r->recreate = true;
    return LORIE_VK_FRAME_RECREATE;
}

int lorieVkAcquire(struct lorie_vk *vk, struct lorie_vk_renderer *r, uint32_t *index, volatile struct lorie_shared_server_state *st) {
    uint64_t start = lorieVkNowUs();
    VkResult res = vk->AcquireNextImageKHR(vk->device, r->swapchain, LORIE_VK_ACQUIRE_TIMEOUT_NS, VK_NULL_HANDLE, r->acquireFence, index);
    VkResult wait;

    switch (res) {
        case VK_SUCCESS:
        case VK_SUBOPTIMAL_KHR:
            // The presentation engine may read the image until the fence signals: waited for here, outside the X
            // server's lock, then reset, so that the next acquire is given an unsignalled fence.
            if ((wait = vk->WaitForFences(vk->device, 1, &r->acquireFence, VK_TRUE, LORIE_VK_FENCE_TIMEOUT_NS)) != VK_SUCCESS ||
                (wait = vk->ResetFences(vk->device, 1, &r->acquireFence)) != VK_SUCCESS) {
                lorieVkFallback(st, LORIE_FALLBACK_CLASS_FATAL_BACKEND,
                                wait == VK_ERROR_DEVICE_LOST ? LORIE_FALLBACK_VK_DEVICE_LOST : LORIE_FALLBACK_VK_SWAPCHAIN, wait);
                return LORIE_VK_FRAME_FATAL;
            }
            LORIE_DIAG_ADD(st, vkAcquireWaitUs, lorieVkNowUs() - start);
            // Suboptimal still presents successfully: counted, not a reason to make the swapchain again.
            if (res == VK_SUBOPTIMAL_KHR)
                LORIE_DIAG_ADD(st, vkAcquireSuboptimal, 1);
            else
                LORIE_DIAG_ADD(st, vkAcquireSuccess, 1);
            return LORIE_VK_FRAME_OK;
        case VK_TIMEOUT:
            LORIE_DIAG_ADD(st, vkAcquireTimeout, 1);
            return LORIE_VK_FRAME_SKIP;
        case VK_NOT_READY:
            LORIE_DIAG_ADD(st, vkAcquireNotReady, 1);
            return LORIE_VK_FRAME_SKIP;
        case VK_ERROR_OUT_OF_DATE_KHR:
            LORIE_DIAG_ADD(st, vkAcquireOutOfDate, 1);
            return lorieVkOutOfDate(r, st);
        default:
            lorieVkFallback(st, LORIE_FALLBACK_CLASS_FATAL_BACKEND,
                            res == VK_ERROR_DEVICE_LOST ? LORIE_FALLBACK_VK_DEVICE_LOST : LORIE_FALLBACK_VK_SWAPCHAIN, res);
            return LORIE_VK_FRAME_FATAL;
    }
}

VkResult lorieVkFrameBegin(struct lorie_vk *vk, struct lorie_vk_renderer *r) {
    VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VkResult res;
    r->usedCount = r->sourceDraws = r->destinationDraws = r->draws = 0;
    r->submitCpuUs = r->fenceWaitUs = 0;
    if ((res = vk->ResetCommandBuffer(r->cmd, 0)) != VK_SUCCESS || (res = vk->ResetDescriptorPool(vk->device, r->descriptors, 0)) != VK_SUCCESS)
        return res;
    return vk->BeginCommandBuffer(r->cmd, &begin);
}

bool lorieVkFrameUse(struct lorie_vk *vk, struct lorie_vk_renderer *r, struct lorie_vk_image *img, enum lorie_vk_use use) {
    uint32_t i;
    for (i = 0; i < r->usedCount; i++)
        if (r->used[i].img == img) {
            if (r->used[i].use != use) {
                lorieVkCmdBarrier(vk, r->cmd, lorieVkUseBarrier(img->image, r->used[i].use, use));
                r->used[i].use = use;
            }
            return true;
        }
    if (r->usedCount == LORIE_VK_MAX_FRAME_IMAGES)
        return false;
    lorieVkCmdBarrier(vk, r->cmd, lorieVkAcquireBarrier(vk, img->image, use));
    r->used[r->usedCount].img = img;
    r->used[r->usedCount++].use = use;
    return true;
}

static VkDescriptorSet lorieVkDescriptor(struct lorie_vk *vk, struct lorie_vk_renderer *r, VkImageView view, int filter) {
    VkDescriptorSetAllocateInfo info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = r->descriptors,
                                         .descriptorSetCount = 1, .pSetLayouts = &r->setLayout };
    VkDescriptorImageInfo image = { r->samplers[filter ? 1 : 0], view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet write = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &image };
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vk->AllocateDescriptorSets(vk->device, &info, &set) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    write.dstSet = set;
    vk->UpdateDescriptorSets(vk->device, 1, &write, 0, NULL);
    return set;
}

bool lorieVkFrameCopy(struct lorie_vk *vk, struct lorie_vk_renderer *r, struct lorie_vk_image *src, struct lorie_vk_image *dst,
                      const struct lorie_vk_quad *quads, uint32_t count, int filter) {
    int pass = dst->format == VK_FORMAT_B8G8R8A8_UNORM ? 1 : 0;
    VkDescriptorSet set;
    uint32_t i;

    if (src == dst || !dst->attachView)
        return false;
    if (!dst->framebuffer) {
        VkFramebufferCreateInfo fb = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = r->copyPass[pass],
                                       .attachmentCount = 1, .pAttachments = &dst->attachView, .width = dst->width,
                                       .height = dst->height, .layers = 1 };
        if (vk->CreateFramebuffer(vk->device, &fb, NULL, &dst->framebuffer) != VK_SUCCESS) {
            dst->framebuffer = VK_NULL_HANDLE;
            return false;
        }
    }
    if (!lorieVkFrameUse(vk, r, src, LORIE_VK_USE_SAMPLE) || !lorieVkFrameUse(vk, r, dst, LORIE_VK_USE_RENDER) ||
        !(set = lorieVkDescriptor(vk, r, src->sampleView, filter)))
        return false;
    {
        VkRenderPassBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = r->copyPass[pass],
                                        .framebuffer = dst->framebuffer, .renderArea = { { 0, 0 }, { dst->width, dst->height } } };
        VkViewport viewport = { 0.f, 0.f, (float) dst->width, (float) dst->height, 0.f, 1.f };
        vk->CmdBeginRenderPass(r->cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
        vk->CmdBindPipeline(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->copyPipeline[pass]);
        vk->CmdSetViewport(r->cmd, 0, 1, &viewport);
        vk->CmdSetScissor(r->cmd, 0, 1, &begin.renderArea);
        vk->CmdBindDescriptorSets(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->layout, 0, 1, &set, 0, NULL);
        for (i = 0; i < count; i++) {
            vk->CmdPushConstants(r->cmd, r->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                 sizeof(*quads), &quads[i]);
            vk->CmdDraw(r->cmd, 4, 1, 0, 0);
            r->draws++;
            r->sourceDraws++;
            r->destinationDraws++;
        }
        vk->CmdEndRenderPass(r->cmd);
    }
    return true;
}

bool lorieVkFrameCursor(struct lorie_vk *vk, struct lorie_vk_renderer *r, const uint32_t *bits, uint32_t width, uint32_t height) {
    VkImageMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                     .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                     .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    if (!width || !height || (size_t) width * height * 4 > LORIE_VK_CURSOR_BYTES)
        return false;
    if (width != r->cursorWidth || height != r->cursorHeight || !r->cursor) {
        VkImageCreateInfo info = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                   .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { width, height, 1 }, .mipLevels = 1, .arrayLayers = 1,
                                   .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                   .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                   .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        VkMemoryRequirements req;
        VkMemoryAllocateInfo alloc = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        VkImageViewCreateInfo view = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                       .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        lorieVkCursorDestroy(vk, r);
        if (vk->CreateImage(vk->device, &info, NULL, &r->cursor) != VK_SUCCESS) {
            r->cursor = VK_NULL_HANDLE;
            return false;
        }
        vk->GetImageMemoryRequirements(vk->device, r->cursor, &req);
        alloc.allocationSize = req.size;
        if ((alloc.memoryTypeIndex = lorieVkMemoryType(vk, req.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) == UINT32_MAX ||
            vk->AllocateMemory(vk->device, &alloc, NULL, &r->cursorMemory) != VK_SUCCESS) {
            r->cursorMemory = VK_NULL_HANDLE;
            lorieVkCursorDestroy(vk, r);
            return false;
        }
        view.image = r->cursor;
        if (vk->BindImageMemory(vk->device, r->cursor, r->cursorMemory, 0) != VK_SUCCESS ||
            vk->CreateImageView(vk->device, &view, NULL, &r->cursorView) != VK_SUCCESS) {
            r->cursorView = VK_NULL_HANDLE;
            lorieVkCursorDestroy(vk, r);
            return false;
        }
        r->cursorWidth = width;
        r->cursorHeight = height;
    }
    memcpy(r->stagingMapped, bits, (size_t) width * height * 4);
    if (!r->stagingCoherent) {
        VkMappedMemoryRange range = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = r->stagingMemory, .size = VK_WHOLE_SIZE };
        vk->FlushMappedMemoryRanges(vk->device, 1, &range);
    }
    // The renderer's own image, all of it written: its earlier contents are not needed.
    barrier.image = r->cursor;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    vk->CmdPipelineBarrier(r->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    vk->CmdCopyBufferToImage(r->cmd, r->staging, r->cursor, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                             &(VkBufferImageCopy) { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { width, height, 1 } });
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vk->CmdPipelineBarrier(r->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    return true;
}

void lorieVkFrameScreen(struct lorie_vk *vk, struct lorie_vk_renderer *r, uint32_t index, struct lorie_vk_image *root,
                        const VkRect2D *viewport, const struct lorie_vk_quad *rootQuad, const struct lorie_vk_quad *cursorQuad,
                        int filter) {
    VkClearValue black = { .color = { .float32 = { 0.f, 0.f, 0.f, 1.f } } };
    VkRenderPassBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = r->screenPass,
                                    .framebuffer = r->framebuffers[index], .renderArea = { { 0, 0 }, r->extent },
                                    .clearValueCount = 1, .pClearValues = &black };
    VkViewport vp = { (float) viewport->offset.x, (float) viewport->offset.y, (float) viewport->extent.width,
                      (float) viewport->extent.height, 0.f, 1.f };
    VkRect2D scissor = *viewport;
    VkDescriptorSet rootSet = VK_NULL_HANDLE, cursorSet = VK_NULL_HANDLE;

    if (root && lorieVkFrameUse(vk, r, root, LORIE_VK_USE_SAMPLE))
        rootSet = lorieVkDescriptor(vk, r, root->sampleView, filter);
    if (cursorQuad && r->cursorView)
        cursorSet = lorieVkDescriptor(vk, r, r->cursorView, filter);
    // The scissor within the image (the viewport may reach out of it).
    if (scissor.offset.x < 0) {
        scissor.extent.width = (uint32_t) ((int32_t) scissor.extent.width + scissor.offset.x > 0 ? (int32_t) scissor.extent.width + scissor.offset.x : 0);
        scissor.offset.x = 0;
    }
    if (scissor.offset.y < 0) {
        scissor.extent.height = (uint32_t) ((int32_t) scissor.extent.height + scissor.offset.y > 0 ? (int32_t) scissor.extent.height + scissor.offset.y : 0);
        scissor.offset.y = 0;
    }
    if ((uint32_t) scissor.offset.x + scissor.extent.width > r->extent.width)
        scissor.extent.width = (uint32_t) scissor.offset.x < r->extent.width ? r->extent.width - (uint32_t) scissor.offset.x : 0;
    if ((uint32_t) scissor.offset.y + scissor.extent.height > r->extent.height)
        scissor.extent.height = (uint32_t) scissor.offset.y < r->extent.height ? r->extent.height - (uint32_t) scissor.offset.y : 0;

    vk->CmdBeginRenderPass(r->cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
    if (vp.width > 0 && vp.height > 0) {
        vk->CmdSetViewport(r->cmd, 0, 1, &vp);
        vk->CmdSetScissor(r->cmd, 0, 1, &scissor);
        if (rootSet) {
            vk->CmdBindPipeline(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->screenPipeline);
            vk->CmdBindDescriptorSets(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->layout, 0, 1, &rootSet, 0, NULL);
            vk->CmdPushConstants(r->cmd, r->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(*rootQuad), rootQuad);
            vk->CmdDraw(r->cmd, 4, 1, 0, 0);
            r->draws++;
            r->sourceDraws++;
        }
        if (cursorSet) {
            vk->CmdBindPipeline(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->cursorPipeline);
            vk->CmdBindDescriptorSets(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->layout, 0, 1, &cursorSet, 0, NULL);
            vk->CmdPushConstants(r->cmd, r->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(*cursorQuad), cursorQuad);
            vk->CmdDraw(r->cmd, 4, 1, 0, 0);
            r->draws++;
        }
    }
    vk->CmdEndRenderPass(r->cmd);
}

VkResult lorieVkFrameQueue(struct lorie_vk *vk, struct lorie_vk_renderer *r, uint32_t index, bool present,
                           volatile struct lorie_shared_server_state *st) {
    VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &r->cmd,
                            .signalSemaphoreCount = present ? 1 : 0, .pSignalSemaphores = present ? &r->rendered[index] : NULL };
    uint64_t start;
    VkResult res;
    uint32_t i;

    // Every X server buffer the frame used goes back to the foreign owner, from the use it ended in.
    for (i = 0; i < r->usedCount; i++)
        lorieVkCmdBarrier(vk, r->cmd, lorieVkReleaseBarrier(vk, r->used[i].img->image, r->used[i].use));
    if ((res = vk->EndCommandBuffer(r->cmd)) != VK_SUCCESS) {
        lorieVkFallback(st, LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_SUBMIT, res);
        return res;
    }
    // Not submitted: nothing changed hands (the recorded acquires never ran), nothing to wait for.
    if (lorieVkInjected(st, LORIE_VK_FAIL_AT_SUBMIT, LORIE_FALLBACK_VK_SUBMIT))
        return VK_ERROR_UNKNOWN;
    start = lorieVkNowUs();
    if ((res = vk->QueueSubmit(vk->queue, 1, &submit, r->frameFence)) != VK_SUCCESS) {
        lorieVkFallback(st, LORIE_FALLBACK_CLASS_FATAL_BACKEND,
                        res == VK_ERROR_DEVICE_LOST ? LORIE_FALLBACK_VK_DEVICE_LOST : LORIE_FALLBACK_VK_SUBMIT, res);
        return res;
    }
    r->submitCpuUs = lorieVkNowUs() - start;
    return VK_SUCCESS;
}

VkResult lorieVkFrameWait(struct lorie_vk *vk, struct lorie_vk_renderer *r, volatile struct lorie_shared_server_state *st,
                          bool *over) {
    uint64_t start = lorieVkNowUs();
    VkResult res, idle;

    if ((res = vk->WaitForFences(vk->device, 1, &r->frameFence, VK_TRUE, LORIE_VK_FENCE_TIMEOUT_NS)) != VK_SUCCESS) {
        lorieVkFallback(st, LORIE_FALLBACK_CLASS_FATAL_BACKEND,
                        res == VK_ERROR_DEVICE_LOST ? LORIE_FALLBACK_VK_DEVICE_LOST : LORIE_FALLBACK_VK_SUBMIT, res);
        // The X server must not touch the frame's buffers before the device is done with them: the device waited
        // for. Lost counts as done (Lost Device: for whether resources are in use, VK_ERROR_DEVICE_LOST is equivalent
        // to VK_SUCCESS; other APIs keep the external memory, its content undefined). Out of memory establishes
        // neither: the frame stays not over.
        idle = vk->DeviceWaitIdle(vk->device);
        *over = idle == VK_SUCCESS || idle == VK_ERROR_DEVICE_LOST || res == VK_ERROR_DEVICE_LOST;
        vk->ResetFences(vk->device, 1, &r->frameFence);
        return res;
    }
    *over = true;
    r->fenceWaitUs = lorieVkNowUs() - start;
    if ((res = vk->ResetFences(vk->device, 1, &r->frameFence)) != VK_SUCCESS) {
        lorieVkFallback(st, LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_SUBMIT, res);
        return res;
    }
    LORIE_DIAG_ADD(st, vkQueueSubmits, 1);
    LORIE_DIAG_ADD(st, vkImportedSourceDraws, r->sourceDraws);
    LORIE_DIAG_ADD(st, vkImportedDestinationDraws, r->destinationDraws);
    LORIE_DIAG_ADD(st, vkRenderCommands, r->draws);
    LORIE_DIAG_ADD(st, vkSubmitCpuUs, r->submitCpuUs);
    LORIE_DIAG_ADD(st, vkFenceWaitUs, r->fenceWaitUs);
    return VK_SUCCESS;
}

VkResult lorieVkFrameSubmit(struct lorie_vk *vk, struct lorie_vk_renderer *r, uint32_t index, bool present,
                            volatile struct lorie_shared_server_state *st) {
    VkResult res = lorieVkFrameQueue(vk, r, index, present, st);
    bool over;
    return res != VK_SUCCESS ? res : lorieVkFrameWait(vk, r, st, &over);
}

int lorieVkPresent(struct lorie_vk *vk, struct lorie_vk_renderer *r, uint32_t index, volatile struct lorie_shared_server_state *st) {
    VkPresentInfoKHR info = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                              .pWaitSemaphores = &r->rendered[index], .swapchainCount = 1, .pSwapchains = &r->swapchain,
                              .pImageIndices = &index };
    uint64_t start;
    VkResult res;

    if (lorieVkInjected(st, LORIE_VK_FAIL_AT_PRESENT, LORIE_FALLBACK_VK_PRESENT))
        return LORIE_VK_FRAME_FATAL;
    start = lorieVkNowUs();
    res = vk->QueuePresentKHR(vk->queue, &info);
    LORIE_DIAG_ADD(st, vkPresentCallUs, lorieVkNowUs() - start);
    switch (res) {
        case VK_SUBOPTIMAL_KHR:     // presented: nothing to make again (see LORIE_VK_MAX_OUT_OF_DATE)
        case VK_SUCCESS:
            r->outOfDateStreak = 0;
            LORIE_DIAG_ADD(st, vkQueuePresentSuccess, 1);
            LORIE_DIAG_ADD(st, vkFramesPresented, 1);
            return LORIE_VK_FRAME_OK;
        case VK_ERROR_OUT_OF_DATE_KHR:
            return lorieVkOutOfDate(r, st);
        default:
            lorieVkFallback(st, LORIE_FALLBACK_CLASS_FATAL_BACKEND,
                            res == VK_ERROR_DEVICE_LOST ? LORIE_FALLBACK_VK_DEVICE_LOST : LORIE_FALLBACK_VK_PRESENT, res);
            return LORIE_VK_FRAME_FATAL;
    }
}
