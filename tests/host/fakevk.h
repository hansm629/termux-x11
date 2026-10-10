/* The fake Vulkan the vk.c host tests run on (tvkinit.c, tvkimport.c): reached only through the
 * vkGetInstanceProcAddr given to lorieVkCreateDevice, as libvulkan.so's would be, configured per case through
 * `cfg` (good() sets up a usable phone), and recording what it was called with in `calls` and `imp`. Also the
 * NDK's AHardwareBuffer as far as vk.c uses it, and the real vk.h/vk.c code (gen.py "vk"). */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_ANDROID_KHR 1
#include <vulkan/vulkan.h>

/* ---- the NDK's AHardwareBuffer, as far as vk.c uses it: a buffer whose properties a test sets ---- */
enum {
    AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM = 1, AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM = 2,
    AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM = 4, AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM = 5,
};
#define AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN 3ULL
#define AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN (3ULL << 4)
#define AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE (1ULL << 8)
#define AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER (1ULL << 9)
typedef struct AHardwareBuffer_Desc {
    uint32_t width, height, layers, format;
    uint64_t usage;
    uint32_t stride, rfu0;
    uint64_t rfu1;
} AHardwareBuffer_Desc;
typedef struct AHardwareBuffer {
    AHardwareBuffer_Desc desc;
    VkFormat vkFormat;              /* what Vulkan reports for it: VK_FORMAT_UNDEFINED for an external format */
    uint32_t memoryTypeBits;
    VkDeviceSize allocationSize;
    VkResult propertiesResult;
} AHardwareBuffer;
static void AHardwareBuffer_describe(const AHardwareBuffer *b, AHardwareBuffer_Desc *d) { *d = b->desc; }

#include "diagtypes_src.inc"
struct lorie_shared_server_state { struct lorie_renderer_diag diag; struct lorie_backend_control backend; };
#define log(...) ((void) 0)
#define loge(...) ((void) 0)
#include "vk_src.inc"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL " __VA_ARGS__); printf("\n"); } } while (0)

/* ---- the fake Vulkan ---- */
struct fake_device {
    uint32_t api;
    VkQueueFlags families[3];
    int nfamilies;
    const char *ext[4];
    int next;
    const char *name;
};
static struct {
    bool noEnumerateInstanceVersion, noGetDeviceProcAddr;
    uint32_t instanceVersion;
    const char *instanceExt[3];
    int ninstanceExt;
    VkResult createInstance, createDevice;
    struct fake_device dev[2];
    int ndev;
    VkPhysicalDeviceMemoryProperties memory;
    /* the image-format query, and the calls an import makes */
    VkResult formatQuery, createImage, allocateMemory, bindMemory;
    int failViewNumber;             /* the n-th vkCreateImageView of a case fails (0: none) */
    VkExternalMemoryFeatureFlags externalFeatures;
    VkExtent3D maxExtent;
} cfg;
static struct {
    int createInstance, destroyInstance, createDevice, destroyDevice, waitIdle, getQueue;
    uint32_t apiVersion, family;
    char instanceExt[256], deviceExt[512];
    VkPhysicalDevice deviceUsed;
} calls;
static int instanceObj, deviceObj, queueObj;

static VKAPI_ATTR VkResult VKAPI_CALL fEnumerateInstanceVersion(uint32_t *v) { *v = cfg.instanceVersion; return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL fEnumerateInstanceExtensionProperties(const char *layer, uint32_t *count, VkExtensionProperties *p) {
    (void) layer;
    if (p)
        for (int i = 0; i < cfg.ninstanceExt && i < (int) *count; i++)
            snprintf(p[i].extensionName, sizeof p[i].extensionName, "%s", cfg.instanceExt[i]);
    *count = cfg.ninstanceExt;
    return VK_SUCCESS;
}
static void joinNames(char *out, size_t size, const char *const *names, uint32_t n) {
    out[0] = 0;
    for (uint32_t i = 0; i < n; i++)
        snprintf(out + strlen(out), size - strlen(out), "%s%s", i ? " " : "", names[i]);
}
static VKAPI_ATTR VkResult VKAPI_CALL fCreateInstance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *a, VkInstance *out) {
    (void) a;
    calls.createInstance++;
    calls.apiVersion = info->pApplicationInfo ? info->pApplicationInfo->apiVersion : 0;
    joinNames(calls.instanceExt, sizeof calls.instanceExt, info->ppEnabledExtensionNames, info->enabledExtensionCount);
    if (cfg.createInstance != VK_SUCCESS)
        return cfg.createInstance;
    *out = (VkInstance) &instanceObj;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL fDestroyInstance(VkInstance i, const VkAllocationCallbacks *a) {
    (void) a;
    CHECK(i == (VkInstance) &instanceObj, "destroyed an instance that was not created");
    calls.destroyInstance++;
}
static VKAPI_ATTR VkResult VKAPI_CALL fEnumeratePhysicalDevices(VkInstance i, uint32_t *count, VkPhysicalDevice *p) {
    (void) i;
    if (p)
        for (int d = 0; d < cfg.ndev && d < (int) *count; d++)
            p[d] = (VkPhysicalDevice) &cfg.dev[d];
    *count = cfg.ndev;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL fGetPhysicalDeviceProperties(VkPhysicalDevice pd, VkPhysicalDeviceProperties *p) {
    struct fake_device *d = (struct fake_device *) pd;
    memset(p, 0, sizeof *p);
    p->apiVersion = d->api;
    p->driverVersion = 0x8040000u;
    snprintf(p->deviceName, sizeof p->deviceName, "%s", d->name);
}
static VKAPI_ATTR void VKAPI_CALL fGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice pd, uint32_t *count, VkQueueFamilyProperties *p) {
    struct fake_device *d = (struct fake_device *) pd;
    if (p)
        for (int i = 0; i < d->nfamilies && i < (int) *count; i++)
            p[i] = (VkQueueFamilyProperties) { .queueFlags = d->families[i], .queueCount = 1 };
    *count = d->nfamilies;
}
static VKAPI_ATTR VkResult VKAPI_CALL fEnumerateDeviceExtensionProperties(VkPhysicalDevice pd, const char *layer, uint32_t *count, VkExtensionProperties *p) {
    struct fake_device *d = (struct fake_device *) pd;
    (void) layer;
    if (p)
        for (int i = 0; i < d->next && i < (int) *count; i++)
            snprintf(p[i].extensionName, sizeof p[i].extensionName, "%s", d->ext[i]);
    *count = d->next;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL fGetPhysicalDeviceMemoryProperties(VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties *p) {
    (void) pd;
    *p = cfg.memory;
}
static VKAPI_ATTR VkResult VKAPI_CALL fCreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *info, const VkAllocationCallbacks *a, VkDevice *out) {
    (void) a;
    calls.createDevice++;
    calls.deviceUsed = pd;
    calls.family = info->queueCreateInfoCount ? info->pQueueCreateInfos[0].queueFamilyIndex : 99;
    joinNames(calls.deviceExt, sizeof calls.deviceExt, info->ppEnabledExtensionNames, info->enabledExtensionCount);
    if (cfg.createDevice != VK_SUCCESS)
        return cfg.createDevice;
    *out = (VkDevice) &deviceObj;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL fDestroyDevice(VkDevice d, const VkAllocationCallbacks *a) {
    (void) a;
    CHECK(d == (VkDevice) &deviceObj, "destroyed a device that was not created");
    CHECK(calls.waitIdle > calls.destroyDevice, "device destroyed without waiting for it to go idle");
    calls.destroyDevice++;
}
static VKAPI_ATTR void VKAPI_CALL fGetDeviceQueue(VkDevice d, uint32_t family, uint32_t index, VkQueue *q) {
    (void) d; (void) family; (void) index;
    calls.getQueue++;
    *q = (VkQueue) &queueObj;
}
static VKAPI_ATTR VkResult VKAPI_CALL fDeviceWaitIdle(VkDevice d) {
    (void) d;
    calls.waitIdle++;
    return VK_SUCCESS;
}
/* ---- what an import calls: every object counted while alive, every argument kept ---- */
static struct {
    int properties, formatQueries, images, memories, views, binds;
    VkPhysicalDeviceImageFormatInfo2 query;
    VkExternalMemoryHandleTypeFlagBits queryHandleType;
    VkImageCreateInfo image;
    VkExternalMemoryHandleTypeFlags imageHandleTypes;
    VkDeviceSize allocationSize;
    uint32_t memoryType;
    const AHardwareBuffer *importedBuffer;
    VkImage dedicatedImage, boundImage, createdImage;
    VkImageViewCreateInfo viewInfo[2];
    int viewCount;
} imp;
static uintptr_t nextHandle = 0x1000;
static const void *findNext(const void *chain, VkStructureType type) {
    for (const VkBaseInStructure *p = chain; p; p = p->pNext)
        if (p->sType == type)
            return p;
    return NULL;
}
static VKAPI_ATTR VkResult VKAPI_CALL fGetAndroidHardwareBufferPropertiesANDROID(VkDevice d, const struct AHardwareBuffer *b,
                                                                                VkAndroidHardwareBufferPropertiesANDROID *p) {
    VkAndroidHardwareBufferFormatPropertiesANDROID *f = (VkAndroidHardwareBufferFormatPropertiesANDROID *)
        findNext(p->pNext, VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID);
    (void) d;
    imp.properties++;
    if (b->propertiesResult != VK_SUCCESS)
        return b->propertiesResult;
    p->allocationSize = b->allocationSize;
    p->memoryTypeBits = b->memoryTypeBits;
    if (f) {
        f->format = b->vkFormat;
        f->externalFormat = b->vkFormat == VK_FORMAT_UNDEFINED ? 77 : 0;
    }
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL fGetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice pd, const VkPhysicalDeviceImageFormatInfo2 *info,
                                                                              VkImageFormatProperties2 *out) {
    const VkPhysicalDeviceExternalImageFormatInfo *ext =
        findNext(info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO);
    VkExternalImageFormatProperties *eout =
        (VkExternalImageFormatProperties *) findNext(out->pNext, VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES);
    (void) pd;
    imp.formatQueries++;
    imp.query = *info;
    imp.queryHandleType = ext ? ext->handleType : 0;
    if (cfg.formatQuery != VK_SUCCESS)
        return cfg.formatQuery;
    out->imageFormatProperties = (VkImageFormatProperties) { .maxExtent = cfg.maxExtent, .maxMipLevels = 1, .maxArrayLayers = 1 };
    if (eout)
        eout->externalMemoryProperties.externalMemoryFeatures = cfg.externalFeatures;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL fCreateImage(VkDevice d, const VkImageCreateInfo *info, const VkAllocationCallbacks *a, VkImage *out) {
    const VkExternalMemoryImageCreateInfo *ext = findNext(info->pNext, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
    (void) d; (void) a;
    imp.image = *info;
    imp.imageHandleTypes = ext ? ext->handleTypes : 0;
    if (cfg.createImage != VK_SUCCESS)
        return cfg.createImage;
    imp.images++;
    *out = imp.createdImage = (VkImage) nextHandle++;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL fDestroyImage(VkDevice d, VkImage i, const VkAllocationCallbacks *a) {
    (void) d; (void) a;
    CHECK(i, "vkDestroyImage of a null image");
    imp.images--;
}
static VKAPI_ATTR VkResult VKAPI_CALL fAllocateMemory(VkDevice d, const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *a, VkDeviceMemory *out) {
    const VkImportAndroidHardwareBufferInfoANDROID *import =
        findNext(info->pNext, VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID);
    const VkMemoryDedicatedAllocateInfo *dedicated = findNext(info->pNext, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO);
    (void) d; (void) a;
    imp.allocationSize = info->allocationSize;
    imp.memoryType = info->memoryTypeIndex;
    imp.importedBuffer = import ? import->buffer : NULL;
    imp.dedicatedImage = dedicated ? dedicated->image : VK_NULL_HANDLE;
    if (cfg.allocateMemory != VK_SUCCESS)
        return cfg.allocateMemory;
    imp.memories++;
    *out = (VkDeviceMemory) nextHandle++;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL fFreeMemory(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *a) {
    (void) d; (void) a;
    CHECK(m, "vkFreeMemory of null memory");
    imp.memories--;
}
static VKAPI_ATTR VkResult VKAPI_CALL fBindImageMemory(VkDevice d, VkImage i, VkDeviceMemory m, VkDeviceSize offset) {
    (void) d; (void) m;
    CHECK(offset == 0, "dedicated memory bound at offset %llu", (unsigned long long) offset);
    imp.boundImage = i;
    if (cfg.bindMemory != VK_SUCCESS)
        return cfg.bindMemory;
    imp.binds++;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL fCreateImageView(VkDevice d, const VkImageViewCreateInfo *info, const VkAllocationCallbacks *a, VkImageView *out) {
    (void) d; (void) a;
    if (imp.viewCount < 2)
        imp.viewInfo[imp.viewCount] = *info;
    if (++imp.viewCount == cfg.failViewNumber)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    imp.views++;
    *out = (VkImageView) nextHandle++;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL fDestroyImageView(VkDevice d, VkImageView v, const VkAllocationCallbacks *a) {
    (void) d; (void) a;
    CHECK(v, "vkDestroyImageView of a null view");
    imp.views--;
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fGetDeviceProcAddr(VkDevice d, const char *name) {
    (void) d;
#define F(n) if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction) f ## n;
    F(DestroyDevice) F(GetDeviceQueue) F(DeviceWaitIdle) F(GetAndroidHardwareBufferPropertiesANDROID) F(CreateImage)
    F(DestroyImage) F(AllocateMemory) F(FreeMemory) F(BindImageMemory) F(CreateImageView) F(DestroyImageView)
#undef F
    return NULL;
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fGetInstanceProcAddr(VkInstance instance, const char *name) {
#define F(n) if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction) f ## n;
    if (!instance) {
        if (!cfg.noEnumerateInstanceVersion)
            F(EnumerateInstanceVersion)
        F(EnumerateInstanceExtensionProperties) F(CreateInstance)
        return NULL;
    }
    F(DestroyInstance) F(EnumeratePhysicalDevices) F(GetPhysicalDeviceProperties) F(GetPhysicalDeviceQueueFamilyProperties)
    F(EnumerateDeviceExtensionProperties) F(GetPhysicalDeviceMemoryProperties) F(CreateDevice)
    F(GetPhysicalDeviceImageFormatProperties2)
    if (!cfg.noGetDeviceProcAddr)
        F(GetDeviceProcAddr)
#undef F
    return NULL;
}

#define SWAPCHAIN VK_KHR_SWAPCHAIN_EXTENSION_NAME
#define EXT_AHB VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME
#define FOREIGN VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME

/* A phone: Vulkan 1.1 loader and device, a compute-only family before the graphics one, all extensions. */
static void good(void) {
    memset(&cfg, 0, sizeof cfg);
    cfg.instanceVersion = VK_API_VERSION_1_1;
    cfg.instanceExt[0] = VK_KHR_SURFACE_EXTENSION_NAME;
    cfg.instanceExt[1] = VK_KHR_ANDROID_SURFACE_EXTENSION_NAME;
    cfg.ninstanceExt = 2;
    cfg.dev[0] = (struct fake_device) { VK_API_VERSION_1_1, { VK_QUEUE_COMPUTE_BIT, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT }, 2,
                                        { SWAPCHAIN, EXT_AHB, FOREIGN, "VK_KHR_other" }, 4, "Adreno (TM) 840" };
    cfg.ndev = 1;
    /* host-visible only, device-local, device-local: an import may use whichever its buffer allows */
    cfg.memory.memoryTypeCount = 3;
    cfg.memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    cfg.memory.memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    cfg.memory.memoryTypes[2].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    cfg.externalFeatures = VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT;
    cfg.maxExtent = (VkExtent3D) { 16384, 16384, 1 };
}
