/* The fake Vulkan the vk.c host tests run on (tvkinit.c, tvkimport.c, tvkbarrier.c, tvkselftest.c, tvkframe.c,
 * tpresentcopy.c): reached only through the vkGetInstanceProcAddr given to lorieVkCreateDevice, as libvulkan.so's would be, configured
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
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <linux/futex.h>
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
typedef struct ANativeWindow { int32_t width, height; } ANativeWindow;
static int32_t ANativeWindow_getWidth(ANativeWindow *w) { return w->width; }
static int32_t ANativeWindow_getHeight(ANativeWindow *w) { return w->height; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL " __VA_ARGS__); printf("\n"); } } while (0)

static struct {
    /* set by a test */
    bool allocFail[8];              /* by AHB format */
    bool bgraExternal;              /* Vulkan reports an external format for BGRA */
    long lockFail, corruptCopy;     /* the n-th lock fails; the n-th copy writes one wrong byte (1-based, 0: none) */
    uint32_t corruptByte;
    VkResult submitResult, fenceResult;
    VkResult acquireResult, presentResult;  /* what the next acquire / present returns */
    int idleFailures;               /* vkDeviceWaitIdle answers out of memory this many times (the device not idle), */
    VkResult idleResult;            /* then this: VK_SUCCESS, or VK_ERROR_DEVICE_LOST */
    bool surfaceUnsupported;
    VkSurfaceFormatKHR surfaceFormats[4];
    uint32_t surfaceFormatCount;
    VkPresentModeKHR presentModes[4];   /* what the surface lists: good() lists FIFO only */
    uint32_t presentModeCount;
    VkSurfaceCapabilitiesKHR caps;
    long failObject;                /* the n-th render pass, pipeline, framebuffer, ... made fails (1-based, 0: none) */
    /* kept */
    long objects;                   /* render passes, pipelines, ... made so far */
    int live;                       /* of those, not destroyed */
    int surfaces, swapchains, swapchainsMade, acquires2, presents, deviceWaits;
    VkSwapchainCreateInfoKHR swapchainInfo;
    uint32_t nextImage;
    int passesIntoImported, drawsFromImported, drawsIntoImported, draws, copyCommands, copyRegions;
    AHardwareBuffer *drawnFrom[64], *drawnInto[64];    /* draw n's X server buffers (NULL: none), at n % 64 */
    long copies, locks;
    bool fencePending;              /* a submission's fence not waited for yet - any (a test clears them all with it) */
    int earlyPresents;              /* presents made before the frame's fence was waited for (on the semaphore alone) */
    int auxCmdFail;                 /* P11-C3-B2: a second command buffer cannot be allocated */
    int submitFailAfter;            /* the n-th submission from now fails (device lost) */
    int submits;                    /* submissions made */
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
static void pendingSync(void);
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
#include "gpucopytypes_src.inc"
/* the shared state, as far as the code under test uses it (lorie.h's fields, its types for the GPU copy queue;
 * a smaller cursor) */
struct lorie_shared_server_state {
    int lock, lockingPid;
    struct {
        volatile uint32_t writeIndex;
        volatile uint32_t readIndex;
        volatile uint64_t completedSerial;
        volatile uint64_t completedUs;
        LorieGpuCopyEntry entries[LORIE_GPU_COPY_QUEUE_CAPACITY];
    } gpuCopyQueue;
    struct {
        volatile uint32_t busy;
        volatile pid_t pid;
        uint32_t count;
        uint64_t ids[LORIE_GPU_IN_FLIGHT_MAX];
        uint32_t auxCount;      /* P11-C3-B2 */
        uint64_t auxIds[LORIE_GPU_IN_FLIGHT_MAX];
    } gpuInFlight;
    struct {                    /* P11-C3-B1 */
        uint32_t count;
        uint64_t ids[LORIE_CPU_ACCESS_MAX];
        volatile uint32_t releases, waiter;
    } cpuAccess;
    struct {                    /* P11-C2 */
        volatile uint32_t seq, taken, cancelled, failed, done;
        volatile pid_t pid;
        uint64_t srcId, dstId, queuedUs;
        uint32_t numRects;
        LorieGpuCopyRect rects[LORIE_GPU_COPY_MAX_RECTS];
        volatile uint64_t doneUs;   /* P11-C3 */
        volatile uint32_t waiting;  /* P11-C3-B2 */
    } rootCopy;
    uint64_t rootWindowTextureID;
    volatile uint8_t drawRequested, surfaceAvailable, waitForNextFrame;
    volatile uint64_t frameWantedUs;    /* P11-C3 */
    volatile int renderedFrames;
    struct lorie_renderer_diag diag;
    struct lorie_backend_control backend;
    struct {
        int lock, lockingPid;
        uint32_t x, y, xhot, yhot, width, height;
        uint32_t bits[64 * 64];
        volatile uint8_t updated, moved;
    } cursor;
};
#ifndef __always_inline
#define __always_inline __attribute__((always_inline))
#endif
#include "inflight_src.inc"
#define log(...) ((void) 0)
#define loge(...) ((void) 0)
#include "vk_shaders.h"
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
    VkFormatFeatureFlags optimalFeatures;   /* every format's optimal-tiling features (good(): sampled, attachment, transfer) */
    bool refuseTransferUsage;               /* the image-format query refuses the transfer usage (an AHB the device cannot copy) */
    bool failTransferImage;                 /* vkCreateImage fails an image with the transfer usage the query allowed */
    bool copyConverts;                      /* a driver bug: a copy between two formats converts (red and blue swapped) */
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
    emu.deviceWaits++;
    if (emu.idleFailures > 0) {
        emu.idleFailures--;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    emu.fencePending = false;
    pendingSync();
    return emu.idleResult;
}
/* ---- what an import calls: every object counted while alive, every argument kept ---- */
static struct {
    int properties, formatQueries, featureQueries, images, memories, views, binds;
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
static struct { VkImage image; AHardwareBuffer *ahb; VkImageUsageFlags usage; } boundImages[64];
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
    if (cfg.refuseTransferUsage && (info->usage & (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    out->imageFormatProperties = (VkImageFormatProperties) { .maxExtent = cfg.maxExtent, .maxMipLevels = 1, .maxArrayLayers = 1 };
    if (eout)
        eout->externalMemoryProperties.externalMemoryFeatures = cfg.externalFeatures;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL fGetPhysicalDeviceFormatProperties(VkPhysicalDevice pd, VkFormat f, VkFormatProperties *out) {
    (void) pd; (void) f;
    imp.featureQueries++;
    *out = (VkFormatProperties) { .optimalTilingFeatures = cfg.optimalFeatures };
}
static VKAPI_ATTR VkResult VKAPI_CALL fCreateImage(VkDevice d, const VkImageCreateInfo *info, const VkAllocationCallbacks *a, VkImage *out) {
    const VkExternalMemoryImageCreateInfo *ext = findNext(info->pNext, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
    (void) d; (void) a;
    imp.image = *info;
    imp.imageHandleTypes = ext ? ext->handleTypes : 0;
    if (cfg.createImage != VK_SUCCESS)
        return cfg.createImage;
    if (cfg.failTransferImage && (info->usage & (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)))
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
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
                    boundImages[j].usage = i == imp.createdImage ? imp.image.usage : 0;
                    break;
                }
    return VK_SUCCESS;
}
static struct { VkImageView view; VkImage image; } viewImages[128];
static VkImage imageOfView(VkImageView v) {
    for (int i = 0; i < 128; i++)
        if (v && viewImages[i].view == v)
            return viewImages[i].image;
    return VK_NULL_HANDLE;
}
static VKAPI_ATTR VkResult VKAPI_CALL fCreateImageView(VkDevice d, const VkImageViewCreateInfo *info, const VkAllocationCallbacks *a, VkImageView *out) {
    (void) d; (void) a;
    if (imp.viewCount < 2)
        imp.viewInfo[imp.viewCount] = *info;
    if (++imp.viewCount == cfg.failViewNumber)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    imp.views++;
    *out = (VkImageView) nextHandle++;
    for (int i = 0; i < 128; i++)
        if (!viewImages[i].view) {
            viewImages[i].view = *out;
            viewImages[i].image = info->image;
            break;
        }
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL fDestroyImageView(VkDevice d, VkImageView v, const VkAllocationCallbacks *a) {
    (void) d; (void) a;
    CHECK(v, "vkDestroyImageView of a null view");
    imp.views--;
    for (int i = 0; i < 128; i++)
        if (viewImages[i].view == v)
            memset(&viewImages[i], 0, sizeof viewImages[i]);
}
/* ---- what the self-test records and submits, run by the small device ---- */
enum { OP_IMAGE_BARRIER, OP_BUFFER_BARRIER, OP_COPY_IMAGE, OP_BUFFER_TO_IMAGE, OP_IMAGE_TO_BUFFER, OP_BEGIN_PASS,
       OP_END_PASS, OP_BIND_SET, OP_DRAW };
struct fake_op {
    int kind;
    VkImageMemoryBarrier image;
    VkBufferMemoryBarrier buffer;
    VkPipelineStageFlags src, dst;
    VkImage a, b;
    VkBuffer buf;
    VkImageLayout la, lb;
    VkExtent3D extent;
    VkFramebuffer framebuffer;
    VkDescriptorSet set;
    VkImageCopy regions[16];        /* vkCmdCopyImage's */
    uint32_t nregions;
};
/* command buffers: the renderer's two (P11-C3-B2: a second submission) - each recorded, then pending from its
 * submission until its fence was waited for (or the device idle); the one being recorded now */
struct fake_cmd { struct fake_op ops[256]; int n; bool recording, allocated, pending; VkFence fence; VkDescriptorPool pools[4]; };
static struct fake_cmd cmdObj, cmdAux, *recordingCmd = &cmdObj;
static struct { VkFramebuffer framebuffer; VkImageView view; } framebuffers[64];
static struct { VkDescriptorSet set; VkImageView view; VkImageLayout layout; VkDescriptorPool pool; } descriptorSets[64];
static VkImage imageOfFramebuffer(VkFramebuffer f) {
    for (int i = 0; i < 64; i++)
        if (f && framebuffers[i].framebuffer == f)
            return imageOfView(framebuffers[i].view);
    return VK_NULL_HANDLE;
}
static const VkImageLayout *layoutOfSet(VkDescriptorSet s, VkImage *image) {
    for (int i = 0; i < 64; i++)
        if (s && descriptorSets[i].set == s) {
            *image = imageOfView(descriptorSets[i].view);
            return &descriptorSets[i].layout;
        }
    *image = VK_NULL_HANDLE;
    return NULL;
}
static uint8_t *bufferBytes(VkBuffer b) {
    for (int i = 0; i < 16; i++)
        if (buffers[i].buffer == b)
            return buffers[i].bytes;
    return NULL;
}
static void record(struct fake_op op) {
    if (!recordingCmd->recording || recordingCmd->n == 256)
        violation("command recorded outside a command buffer being recorded, or too many");
    else
        recordingCmd->ops[recordingCmd->n++] = op;
}
/* the fences submissions are pending on (emu.fencePending: any - a test that clears it clears them all) */
static struct { VkFence fence; bool signaled, pending; } fences[16];
static void pendingSync(void) {
    if (emu.fencePending)
        return;
    cmdObj.pending = cmdAux.pending = false;
    for (int k = 0; k < 16; k++)
        fences[k].pending = false;
}
static void pendingRecount(void) {
    emu.fencePending = false;
    for (int k = 0; k < 16; k++)
        emu.fencePending |= fences[k].fence && fences[k].pending;
}
static struct fake_cmd *cmdOf(VkCommandBuffer c) {
    return (struct fake_cmd *) c == &cmdAux ? &cmdAux : &cmdObj;
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
    cmdObj.allocated = cmdAux.allocated = false;
}
static VKAPI_ATTR VkResult VKAPI_CALL fAllocateCommandBuffers(VkDevice d, const VkCommandBufferAllocateInfo *i, VkCommandBuffer *c) {
    (void) d;
    CHECK(i->commandBufferCount == 1, "fake device: one command buffer");
    if (emu.auxCmdFail && cmdObj.allocated)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    /* the first, the second (P11-C3-B2); a renderer made again without its pool destroyed gets the first again */
    *c = (VkCommandBuffer) (cmdObj.allocated && !cmdAux.allocated ? &cmdAux : &cmdObj);
    cmdOf(*c)->allocated = true;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL fResetCommandBuffer(VkCommandBuffer c, VkCommandBufferResetFlags f) {
    (void) f;
    pendingSync();
    if (cmdOf(c)->pending)
        violation("a command buffer reset while its submission runs");
    cmdOf(c)->n = 0;
    cmdOf(c)->recording = false;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL fBeginCommandBuffer(VkCommandBuffer c, const VkCommandBufferBeginInfo *i) {
    (void) i;
    pendingSync();
    if (cmdOf(c)->pending)
        violation("a command buffer begun while its submission runs");
    recordingCmd = cmdOf(c);
    recordingCmd->n = 0;
    recordingCmd->recording = true;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL fEndCommandBuffer(VkCommandBuffer c) {
    cmdOf(c)->recording = false;
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
    struct fake_op op = { .kind = OP_COPY_IMAGE, .a = s, .la = sl, .b = t, .lb = tl, .extent = r->extent, .nregions = n };
    (void) c;
    CHECK(n >= 1 && n <= 16, "fake device: %u regions", n);
    memcpy(op.regions, r, (n <= 16 ? n : 16) * sizeof *r);
    emu.copyCommands++;
    emu.copyRegions += n;
    record(op);
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
static VkImageUsageFlags usageOf(VkImage image) {
    for (int i = 0; i < 64; i++)
        if (boundImages[i].image == image)
            return boundImages[i].usage;
    return 0;
}
/* a region of a copy: the color aspect of mip 0, layer 0, a 2D extent of at least one pixel, all of it in the image */
static bool inside(VkImageSubresourceLayers s, VkOffset3D o, VkExtent3D e, const AHardwareBuffer *b) {
    return s.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && s.mipLevel == 0 && s.baseArrayLayer == 0 && s.layerCount == 1 &&
           o.z == 0 && e.depth == 1 && e.width && e.height && o.x >= 0 && o.y >= 0 &&
           (uint64_t) o.x + e.width <= b->desc.width && (uint64_t) o.y + e.height <= b->desc.height;
}
static AHardwareBuffer *passTarget;
static VkDescriptorSet boundSet;
static void run(const struct fake_op *op) {
    AHardwareBuffer *a = op->a ? ahbOf(op->a) : NULL, *b = op->b ? ahbOf(op->b) : NULL, *x;
    const VkImageMemoryBarrier *m = &op->image;
    switch (op->kind) {
    case OP_IMAGE_BARRIER:
        if (!(x = ahbOf(m->image))) {
            if (m->srcQueueFamilyIndex != m->dstQueueFamilyIndex)
                violation("ownership transfer of an image that is no X server buffer");
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
        if (!(usageOf(op->a) & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || !(usageOf(op->b) & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
            violation("copy with an image made without the transfer usage");
            return;
        }
        /* one format, or B8G8R8A8_UNORM into R8G8B8A8_UNORM: size-compatible, both of the 32-bit class
         * (VUID-vkCmdCopyImage-srcImage-01548), copied as raw bits - the one pair of two formats the renderer copies */
        if (!a || !b || a == b || (a->vkFormat != b->vkFormat &&
                                   !(a->vkFormat == VK_FORMAT_B8G8R8A8_UNORM && b->vkFormat == VK_FORMAT_R8G8B8A8_UNORM))) {
            violation("copy between images not two X server buffers of one format or of B8G8R8A8 into R8G8B8A8");
            return;
        }
        if (!usable(a, op->la, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, (VkExtent3D) { a->desc.width, a->desc.height, 1 }) ||
            !usable(b, op->lb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (VkExtent3D) { b->desc.width, b->desc.height, 1 }))
            return;
        for (uint32_t i = 0; i < op->nregions; i++) {
            const VkImageCopy *g = &op->regions[i];
            if (!inside(g->srcSubresource, g->srcOffset, g->extent, a) || !inside(g->dstSubresource, g->dstOffset, g->extent, b)) {
                violation("copy region not inside both images");
                return;
            }
            for (uint32_t j = 0; j < i; j++)
                if (g->dstOffset.x < op->regions[j].dstOffset.x + (int32_t) op->regions[j].extent.width &&
                    op->regions[j].dstOffset.x < g->dstOffset.x + (int32_t) g->extent.width &&
                    g->dstOffset.y < op->regions[j].dstOffset.y + (int32_t) op->regions[j].extent.height &&
                    op->regions[j].dstOffset.y < g->dstOffset.y + (int32_t) g->extent.height) {
                    violation("copy regions overlapping in the destination (written in no order)");
                    return;
                }
        }
        for (uint32_t i = 0; i < op->nregions; i++) {
            const VkImageCopy *g = &op->regions[i];
            for (uint32_t y = 0; y < g->extent.height; y++) {
                uint8_t *to = b->gpu + ((size_t) (g->dstOffset.y + y) * b->desc.width + g->dstOffset.x) * 4;
                memcpy(to, a->gpu + ((size_t) (g->srcOffset.y + y) * a->desc.width + g->srcOffset.x) * 4, (size_t) g->extent.width * 4);
                if (cfg.copyConverts && a->vkFormat != b->vkFormat)
                    for (uint32_t x = 0; x < g->extent.width; x++) {
                        uint8_t t = to[x * 4];
                        to[x * 4] = to[x * 4 + 2], to[x * 4 + 2] = t;
                    }
            }
        }
        copied(b->gpu);
        return;
    case OP_BUFFER_TO_IMAGE:
        if (!b && op->b)
            return;     /* the renderer's own image (the cursor) */
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
    case OP_BEGIN_PASS:
        passTarget = ahbOf(imageOfFramebuffer(op->framebuffer));
        if (passTarget && (!passTarget->owned || passTarget->layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL))
            violation("render pass into an X server buffer not owned, or not in COLOR_ATTACHMENT_OPTIMAL");
        if (passTarget)
            emu.passesIntoImported++;
        return;
    case OP_END_PASS:
        passTarget = NULL;
        return;
    case OP_BIND_SET:
        boundSet = op->set;
        return;
    case OP_DRAW: {
        VkImage sampled;
        const VkImageLayout *layout = layoutOfSet(boundSet, &sampled);
        AHardwareBuffer *source = ahbOf(sampled);
        if (!layout)
            violation("draw without a descriptor set");
        else if (source && (!source->owned || source->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
                            *layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
            violation("draw sampling an X server buffer not owned, or not in SHADER_READ_ONLY_OPTIMAL");
        emu.drawnFrom[emu.draws % 64] = source;
        emu.drawnInto[emu.draws % 64] = passTarget;
        emu.draws++;
        if (source)
            emu.drawsFromImported++;
        if (passTarget)
            emu.drawsIntoImported++;
        return;
    }
    }
}
static bool *signaled(VkFence f) {
    for (int i = 0; i < 16; i++)
        if (f && fences[i].fence == f)
            return &fences[i].signaled;
    return NULL;
}
/* binary semaphores signalled and not yet waited on: a signal must find its semaphore unsignalled, a wait (a
 * submission's, a present's) one a submission signalled - and leaves it unsignalled */
static VkSemaphore semaphoresSignalled[32];
static bool semaphoreSignalled(VkSemaphore s, int set) {
    int i, free = -1;
    for (i = 0; i < 32; i++) {
        if (s && semaphoresSignalled[i] == s) {
            if (set == 0)
                semaphoresSignalled[i] = VK_NULL_HANDLE;
            return true;
        }
        if (!semaphoresSignalled[i] && free < 0)
            free = i;
    }
    if (set == 1 && free >= 0)
        semaphoresSignalled[free] = s;
    return false;
}
static bool *pendingOf(VkFence f) {
    for (int i = 0; i < 16; i++)
        if (f && fences[i].fence == f)
            return &fences[i].pending;
    return NULL;
}
static VKAPI_ATTR VkResult VKAPI_CALL fQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *s, VkFence f) {
    struct fake_cmd *c;
    (void) q;
    pendingSync();
    if (emu.submitResult != VK_SUCCESS)
        return emu.submitResult;
    if (emu.submitFailAfter > 0 && --emu.submitFailAfter == 0)
        return VK_ERROR_DEVICE_LOST;
    emu.submits++;
    for (uint32_t i = 0; i < s->waitSemaphoreCount; i++)
        if (!semaphoreSignalled(s->pWaitSemaphores[i], 0))
            violation("a submission waiting for a semaphore nothing signalled");
    for (uint32_t i = 0; i < s->signalSemaphoreCount; i++)
        if (semaphoreSignalled(s->pSignalSemaphores[i], 1))
            violation("a submission signalling a semaphore already signalled");
    if (f && signaled(f) && *signaled(f))
        violation("submission with a signalled fence");
    /* P11-C3-B2: two may run at once - each its own command buffer and fence: none may be reused while it runs; a
     * submission without command buffers (a semaphore wait alone) is one too */
    c = s->commandBufferCount ? cmdOf(s->pCommandBuffers[0]) : NULL;
    CHECK(n == 1 && s->commandBufferCount <= 1 && (!c || ((s->pCommandBuffers[0] == (VkCommandBuffer) &cmdObj ||
          s->pCommandBuffers[0] == (VkCommandBuffer) &cmdAux) && !c->recording)),
          "fake device: submission of a command buffer still being recorded");
    if (c && c->pending)
        violation("a command buffer submitted again while its submission runs");
    if (f && pendingOf(f) && *pendingOf(f))
        violation("submission with the fence of a submission still running");
    for (int i = 0; c && i < c->n; i++) {
        run(&c->ops[i]);
        /* the host reads a download after the fence: only a barrier to HOST_READ makes it visible there */
        if (c->ops[i].kind == OP_IMAGE_TO_BUFFER) {
            bool barrier = false;
            for (int j = i + 1; j < c->n; j++)
                if (c->ops[j].kind == OP_BUFFER_BARRIER && c->ops[j].buffer.buffer == c->ops[i].buf &&
                    (c->ops[j].buffer.srcAccessMask & VK_ACCESS_TRANSFER_WRITE_BIT) &&
                    (c->ops[j].buffer.dstAccessMask & VK_ACCESS_HOST_READ_BIT) &&
                    (c->ops[j].src & VK_PIPELINE_STAGE_TRANSFER_BIT) && (c->ops[j].dst & VK_PIPELINE_STAGE_HOST_BIT))
                    barrier = true;
            if (barrier)
                emu.hostBarriers++;
            else
                violation("a copy into host memory with no barrier making it visible to the host");
        }
    }
    if (c) {
        /* the descriptor pools its sets came from are in use while it runs */
        int np = 0;
        memset(c->pools, 0, sizeof c->pools);
        for (int i = 0; i < c->n && np < 4; i++)
            if (c->ops[i].kind == OP_BIND_SET)
                for (int k = 0; k < 64; k++)
                    if (descriptorSets[k].set && descriptorSets[k].set == c->ops[i].set) {
                        bool known = false;
                        for (int m = 0; m < np; m++)
                            known |= c->pools[m] == descriptorSets[k].pool;
                        if (!known)
                            c->pools[np++] = descriptorSets[k].pool;
                    }
        c->pending = f != VK_NULL_HANDLE;
        c->fence = f;
    }
    if (f && pendingOf(f))
        *pendingOf(f) = true;
    pendingRecount();
    if (f && signaled(f))
        *signaled(f) = true;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL fCreateFence(VkDevice d, const VkFenceCreateInfo *i, const VkAllocationCallbacks *a, VkFence *f) {
    (void) d; (void) a;
    emu.fences++;
    *f = (VkFence) nextHandle++;
    for (int k = 0; k < 16; k++)
        if (!fences[k].fence) {
            fences[k].fence = *f;
            fences[k].signaled = i->flags & VK_FENCE_CREATE_SIGNALED_BIT;
            break;
        }
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL fDestroyFence(VkDevice d, VkFence f, const VkAllocationCallbacks *a) {
    (void) d; (void) a;
    emu.fences--;
    for (int k = 0; k < 16; k++)
        if (fences[k].fence == f)
            memset(&fences[k], 0, sizeof fences[k]);
}
static void (*emuFenceWait)(VkFence);  /* a test's look at the moment the renderer waits for a fence */
static VKAPI_ATTR VkResult VKAPI_CALL fWaitForFences(VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t timeout) {
    (void) d; (void) n; (void) f; (void) all;
    pendingSync();
    if (emuFenceWait && n)
        emuFenceWait(f[0]);
    CHECK(timeout != UINT64_MAX, "fake device: an unbounded fence wait");
    if (emu.fenceResult != VK_SUCCESS)
        return emu.fenceResult;
    for (uint32_t i = 0; i < n; i++) {
        if (signaled(f[i]) && !*signaled(f[i]))
            violation("waiting for a fence nothing signals");
        if (pendingOf(f[i]))
            *pendingOf(f[i]) = false;
        if (cmdObj.fence == f[i])
            cmdObj.pending = false;
        if (cmdAux.fence == f[i])
            cmdAux.pending = false;
    }
    pendingRecount();
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL fResetFences(VkDevice d, uint32_t n, const VkFence *f) {
    (void) d;
    pendingSync();
    for (uint32_t i = 0; i < n; i++)
        if (pendingOf(f[i]) && *pendingOf(f[i]))
            violation("a fence reset while its submission runs");
    for (uint32_t i = 0; i < n; i++)
        if (signaled(f[i]))
            *signaled(f[i]) = false;
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
/* ---- presenting: surface, swapchain, and what a frame is drawn with ---- */
#define MADE(type, out) do { if (++emu.objects == emu.failObject) return VK_ERROR_OUT_OF_HOST_MEMORY; \
                             emu.live++; *(out) = (type) nextHandle++; } while (0)
#define GONE(h) do { if (h) emu.live--; } while (0)
static VKAPI_ATTR VkResult VKAPI_CALL fCreateAndroidSurfaceKHR(VkInstance i, const VkAndroidSurfaceCreateInfoKHR *info,
                                                               const VkAllocationCallbacks *a, VkSurfaceKHR *out) {
    (void) i; (void) a;
    CHECK(info->window, "fake device: a surface without a window");
    emu.surfaces++;
    *out = (VkSurfaceKHR) nextHandle++;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL fDestroySurfaceKHR(VkInstance i, VkSurfaceKHR s, const VkAllocationCallbacks *a) {
    (void) i; (void) a;
    CHECK(s, "fake device: destroying a null surface");
    CHECK(emu.swapchains == 0, "fake device: surface destroyed before its swapchain");
    emu.surfaces--;
}
static VKAPI_ATTR VkResult VKAPI_CALL fGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice pd, uint32_t family, VkSurfaceKHR s, VkBool32 *ok) {
    (void) pd; (void) s;
    *ok = family == 1 && !emu.surfaceUnsupported;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL fGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice pd, VkSurfaceKHR s, VkSurfaceCapabilitiesKHR *c) {
    (void) pd; (void) s;
    *c = emu.caps;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL fGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice pd, VkSurfaceKHR s, uint32_t *n, VkSurfaceFormatKHR *f) {
    (void) pd; (void) s;
    if (f)
        for (uint32_t i = 0; i < emu.surfaceFormatCount && i < *n; i++)
            f[i] = emu.surfaceFormats[i];
    *n = emu.surfaceFormatCount;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL fGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice pd, VkSurfaceKHR s, uint32_t *n, VkPresentModeKHR *m) {
    (void) pd; (void) s;
    if (m)
        for (uint32_t i = 0; i < emu.presentModeCount && i < *n; i++)
            m[i] = emu.presentModes[i];
    *n = emu.presentModeCount;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL fCreateSwapchainKHR(VkDevice d, const VkSwapchainCreateInfoKHR *info, const VkAllocationCallbacks *a,
                                                          VkSwapchainKHR *out) {
    (void) d; (void) a;
    emu.swapchainInfo = *info;
    if (++emu.objects == emu.failObject)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    emu.swapchains++;
    emu.swapchainsMade++;
    *out = (VkSwapchainKHR) nextHandle++;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL fDestroySwapchainKHR(VkDevice d, VkSwapchainKHR s, const VkAllocationCallbacks *a) {
    (void) d; (void) a;
    CHECK(s, "fake device: destroying a null swapchain");
    emu.swapchains--;
}
static VKAPI_ATTR VkResult VKAPI_CALL fGetSwapchainImagesKHR(VkDevice d, VkSwapchainKHR s, uint32_t *n, VkImage *images) {
    (void) d; (void) s;
    uint32_t count = emu.swapchainInfo.minImageCount;
    if (images)
        for (uint32_t i = 0; i < count && i < *n; i++)
            images[i] = (VkImage) (uintptr_t) (0x900000 + i);    /* the presentation engine's: no X server buffer */
    *n = count;
    return VK_SUCCESS;
}
static void (*emuOnAcquire)(void);   /* a test's moment while the renderer acquires an image */
static VKAPI_ATTR VkResult VKAPI_CALL fAcquireNextImageKHR(VkDevice d, VkSwapchainKHR s, uint64_t timeout, VkSemaphore sem, VkFence f, uint32_t *index) {
    (void) d; (void) s;
    if (emuOnAcquire)
        emuOnAcquire();
    CHECK(timeout != UINT64_MAX && !sem && f, "fake device: acquire with an unbounded wait, a semaphore, or no fence");
    emu.acquires2++;
    if (signaled(f) && *signaled(f))
        violation("acquire with a signalled fence (it was not reset)");
    if (emu.acquireResult != VK_SUCCESS && emu.acquireResult != VK_SUBOPTIMAL_KHR)
        return emu.acquireResult;
    if (signaled(f))
        *signaled(f) = true;
    *index = emu.nextImage++ % emu.swapchainInfo.minImageCount;
    return emu.acquireResult;
}
static VKAPI_ATTR VkResult VKAPI_CALL fQueuePresentKHR(VkQueue q, const VkPresentInfoKHR *info) {
    (void) q;
    CHECK(info->waitSemaphoreCount == 1 && info->swapchainCount == 1, "fake device: present without its semaphore");
    /* the presentation engine waits on the GPU for what the semaphore says: the frame's fence need not be waited for */
    if (!semaphoreSignalled(info->pWaitSemaphores[0], 0))
        violation("present waiting for a semaphore no submission signalled");
    if (emu.fencePending)
        emu.earlyPresents++;
    emu.presents++;
    return emu.presentResult;
}
static VKAPI_ATTR VkResult VKAPI_CALL fCreateRenderPass(VkDevice d, const VkRenderPassCreateInfo *i, const VkAllocationCallbacks *a, VkRenderPass *p) {
    (void) d; (void) a;
    CHECK(i->attachmentCount == 1, "fake device: one attachment");
    if (i->pAttachments[0].format != VK_FORMAT_B8G8R8A8_UNORM && i->pAttachments[0].format != VK_FORMAT_R8G8B8A8_UNORM)
        violation("render pass of an unexpected format");
    if (i->pAttachments[0].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD && i->pAttachments[0].initialLayout == VK_IMAGE_LAYOUT_UNDEFINED)
        violation("contents loaded from UNDEFINED");
    MADE(VkRenderPass, p);
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL fDestroyRenderPass(VkDevice d, VkRenderPass p, const VkAllocationCallbacks *a) { (void) d; (void) a; GONE(p); }
static VKAPI_ATTR VkResult VKAPI_CALL fCreateFramebuffer(VkDevice d, const VkFramebufferCreateInfo *i, const VkAllocationCallbacks *a, VkFramebuffer *f) {
    (void) d; (void) a;
    MADE(VkFramebuffer, f);
    for (int k = 0; k < 64; k++)
        if (!framebuffers[k].framebuffer) {
            framebuffers[k].framebuffer = *f;
            framebuffers[k].view = i->pAttachments[0];
            break;
        }
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL fDestroyFramebuffer(VkDevice d, VkFramebuffer f, const VkAllocationCallbacks *a) {
    (void) d; (void) a;
    GONE(f);
    for (int k = 0; k < 64; k++)
        if (framebuffers[k].framebuffer == f)
            memset(&framebuffers[k], 0, sizeof framebuffers[k]);
}
static VKAPI_ATTR VkResult VKAPI_CALL fCreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo *i, const VkAllocationCallbacks *a, VkShaderModule *m) {
    (void) d; (void) a;
    CHECK(i->codeSize % 4 == 0 && i->codeSize >= 20 && i->pCode[0] == 0x07230203, "fake device: not SPIR-V");
    MADE(VkShaderModule, m);
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL fDestroyShaderModule(VkDevice d, VkShaderModule m, const VkAllocationCallbacks *a) { (void) d; (void) a; GONE(m); }
static VKAPI_ATTR VkResult VKAPI_CALL fCreatePipelineLayout(VkDevice d, const VkPipelineLayoutCreateInfo *i, const VkAllocationCallbacks *a, VkPipelineLayout *l) {
    (void) d; (void) a;
    CHECK(i->pushConstantRangeCount == 1 && i->pPushConstantRanges[0].size == sizeof(struct lorie_vk_quad), "fake device: push constants");
    MADE(VkPipelineLayout, l);
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL fDestroyPipelineLayout(VkDevice d, VkPipelineLayout l, const VkAllocationCallbacks *a) { (void) d; (void) a; GONE(l); }
static VKAPI_ATTR VkResult VKAPI_CALL fCreateGraphicsPipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *i,
                                                               const VkAllocationCallbacks *a, VkPipeline *p) {
    (void) d; (void) c; (void) a;
    CHECK(n == 1 && i->renderPass && i->layout, "fake device: pipeline");
    MADE(VkPipeline, p);
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL fDestroyPipeline(VkDevice d, VkPipeline p, const VkAllocationCallbacks *a) { (void) d; (void) a; GONE(p); }
static VKAPI_ATTR VkResult VKAPI_CALL fCreateDescriptorSetLayout(VkDevice d, const VkDescriptorSetLayoutCreateInfo *i, const VkAllocationCallbacks *a,
                                                                 VkDescriptorSetLayout *l) {
    (void) d; (void) i; (void) a;
    MADE(VkDescriptorSetLayout, l);
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL fDestroyDescriptorSetLayout(VkDevice d, VkDescriptorSetLayout l, const VkAllocationCallbacks *a) { (void) d; (void) a; GONE(l); }
static VKAPI_ATTR VkResult VKAPI_CALL fCreateDescriptorPool(VkDevice d, const VkDescriptorPoolCreateInfo *i, const VkAllocationCallbacks *a, VkDescriptorPool *p) {
    (void) d; (void) i; (void) a;
    MADE(VkDescriptorPool, p);
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL fDestroyDescriptorPool(VkDevice d, VkDescriptorPool p, const VkAllocationCallbacks *a) { (void) d; (void) a; GONE(p); }
static VKAPI_ATTR VkResult VKAPI_CALL fResetDescriptorPool(VkDevice d, VkDescriptorPool p, VkDescriptorPoolResetFlags f) {
    (void) d; (void) f;
    pendingSync();
    for (int m = 0; m < 4; m++)
        if ((cmdObj.pending && cmdObj.pools[m] == p) || (cmdAux.pending && cmdAux.pools[m] == p))
            violation("a descriptor pool reset while a submission using its sets runs");
    for (int k = 0; k < 64; k++)
        if (descriptorSets[k].pool == p)
            memset(&descriptorSets[k], 0, sizeof descriptorSets[k]);
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL fAllocateDescriptorSets(VkDevice d, const VkDescriptorSetAllocateInfo *i, VkDescriptorSet *s) {
    int k, inPool = 0;
    (void) d;
    for (k = 0; k < 64; k++)
        inPool += descriptorSets[k].set && descriptorSets[k].pool == i->descriptorPool;
    if (i->descriptorSetCount != 1 || inPool == 32)
        return VK_ERROR_OUT_OF_POOL_MEMORY;
    for (k = 0; k < 64 && descriptorSets[k].set; k++)
        ;
    if (k == 64)
        return VK_ERROR_OUT_OF_POOL_MEMORY;
    *s = (VkDescriptorSet) nextHandle++;
    descriptorSets[k].set = *s;
    descriptorSets[k].pool = i->descriptorPool;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL fUpdateDescriptorSets(VkDevice d, uint32_t n, const VkWriteDescriptorSet *w, uint32_t nc, const VkCopyDescriptorSet *c) {
    (void) d; (void) c;
    CHECK(nc == 0, "fake device: descriptor copies");
    for (uint32_t i = 0; i < n; i++)
        for (int k = 0; k < 64; k++)
            if (descriptorSets[k].set == w[i].dstSet) {
                descriptorSets[k].view = w[i].pImageInfo->imageView;
                descriptorSets[k].layout = w[i].pImageInfo->imageLayout;
            }
}
static VKAPI_ATTR VkResult VKAPI_CALL fCreateSampler(VkDevice d, const VkSamplerCreateInfo *i, const VkAllocationCallbacks *a, VkSampler *s) {
    (void) d; (void) i; (void) a;
    MADE(VkSampler, s);
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL fDestroySampler(VkDevice d, VkSampler s, const VkAllocationCallbacks *a) { (void) d; (void) a; GONE(s); }
static VKAPI_ATTR VkResult VKAPI_CALL fCreateSemaphore(VkDevice d, const VkSemaphoreCreateInfo *i, const VkAllocationCallbacks *a, VkSemaphore *s) {
    (void) d; (void) i; (void) a;
    MADE(VkSemaphore, s);
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL fDestroySemaphore(VkDevice d, VkSemaphore s, const VkAllocationCallbacks *a) {
    (void) d; (void) a;
    semaphoreSignalled(s, 0);
    GONE(s);
}
static VKAPI_ATTR void VKAPI_CALL fGetImageMemoryRequirements(VkDevice d, VkImage i, VkMemoryRequirements *r) {
    (void) d; (void) i;
    *r = (VkMemoryRequirements) { 512 * 512 * 4, 256, 0x7 };
}
static VKAPI_ATTR void VKAPI_CALL fCmdBeginRenderPass(VkCommandBuffer c, const VkRenderPassBeginInfo *i, VkSubpassContents k) {
    (void) c; (void) k;
    record((struct fake_op) { .kind = OP_BEGIN_PASS, .framebuffer = i->framebuffer });
}
static VKAPI_ATTR void VKAPI_CALL fCmdEndRenderPass(VkCommandBuffer c) {
    (void) c;
    record((struct fake_op) { .kind = OP_END_PASS });
}
static VKAPI_ATTR void VKAPI_CALL fCmdBindPipeline(VkCommandBuffer c, VkPipelineBindPoint b, VkPipeline p) {
    (void) c; (void) b;
    CHECK(p, "fake device: binding no pipeline");
}
static VKAPI_ATTR void VKAPI_CALL fCmdBindDescriptorSets(VkCommandBuffer c, VkPipelineBindPoint b, VkPipelineLayout l, uint32_t first, uint32_t n,
                                                         const VkDescriptorSet *sets, uint32_t nd, const uint32_t *dyn) {
    (void) c; (void) b; (void) l; (void) first; (void) nd; (void) dyn;
    CHECK(n == 1, "fake device: one set");
    record((struct fake_op) { .kind = OP_BIND_SET, .set = sets[0] });
}
static struct lorie_vk_quad pushed[64];
static int pushes;
static VKAPI_ATTR void VKAPI_CALL fCmdPushConstants(VkCommandBuffer c, VkPipelineLayout l, VkShaderStageFlags st, uint32_t off, uint32_t size, const void *v) {
    (void) c; (void) l; (void) st;
    CHECK(off == 0 && size == sizeof(struct lorie_vk_quad), "fake device: push constants");
    if (pushes < 64)
        memcpy(&pushed[pushes++], v, sizeof(struct lorie_vk_quad));
}
static VKAPI_ATTR void VKAPI_CALL fCmdSetViewport(VkCommandBuffer c, uint32_t f, uint32_t n, const VkViewport *v) { (void) c; (void) f; (void) n; (void) v; }
static VKAPI_ATTR void VKAPI_CALL fCmdSetScissor(VkCommandBuffer c, uint32_t f, uint32_t n, const VkRect2D *r) { (void) c; (void) f; (void) n; (void) r; }
static VKAPI_ATTR void VKAPI_CALL fCmdDraw(VkCommandBuffer c, uint32_t v, uint32_t i, uint32_t fv, uint32_t fi) {
    (void) c; (void) fv; (void) fi;
    CHECK(v == 4 && i == 1, "fake device: a quad");
    record((struct fake_op) { .kind = OP_DRAW });
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
    F(InvalidateMappedMemoryRanges) F(GetImageMemoryRequirements) F(CreateSwapchainKHR) F(DestroySwapchainKHR)
    F(GetSwapchainImagesKHR) F(AcquireNextImageKHR) F(QueuePresentKHR) F(CreateRenderPass) F(DestroyRenderPass)
    F(CreateFramebuffer) F(DestroyFramebuffer) F(CreateShaderModule) F(DestroyShaderModule) F(CreatePipelineLayout)
    F(DestroyPipelineLayout) F(CreateGraphicsPipelines) F(DestroyPipeline) F(CreateDescriptorSetLayout)
    F(DestroyDescriptorSetLayout) F(CreateDescriptorPool) F(DestroyDescriptorPool) F(ResetDescriptorPool)
    F(AllocateDescriptorSets) F(UpdateDescriptorSets) F(CreateSampler) F(DestroySampler) F(CreateSemaphore)
    F(DestroySemaphore) F(CmdBeginRenderPass) F(CmdEndRenderPass) F(CmdBindPipeline) F(CmdBindDescriptorSets)
    F(CmdPushConstants) F(CmdSetViewport) F(CmdSetScissor) F(CmdDraw)
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
    F(GetPhysicalDeviceImageFormatProperties2) F(GetPhysicalDeviceFormatProperties) F(CreateAndroidSurfaceKHR) F(DestroySurfaceKHR)
    F(GetPhysicalDeviceSurfaceSupportKHR) F(GetPhysicalDeviceSurfaceCapabilitiesKHR) F(GetPhysicalDeviceSurfaceFormatsKHR)
    F(GetPhysicalDeviceSurfacePresentModesKHR)
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
    cfg.optimalFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                          VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    emu.surfaceFormats[0] = (VkSurfaceFormatKHR) { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
    emu.surfaceFormats[1] = (VkSurfaceFormatKHR) { VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
    emu.surfaceFormatCount = 2;
    emu.presentModes[0] = VK_PRESENT_MODE_FIFO_KHR;
    emu.presentModeCount = 1;
    emu.caps = (VkSurfaceCapabilitiesKHR) {
            .minImageCount = 2, .maxImageCount = 4, .currentExtent = { 1080, 2400 }, .minImageExtent = { 1, 1 },
            .maxImageExtent = { 4096, 4096 }, .maxImageArrayLayers = 1,
            .supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR,
            .currentTransform = VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR,
            .supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR | VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
            .supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT };
}
