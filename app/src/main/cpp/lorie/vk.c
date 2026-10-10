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
static void lorieVkImportFailed(volatile struct lorie_shared_server_state *st, uint8_t reason, VkResult result,
                                uint64_t id, const AHardwareBuffer_Desc *desc) {
    loge("cannot import AHardwareBuffer %llu (%ux%u format %u usage %#llx): reason %u", (unsigned long long) id,
         desc->width, desc->height, desc->format, (unsigned long long) desc->usage, reason);
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
        lorieVkImportFailed(st, LORIE_FALLBACK_AHB_PROPERTIES, VK_ERROR_INVALID_EXTERNAL_HANDLE, id, &desc);
        return NULL;
    }
    LORIE_DIAG_ADD(st, vkAhbImportAttempts, 1);
    AHardwareBuffer_describe(ahb, &desc);
    if (lorieVkInjected(st, LORIE_VK_FAIL_AT_IMPORT, LORIE_FALLBACK_AHB_IMPORT)) {
        lorieVkImportFailed(st, LORIE_FALLBACK_NONE, VK_SUCCESS, id, &desc);
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
    lorieVkImportFailed(st, reason, r, id, &desc);
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
