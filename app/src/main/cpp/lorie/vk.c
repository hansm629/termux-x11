#include <dlfcn.h>
#include <time.h>
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
        if (images->items[i].id == id)
            return &images->items[i];
    return NULL;
}

static void lorieVkImageDestroy(struct lorie_vk *vk, struct lorie_vk_image *img) {
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
        struct lorie_vk_image *items = realloc(images->items, capacity * sizeof(*items));
        if (!items) {
            reason = LORIE_FALLBACK_AHB_IMPORT;
            r = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto fail;
        }
        images->items = items;
        images->capacity = capacity;
    }
    images->items[images->count] = img;
    LORIE_DIAG_ADD(st, vkAhbImportSuccess, 1);
    return &images->items[images->count++];

    fail:
    lorieVkImageDestroy(vk, &img);
    lorieVkImportFailed(images, st, reason, r, id, &desc);
    return NULL;
}

void lorieVkImageRemove(struct lorie_vk *vk, struct lorie_vk_images *images, uint64_t id) {
    struct lorie_vk_image *img = lorieVkImageFind(images, id);
    if (!img)
        return;
    lorieVkImageDestroy(vk, img);
    *img = images->items[--images->count];
}

void lorieVkImagesClear(struct lorie_vk *vk, struct lorie_vk_images *images) {
    while (images->count)
        lorieVkImageRemove(vk, images, images->items[0].id);
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
        st->diag.vkSelftestUs = (uint64_t) (t1.tv_sec - t0.tv_sec) * 1000000 + (uint64_t) (t1.tv_nsec - t0.tv_nsec) / 1000;
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
        st->diag.vkSelftestUs = (uint64_t) (t1.tv_sec - t0.tv_sec) * 1000000 + (uint64_t) (t1.tv_nsec - t0.tv_nsec) / 1000;
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
