/* The fake Vulkan the vk.c host tests run on (tvkinit.c, tvkimport.c, tvkbarrier.c, tvkselftest.c): reached
 * only through the vkGetInstanceProcAddr given to lorieVkCreateDevice, as libvulkan.so's would be, configured
 * per case through `cfg` (good() sets up a usable phone), and recording what it was called with in `calls` and
 * `imp`. Also the NDK's AHardwareBuffer as far as vk.c uses it, and the real vk.h/vk.c code (gen.py "vk").
 *
 * A small device behind them (`emu`) runs what is submitted, so the ownership self-test can be run for real: an
 * AHardwareBuffer has the CPU's memory (rows `stride` pixels apart) and the device's (tightly packed). Only an
 * acquire from VK_QUEUE_FAMILY_FOREIGN_EXT in GENERAL brings the CPU's contents to the device (from UNDEFINED
 * they are discarded); only a release to FOREIGN in GENERAL brings the device's back. Copies need the buffer
 * owned and in the copy's layout. The CPU locking a buffer the renderer owns, or before a submission's fence
 * was waited for, is a violation. A test can fail an allocation, a lock, a submission or a fence wait, or make
 * the n-th copy write one wrong byte. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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
    /* the small device: memory as the CPU and as the device see it, who owns it, its layout on the device */
    uint8_t *cpu, *gpu;
    uint64_t lockedHash;            /* the CPU's memory when it was locked: an unlock that changed it was a write */
    bool owned, locked;
    VkImageLayout layout;
} AHardwareBuffer;
typedef struct ARect { int32_t left, top, right, bottom; } ARect;

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL " __VA_ARGS__); printf("\n"); } } while (0)

static struct {
    /* set by a test */
    bool allocFail[8];              /* by AHB format */
    bool bgraExternal;              /* Vulkan reports an external format for BGRA */
    long lockFail, corruptCopy;     /* the n-th lock fails; the n-th copy writes one wrong byte (1-based, 0: none) */
    uint32_t corruptByte;
    VkResult submitResult, fenceResult;
    /* kept */
    long copies, locks;
    bool fencePending;
    int violations, acquires, acquiresFromUndefined, releases, ahbLive, pools, fences, buffers, mapped;
    int flushes, invalidates, downloads, hostBarriers, cpuWrites;
} emu;
static uint64_t hashOf(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    while (n--)
        h = (h ^ *p++) * 1099511628211ULL;
    return h;
}

static void violation(const char *what) {
    emu.violations++;
    CHECK(0, "fake device: %s", what);
}
static void AHardwareBuffer_describe(const AHardwareBuffer *b, AHardwareBuffer_Desc *d) { *d = b->desc; }
static int AHardwareBuffer_allocate(const AHardwareBuffer_Desc *d, AHardwareBuffer **out) {
    AHardwareBuffer *b;
    if (emu.allocFail[d->format & 7] || !(b = calloc(1, sizeof *b)))
        return -12;
    b->desc = *d;
    b->desc.stride = (d->width + 31) & ~31u;
    b->vkFormat = d->format == AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM ? (emu.bgraExternal ? VK_FORMAT_UNDEFINED : VK_FORMAT_B8G8R8A8_UNORM)
                : VK_FORMAT_R8G8B8A8_UNORM;
    b->memoryTypeBits = 0x7;
    b->allocationSize = (VkDeviceSize) b->desc.stride * d->height * 4;
    b->cpu = malloc((size_t) b->desc.stride * d->height * 4);
    b->gpu = malloc((size_t) d->width * d->height * 4);
    memset(b->cpu, 0xa5, (size_t) b->desc.stride * d->height * 4);
    memset(b->gpu, 0x5a, (size_t) d->width * d->height * 4);
    emu.ahbLive++;
    *out = b;
    return 0;
}
static void AHardwareBuffer_release(AHardwareBuffer *b) {
    CHECK(!b->locked && !b->owned, "fake device: buffer released while %s", b->locked ? "locked" : "owned by the renderer");
    free(b->cpu);
    free(b->gpu);
    free(b);
    emu.ahbLive--;
}
static int AHardwareBuffer_lock(AHardwareBuffer *b, uint64_t usage, int32_t fence, const ARect *rect, void **out) {
    (void) usage; (void) rect;
    CHECK(fence == -1, "fake device: lock with a fence");
    if (++emu.locks == emu.lockFail)
        return -5;
    if (emu.fencePending)
        violation("CPU lock before the submission's fence was waited for");
    if (b->owned)
        violation("CPU lock of a buffer the renderer owns");
    if (b->locked)
        violation("CPU lock of a locked buffer");
    b->locked = true;
    b->lockedHash = hashOf(b->cpu, (size_t) b->desc.stride * b->desc.height * 4);
    *out = b->cpu;
    return 0;
}
static int AHardwareBuffer_unlock(AHardwareBuffer *b, int32_t *fence) {
    CHECK(!fence, "fake device: unlock asking for a fence");
    if (!b->locked)
        violation("CPU unlock of a buffer not locked");
    else if (hashOf(b->cpu, (size_t) b->desc.stride * b->desc.height * 4) != b->lockedHash)
        emu.cpuWrites++;
    b->locked = false;
    return 0;
}

#include "diagtypes_src.inc"
struct lorie_shared_server_state { struct lorie_renderer_diag diag; struct lorie_backend_control backend; };
#define log(...) ((void) 0)
#define loge(...) ((void) 0)
#include "vk_src.inc"

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
    emu.fencePending = false;
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
static struct { VkDeviceMemory memory; AHardwareBuffer *ahb; uint8_t *host; } memories[64];
static struct { VkImage image; AHardwareBuffer *ahb; } boundImages[64];
static struct { VkBuffer buffer; VkDeviceSize size; uint8_t *bytes; } buffers[16];
static AHardwareBuffer *ahbOf(VkImage image) {
    for (int i = 0; i < 64; i++)
        if (boundImages[i].image == image)
            return boundImages[i].ahb;
    return NULL;
}
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
    for (int j = 0; j < 64; j++)
        if (boundImages[j].image == i)
            memset(&boundImages[j], 0, sizeof boundImages[j]);
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
    for (int i = 0; i < 64; i++)
        if (!memories[i].memory) {
            memories[i].memory = *out;
            memories[i].ahb = import ? (AHardwareBuffer *) import->buffer : NULL;
            memories[i].host = import ? NULL : calloc(1, info->allocationSize);
            break;
        }
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL fFreeMemory(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *a) {
    (void) d; (void) a;
    CHECK(m, "vkFreeMemory of null memory");
    imp.memories--;
    for (int i = 0; i < 64; i++)
        if (memories[i].memory == m) {
            free(memories[i].host);
            memset(&memories[i], 0, sizeof memories[i]);
        }
}
static VKAPI_ATTR VkResult VKAPI_CALL fBindImageMemory(VkDevice d, VkImage i, VkDeviceMemory m, VkDeviceSize offset) {
    (void) d; (void) m;
    CHECK(offset == 0, "dedicated memory bound at offset %llu", (unsigned long long) offset);
    imp.boundImage = i;
    if (cfg.bindMemory != VK_SUCCESS)
        return cfg.bindMemory;
    imp.binds++;
    for (int k = 0; k < 64; k++)
        if (memories[k].memory == m)
            for (int j = 0; j < 64; j++)
                if (!boundImages[j].image || boundImages[j].image == i) {
                    boundImages[j].image = i;
                    boundImages[j].ahb = memories[k].ahb;
                    break;
                }
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
/* ---- what the self-test records and submits, run by the small device ---- */
enum { OP_IMAGE_BARRIER, OP_BUFFER_BARRIER, OP_COPY_IMAGE, OP_BUFFER_TO_IMAGE, OP_IMAGE_TO_BUFFER };
struct fake_op {
    int kind;
    VkImageMemoryBarrier image;
    VkBufferMemoryBarrier buffer;
    VkPipelineStageFlags src, dst;
    VkImage a, b;
    VkBuffer buf;
    VkImageLayout la, lb;
    VkExtent3D extent;
};
static struct { struct fake_op ops[16]; int n; bool recording; } cmdObj;
static uint8_t *bufferBytes(VkBuffer b) {
    for (int i = 0; i < 16; i++)
        if (buffers[i].buffer == b)
            return buffers[i].bytes;
    return NULL;
}
static void record(struct fake_op op) {
    if (!cmdObj.recording || cmdObj.n == 16)
        violation("command recorded outside a command buffer being recorded, or too many");
    else
        cmdObj.ops[cmdObj.n++] = op;
}
static VKAPI_ATTR VkResult VKAPI_CALL fCreateCommandPool(VkDevice d, const VkCommandPoolCreateInfo *i, const VkAllocationCallbacks *a, VkCommandPool *p) {
    (void) d; (void) a;
    CHECK(i->queueFamilyIndex == 1 && (i->flags & VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT), "fake device: command pool");
    emu.pools++;
    *p = (VkCommandPool) nextHandle++;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL fDestroyCommandPool(VkDevice d, VkCommandPool p, const VkAllocationCallbacks *a) {
    (void) d; (void) p; (void) a;
    emu.pools--;
}
static VKAPI_ATTR VkResult VKAPI_CALL fAllocateCommandBuffers(VkDevice d, const VkCommandBufferAllocateInfo *i, VkCommandBuffer *c) {
    (void) d;
    CHECK(i->commandBufferCount == 1, "fake device: one command buffer");
    *c = (VkCommandBuffer) &cmdObj;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL fResetCommandBuffer(VkCommandBuffer c, VkCommandBufferResetFlags f) {
    (void) c; (void) f;
    cmdObj.n = 0;
    cmdObj.recording = false;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL fBeginCommandBuffer(VkCommandBuffer c, const VkCommandBufferBeginInfo *i) {
    (void) c; (void) i;
    cmdObj.n = 0;
    cmdObj.recording = true;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL fEndCommandBuffer(VkCommandBuffer c) {
    (void) c;
    cmdObj.recording = false;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL fCmdPipelineBarrier(VkCommandBuffer c, VkPipelineStageFlags src, VkPipelineStageFlags dst,
        VkDependencyFlags f, uint32_t nm, const VkMemoryBarrier *m, uint32_t nb, const VkBufferMemoryBarrier *b,
        uint32_t ni, const VkImageMemoryBarrier *im) {
    (void) c; (void) f; (void) m;
    CHECK(nm == 0, "fake device: global memory barrier");
    for (uint32_t i = 0; i < nb; i++)
        record((struct fake_op) { .kind = OP_BUFFER_BARRIER, .buffer = b[i], .src = src, .dst = dst });
    for (uint32_t i = 0; i < ni; i++)
        record((struct fake_op) { .kind = OP_IMAGE_BARRIER, .image = im[i], .src = src, .dst = dst });
}
static VKAPI_ATTR void VKAPI_CALL fCmdCopyImage(VkCommandBuffer c, VkImage s, VkImageLayout sl, VkImage t, VkImageLayout tl,
                                                uint32_t n, const VkImageCopy *r) {
    (void) c;
    CHECK(n == 1, "fake device: one region");
    record((struct fake_op) { .kind = OP_COPY_IMAGE, .a = s, .la = sl, .b = t, .lb = tl, .extent = r->extent });
}
static VKAPI_ATTR void VKAPI_CALL fCmdCopyBufferToImage(VkCommandBuffer c, VkBuffer s, VkImage t, VkImageLayout tl, uint32_t n,
                                                        const VkBufferImageCopy *r) {
    (void) c;
    CHECK(n == 1 && r->bufferOffset == 0 && r->bufferRowLength == 0 && r->bufferImageHeight == 0, "fake device: a tightly packed region");
    record((struct fake_op) { .kind = OP_BUFFER_TO_IMAGE, .buf = s, .b = t, .lb = tl, .extent = r->imageExtent });
}
static VKAPI_ATTR void VKAPI_CALL fCmdCopyImageToBuffer(VkCommandBuffer c, VkImage s, VkImageLayout sl, VkBuffer t, uint32_t n,
                                                        const VkBufferImageCopy *r) {
    (void) c;
    CHECK(n == 1 && r->bufferOffset == 0 && r->bufferRowLength == 0 && r->bufferImageHeight == 0, "fake device: a tightly packed region");
    record((struct fake_op) { .kind = OP_IMAGE_TO_BUFFER, .a = s, .la = sl, .buf = t, .extent = r->imageExtent });
}
static void pack(AHardwareBuffer *b) {
    for (uint32_t y = 0; y < b->desc.height; y++)
        memcpy(b->gpu + (size_t) y * b->desc.width * 4, b->cpu + (size_t) y * b->desc.stride * 4, b->desc.width * 4);
}
static void unpack(AHardwareBuffer *b) {
    for (uint32_t y = 0; y < b->desc.height; y++)
        memcpy(b->cpu + (size_t) y * b->desc.stride * 4, b->gpu + (size_t) y * b->desc.width * 4, b->desc.width * 4);
}
static void copied(uint8_t *dst) {
    if (++emu.copies == emu.corruptCopy)
        dst[emu.corruptByte] ^= 0x01;
}
static bool usable(AHardwareBuffer *b, VkImageLayout l, VkImageLayout want, VkExtent3D e) {
    if (!b || !b->owned || b->layout != l || l != want) {
        violation("copy with an image not owned, or not in the copy's layout");
        return false;
    }
    if (e.width != b->desc.width || e.height != b->desc.height || e.depth != 1) {
        violation("copy of a part of the image");
        return false;
    }
    return true;
}
static void run(const struct fake_op *op) {
    AHardwareBuffer *a = op->a ? ahbOf(op->a) : NULL, *b = op->b ? ahbOf(op->b) : NULL, *x;
    const VkImageMemoryBarrier *m = &op->image;
    switch (op->kind) {
    case OP_IMAGE_BARRIER:
        if (!(x = ahbOf(m->image))) {
            violation("barrier on an unknown image");
            return;
        }
        if (m->srcQueueFamilyIndex == VK_QUEUE_FAMILY_FOREIGN_EXT && m->dstQueueFamilyIndex == 1) {
            if (x->owned || x->locked)
                violation("acquire of a buffer the renderer owns or the CPU holds");
            if (m->oldLayout == VK_IMAGE_LAYOUT_GENERAL)
                pack(x);
            else if (m->oldLayout == VK_IMAGE_LAYOUT_UNDEFINED)
                memset(x->gpu, 0xee, (size_t) x->desc.width * x->desc.height * 4), emu.acquiresFromUndefined++;
            else
                violation("acquire from a layout the buffer is never released in");
            x->owned = true;
            x->layout = m->newLayout;
            emu.acquires++;
        } else if (m->srcQueueFamilyIndex == 1 && m->dstQueueFamilyIndex == VK_QUEUE_FAMILY_FOREIGN_EXT) {
            if (!x->owned || m->oldLayout != x->layout)
                violation("release of a buffer not owned, or from another layout");
            x->owned = false;
            if (m->newLayout == VK_IMAGE_LAYOUT_GENERAL)
                unpack(x), emu.releases++;
            else
                violation("release in a layout other than GENERAL");
        } else if (m->srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED && m->dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED) {
            if (!x->owned || (m->oldLayout != x->layout && m->oldLayout != VK_IMAGE_LAYOUT_UNDEFINED))
                violation("layout transition of a buffer not owned, or from another layout");
            if (m->oldLayout == VK_IMAGE_LAYOUT_UNDEFINED)
                memset(x->gpu, 0xee, (size_t) x->desc.width * x->desc.height * 4);
            x->layout = m->newLayout;
        } else
            violation("ownership transfer with a queue family other than FOREIGN and the renderer's");
        return;
    case OP_BUFFER_BARRIER:
        return;
    case OP_COPY_IMAGE:
        if (usable(a, op->la, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, op->extent) && usable(b, op->lb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, op->extent)) {
            memcpy(b->gpu, a->gpu, (size_t) a->desc.width * a->desc.height * 4);
            copied(b->gpu);
        }
        return;
    case OP_BUFFER_TO_IMAGE:
        if (usable(b, op->lb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, op->extent)) {
            memcpy(b->gpu, bufferBytes(op->buf), (size_t) b->desc.width * b->desc.height * 4);
            copied(b->gpu);
        }
        return;
    case OP_IMAGE_TO_BUFFER:
        if (usable(a, op->la, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, op->extent)) {
            memcpy(bufferBytes(op->buf), a->gpu, (size_t) a->desc.width * a->desc.height * 4);
            copied(bufferBytes(op->buf));
        }
        emu.downloads++;
        return;
    }
}
static VKAPI_ATTR VkResult VKAPI_CALL fQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *s, VkFence f) {
    (void) q;
    if (emu.submitResult != VK_SUCCESS)
        return emu.submitResult;
    CHECK(n == 1 && s->commandBufferCount == 1 && s->pCommandBuffers[0] == (VkCommandBuffer) &cmdObj && !cmdObj.recording,
          "fake device: submission of a command buffer still being recorded");
    if (emu.fencePending)
        violation("submission while the previous one's fence was not waited for");
    for (int i = 0; i < cmdObj.n; i++) {
        run(&cmdObj.ops[i]);
        /* the host reads a download after the fence: only a barrier to HOST_READ makes it visible there */
        if (cmdObj.ops[i].kind == OP_IMAGE_TO_BUFFER) {
            bool barrier = false;
            for (int j = i + 1; j < cmdObj.n; j++)
                if (cmdObj.ops[j].kind == OP_BUFFER_BARRIER && cmdObj.ops[j].buffer.buffer == cmdObj.ops[i].buf &&
                    (cmdObj.ops[j].buffer.srcAccessMask & VK_ACCESS_TRANSFER_WRITE_BIT) &&
                    (cmdObj.ops[j].buffer.dstAccessMask & VK_ACCESS_HOST_READ_BIT) &&
                    (cmdObj.ops[j].src & VK_PIPELINE_STAGE_TRANSFER_BIT) && (cmdObj.ops[j].dst & VK_PIPELINE_STAGE_HOST_BIT))
                    barrier = true;
            if (barrier)
                emu.hostBarriers++;
            else
                violation("a copy into host memory with no barrier making it visible to the host");
        }
    }
    emu.fencePending = f != VK_NULL_HANDLE;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL fCreateFence(VkDevice d, const VkFenceCreateInfo *i, const VkAllocationCallbacks *a, VkFence *f) {
    (void) d; (void) i; (void) a;
    emu.fences++;
    *f = (VkFence) nextHandle++;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL fDestroyFence(VkDevice d, VkFence f, const VkAllocationCallbacks *a) {
    (void) d; (void) f; (void) a;
    emu.fences--;
}
static VKAPI_ATTR VkResult VKAPI_CALL fWaitForFences(VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t timeout) {
    (void) d; (void) n; (void) f; (void) all;
    CHECK(timeout != UINT64_MAX, "fake device: an unbounded fence wait");
    if (emu.fenceResult != VK_SUCCESS)
        return emu.fenceResult;
    emu.fencePending = false;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL fResetFences(VkDevice d, uint32_t n, const VkFence *f) {
    (void) d; (void) n; (void) f;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL fCreateBuffer(VkDevice d, const VkBufferCreateInfo *i, const VkAllocationCallbacks *a, VkBuffer *b) {
    (void) d; (void) a;
    emu.buffers++;
    *b = (VkBuffer) nextHandle++;
    for (int k = 0; k < 16; k++)
        if (!buffers[k].buffer) {
            buffers[k].buffer = *b;
            buffers[k].size = i->size;
            buffers[k].bytes = NULL;
            break;
        }
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL fDestroyBuffer(VkDevice d, VkBuffer b, const VkAllocationCallbacks *a) {
    (void) d; (void) a;
    emu.buffers--;
    for (int k = 0; k < 16; k++)
        if (buffers[k].buffer == b)
            memset(&buffers[k], 0, sizeof buffers[k]);
}
static VKAPI_ATTR void VKAPI_CALL fGetBufferMemoryRequirements(VkDevice d, VkBuffer b, VkMemoryRequirements *r) {
    (void) d;
    for (int k = 0; k < 16; k++)
        if (buffers[k].buffer == b)
            *r = (VkMemoryRequirements) { buffers[k].size, 256, 0x7 };
}
static VKAPI_ATTR VkResult VKAPI_CALL fBindBufferMemory(VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize offset) {
    (void) d;
    for (int k = 0; k < 16; k++)
        if (buffers[k].buffer == b)
            for (int j = 0; j < 64; j++)
                if (memories[j].memory == m)
                    buffers[k].bytes = memories[j].host + offset;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL fMapMemory(VkDevice d, VkDeviceMemory m, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags f, void **out) {
    (void) d; (void) size; (void) f;
    for (int j = 0; j < 64; j++)
        if (memories[j].memory == m && memories[j].host) {
            emu.mapped++;
            *out = memories[j].host + offset;
            return VK_SUCCESS;
        }
    return VK_ERROR_MEMORY_MAP_FAILED;
}
static VKAPI_ATTR void VKAPI_CALL fUnmapMemory(VkDevice d, VkDeviceMemory m) {
    (void) d; (void) m;
    emu.mapped--;
}
static VKAPI_ATTR VkResult VKAPI_CALL fFlushMappedMemoryRanges(VkDevice d, uint32_t n, const VkMappedMemoryRange *r) {
    (void) d; (void) n; (void) r;
    emu.flushes++;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL fInvalidateMappedMemoryRanges(VkDevice d, uint32_t n, const VkMappedMemoryRange *r) {
    (void) d; (void) n; (void) r;
    emu.invalidates++;
    return VK_SUCCESS;
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fGetDeviceProcAddr(VkDevice d, const char *name) {
    (void) d;
#define F(n) if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction) f ## n;
    F(DestroyDevice) F(GetDeviceQueue) F(DeviceWaitIdle) F(GetAndroidHardwareBufferPropertiesANDROID) F(CreateImage)
    F(DestroyImage) F(AllocateMemory) F(FreeMemory) F(BindImageMemory) F(CreateImageView) F(DestroyImageView)
    F(CreateCommandPool) F(DestroyCommandPool) F(AllocateCommandBuffers) F(ResetCommandBuffer) F(BeginCommandBuffer)
    F(EndCommandBuffer) F(CmdPipelineBarrier) F(CmdCopyImage) F(CmdCopyBufferToImage) F(CmdCopyImageToBuffer)
    F(QueueSubmit) F(CreateFence) F(DestroyFence) F(WaitForFences) F(ResetFences) F(CreateBuffer) F(DestroyBuffer)
    F(GetBufferMemoryRequirements) F(BindBufferMemory) F(MapMemory) F(UnmapMemory) F(FlushMappedMemoryRanges)
    F(InvalidateMappedMemoryRanges)
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
    memset(&emu, 0, sizeof emu);
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
