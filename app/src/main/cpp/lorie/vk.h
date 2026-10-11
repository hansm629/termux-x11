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
    X(GetPhysicalDeviceMemoryProperties) X(CreateDevice) X(GetDeviceProcAddr) X(GetPhysicalDeviceImageFormatProperties2) \
    X(CreateAndroidSurfaceKHR) X(DestroySurfaceKHR) X(GetPhysicalDeviceSurfaceSupportKHR) \
    X(GetPhysicalDeviceSurfaceCapabilitiesKHR) X(GetPhysicalDeviceSurfaceFormatsKHR)
#define LORIE_VK_DEVICE_FNS(X) X(DestroyDevice) X(GetDeviceQueue) X(DeviceWaitIdle) \
    X(GetAndroidHardwareBufferPropertiesANDROID) X(CreateImage) X(DestroyImage) X(AllocateMemory) X(FreeMemory) \
    X(BindImageMemory) X(CreateImageView) X(DestroyImageView) \
    X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(ResetCommandBuffer) \
    X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdPipelineBarrier) X(CmdCopyImage) X(CmdCopyBufferToImage) \
    X(CmdCopyImageToBuffer) X(QueueSubmit) X(CreateFence) X(DestroyFence) X(WaitForFences) X(ResetFences) \
    X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) X(MapMemory) \
    X(UnmapMemory) X(FlushMappedMemoryRanges) X(InvalidateMappedMemoryRanges) X(GetImageMemoryRequirements) \
    X(CreateSwapchainKHR) X(DestroySwapchainKHR) X(GetSwapchainImagesKHR) X(AcquireNextImageKHR) X(QueuePresentKHR) \
    X(CreateRenderPass) X(DestroyRenderPass) X(CreateFramebuffer) X(DestroyFramebuffer) X(CreateShaderModule) \
    X(DestroyShaderModule) X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateGraphicsPipelines) X(DestroyPipeline) \
    X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreateDescriptorPool) X(DestroyDescriptorPool) \
    X(ResetDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreateSampler) X(DestroySampler) \
    X(CreateSemaphore) X(DestroySemaphore) X(CmdBeginRenderPass) X(CmdEndRenderPass) X(CmdBindPipeline) \
    X(CmdBindDescriptorSets) X(CmdPushConstants) X(CmdSetViewport) X(CmdSetScissor) X(CmdDraw)

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
    uint8_t verifiedFormats;    /* AHB formats the ownership self-test passed: 1 RGBA, 2 RGBX, 4 BGRA */
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
 * with external memory. It is taken over from, and given back to, the X server by the barriers below.
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
    VkFramebuffer framebuffer;  /* for rendering into it (GPU copies): made when first needed */
};

/*
 * The renderer's images, each allocated on its own: a pointer lorieVkImport returned stays valid until that image
 * is removed, however many are imported after it - a frame keeps the images it uses (lorieVkFrameUse) while it
 * imports more. Only the array of pointers grows.
 */
struct lorie_vk_images {
    struct lorie_vk_image **items;
    uint32_t count, capacity;
    uint8_t lastReason;         /* why the last refused import was refused (LORIE_FALLBACK_*) */
    VkResult lastResult;
};

/*
 * Ownership of the X server's AHardwareBuffers. The specification makes AHardwareBuffer_lock/_unlock an
 * acquire/release of ownership by a foreign device (Android Hardware Buffer, External Resource Sharing), so
 * the renderer must acquire every image from VK_QUEUE_FAMILY_FOREIGN_EXT before using it, and release it back
 * when done, its release complete (the frame's fence) before the X server may lock the buffer again.
 *
 * Which layout an AHardwareBuffer is in while foreign owned, the normative specification does not say: its
 * implied-layout table lists D3D11 handles only; GENERAL rests on VK_KHR_external_memory's resolved issue 3
 * (informative: "Vulkan-incompatible APIs will require the image to be in the GENERAL layout whenever they are
 * accessing them"). Stage 1 therefore takes, as an implementation policy and not as a specification rule:
 *
 *     foreign (X server / CPU) owned  =  VK_QUEUE_FAMILY_FOREIGN_EXT, VK_IMAGE_LAYOUT_GENERAL
 *
 * Every acquire starts from GENERAL - never from VK_IMAGE_LAYOUT_UNDEFINED, which may discard the contents the
 * CPU defined - and every release ends in GENERAL. Within the renderer's queue an image is in the layout of
 * its use. The device self-test checks the policy byte for byte; nothing else is tried if it fails.
 */
enum lorie_vk_use {
    LORIE_VK_USE_SAMPLE,        // read by a fragment shader
    LORIE_VK_USE_RENDER,        // colour attachment, loaded and stored
    LORIE_VK_USE_TRANSFER_SRC,
    LORIE_VK_USE_TRANSFER_DST,
};
struct lorie_vk_barrier {
    VkImageMemoryBarrier barrier;
    VkPipelineStageFlags srcStage, dstStage;
};
// Acquire from the foreign owner for `use`: FOREIGN/GENERAL to the renderer's queue family in use's layout.
struct lorie_vk_barrier lorieVkAcquireBarrier(const struct lorie_vk *vk, VkImage image, enum lorie_vk_use use);
// Release to the foreign owner after `use`: from use's layout to FOREIGN/GENERAL.
struct lorie_vk_barrier lorieVkReleaseBarrier(const struct lorie_vk *vk, VkImage image, enum lorie_vk_use use);
// Within the renderer's queue, from one use to another.
struct lorie_vk_barrier lorieVkUseBarrier(VkImage image, enum lorie_vk_use from, enum lorie_vk_use to);

/*
 * The device self-test of the ownership policy above, run before the backend uses any X server buffer. For
 * RGBA, RGBX and BGRA, on two AHardwareBuffers allocated and locked as the X server's are, each reused for
 * every iteration, `iterations` times each way:
 *   CPU -> Vulkan -> CPU    the CPU writes a pattern into one, Vulkan copies it into the other, the CPU reads it
 *   Vulkan -> CPU -> Vulkan Vulkan uploads a pattern, the CPU reads it and writes another, Vulkan reads that back
 * with every acquire and release as above, the pattern changing every iteration (zero, all-one, checkerboard,
 * row-dependent, pseudo-random), each read compared byte for byte - except an RGBX buffer's X byte: the
 * specification makes the X/A component Vulkan reads after an external write undefined (AHardwareBuffer
 * Format Equivalence, note 1), so it cannot be compared; the renderer reads it as one (sampleView) instead.
 * The compare counter moves only on a passing compare; one mismatch, or any failed step, fails it. RGBA and
 * RGBX must pass; BGRA may be unavailable (not allocatable, or a format the backend does not import), which is
 * recorded, never counted as passed. Only the formats that passed are imported afterwards (verifiedFormats):
 * an X server buffer of an unverified format is refused like an unsupported one. On failure the fallback is
 * recorded (FATAL_BACKEND, VK_SELFTEST) with where it failed; nothing else is tried.
 */
#define LORIE_VK_SELFTEST_ITERATIONS 1000
// The quick one the backend runs when it starts, unless the full one is asked for: every pattern kind once each
// way (TERMUX_X11_RENDERER_VK_SELFTEST=full asks for LORIE_VK_SELFTEST_ITERATIONS).
#define LORIE_VK_SELFTEST_QUICK_ITERATIONS 5
bool lorieVkSelftest(struct lorie_vk *vk, volatile struct lorie_shared_server_state *st, uint32_t iterations);

/*
 * Presenting: what the renderer draws a frame with (lorieVkRendererCreate, once per device) and the window's
 * swapchain (lorieVkSwapchainCreate, per window). A frame is done as GLES does it, one at a time:
 *
 *   lorieVkAcquire         swapchain image, outside the X server's lock; its fence waited for (bounded) and reset
 *   - X server's lock taken by the caller -
 *   lorieVkFrameBegin      command buffer and descriptor sets reset
 *   lorieVkFrameCopy       each pending GPU copy: source sampled, destination rendered into
 *   lorieVkFrameCursor     the cursor image refilled, if it changed
 *   lorieVkFrameScreen     the root (and cursor) drawn into the swapchain image
 *   lorieVkFrameQueue      every X server buffer the frame used released, the frame submitted
 *   - the frame's buffers listed in flight (lorie.h gpuInFlight), X server's lock let go by the caller -
 *   lorieVkFrameWait       its fence waited for (bounded; the device idle otherwise)
 *   - the buffers no longer in flight, completed serial published by the caller -
 *   lorieVkPresent
 *
 * Every X server buffer a frame uses is acquired from the foreign owner once, at its first use, moved between
 * uses within the frame, and released at the end (lorieVkFrameUse); the X server may CPU-lock it again only
 * after the frame's fence was waited for: until then it is in flight, and the X server, which may take its lock
 * meanwhile, leaves it alone. Draws counted as using an imported image (source or destination) are added to the
 * diagnostics only after the fence signalled. lorieVkFrameSubmit is lorieVkFrameQueue and lorieVkFrameWait in one.
 */
#define LORIE_VK_MAX_SWAPCHAIN_IMAGES 8
/*
 * The swapchain is made again only when the presentation engine says it is out of date (and when the window
 * itself changes: the activity hands the renderer the surface again on every size or format change). Not on
 * VK_SUBOPTIMAL_KHR, which by definition still presents successfully: Android reports it for every frame whose
 * swapchain is not pre-rotated to the display's current transform - ours never is, as GLES's surface is not -
 * and making it again would give the same swapchain and the same result. More than this many OUT_OF_DATE in a
 * row without a frame presented in between is a failure, not a reason to make it once more.
 */
#define LORIE_VK_MAX_OUT_OF_DATE 3
#define LORIE_VK_MAX_FRAME_IMAGES 32

struct lorie_vk_quad {          /* the shaders' push constants: see shaders/quad.vert */
    float rect[4], uv[4];
    uint32_t flags;
};
#define LORIE_VK_QUAD_SWIZZLE 1u    /* red and blue swapped: GLES's .bgra program */
#define LORIE_VK_QUAD_OPAQUE 2u     /* alpha written as one */

struct lorie_vk_renderer {
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence frameFence, acquireFence;
    VkDescriptorSetLayout setLayout;
    VkPipelineLayout layout;
    VkDescriptorPool descriptors;
    VkSampler samplers[2];          /* nearest, linear */
    VkShaderModule vert, frag;
    VkRenderPass copyPass[2];       /* rendering into an imported buffer: [0] R8G8B8A8, [1] B8G8R8A8 */
    VkPipeline copyPipeline[2];
    /* the cursor: the renderer's own image, filled from the X server's bits through a staging buffer */
    VkImage cursor;
    VkDeviceMemory cursorMemory;
    VkImageView cursorView;
    uint32_t cursorWidth, cursorHeight;
    VkBuffer staging;
    VkDeviceMemory stagingMemory;
    uint8_t *stagingMapped;
    bool stagingCoherent;
    /* the window's swapchain */
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkExtent2D extent;
    uint32_t outOfDateStreak;   /* OUT_OF_DATE results since a frame was last presented */
    uint32_t imageCount;
    VkImage images[LORIE_VK_MAX_SWAPCHAIN_IMAGES];
    VkImageView views[LORIE_VK_MAX_SWAPCHAIN_IMAGES];
    VkFramebuffer framebuffers[LORIE_VK_MAX_SWAPCHAIN_IMAGES];
    VkSemaphore rendered[LORIE_VK_MAX_SWAPCHAIN_IMAGES];
    VkRenderPass screenPass;
    VkPipeline screenPipeline, cursorPipeline;
    bool recreate;                  /* out of date: made again before the next frame */
    /* this frame: the imported images it used and the use each is in; its draws */
    struct { struct lorie_vk_image *img; enum lorie_vk_use use; } used[LORIE_VK_MAX_FRAME_IMAGES];
    uint32_t usedCount, sourceDraws, destinationDraws, draws;
    uint64_t submitCpuUs, fenceWaitUs;
};

enum { LORIE_VK_FRAME_OK, LORIE_VK_FRAME_SKIP, LORIE_VK_FRAME_RECREATE, LORIE_VK_FRAME_FATAL };

bool lorieVkRendererCreate(struct lorie_vk *vk, struct lorie_vk_renderer *r, volatile struct lorie_shared_server_state *st);
void lorieVkRendererDestroy(struct lorie_vk *vk, struct lorie_vk_renderer *r);
// The window's surface and swapchain (made again, keeping the surface, when it is already there).
bool lorieVkSwapchainCreate(struct lorie_vk *vk, struct lorie_vk_renderer *r, struct ANativeWindow *window,
                            volatile struct lorie_shared_server_state *st);
void lorieVkSwapchainDestroy(struct lorie_vk *vk, struct lorie_vk_renderer *r);
int lorieVkAcquire(struct lorie_vk *vk, struct lorie_vk_renderer *r, uint32_t *index, volatile struct lorie_shared_server_state *st);
VkResult lorieVkFrameBegin(struct lorie_vk *vk, struct lorie_vk_renderer *r);
bool lorieVkFrameUse(struct lorie_vk *vk, struct lorie_vk_renderer *r, struct lorie_vk_image *img, enum lorie_vk_use use);
bool lorieVkFrameCopy(struct lorie_vk *vk, struct lorie_vk_renderer *r, struct lorie_vk_image *src, struct lorie_vk_image *dst,
                      const struct lorie_vk_quad *quads, uint32_t count, int filter);
bool lorieVkFrameCursor(struct lorie_vk *vk, struct lorie_vk_renderer *r, const uint32_t *bits, uint32_t width, uint32_t height);
void lorieVkFrameScreen(struct lorie_vk *vk, struct lorie_vk_renderer *r, uint32_t index, struct lorie_vk_image *root,
                        const VkRect2D *viewport, const struct lorie_vk_quad *rootQuad, const struct lorie_vk_quad *cursorQuad,
                        int filter);
// Releases the frame's images, ends and submits it (signalling the swapchain image's semaphore when `present`).
// After VK_SUCCESS the device runs it: lorieVkFrameWait must follow before the next frame. Otherwise nothing was
// submitted and nothing is to be waited for.
VkResult lorieVkFrameQueue(struct lorie_vk *vk, struct lorie_vk_renderer *r, uint32_t index, bool present,
                           volatile struct lorie_shared_server_state *st);
// Waits for the frame lorieVkFrameQueue submitted: VK_SUCCESS once its fence signalled. *over says whether the frame
// is known to be over - fence signalled, or else the device idle or lost - and false only when that was not
// established (vkDeviceWaitIdle out of memory): its buffers may still be in use then.
VkResult lorieVkFrameWait(struct lorie_vk *vk, struct lorie_vk_renderer *r, volatile struct lorie_shared_server_state *st,
                          bool *over);
// lorieVkFrameQueue, then lorieVkFrameWait.
VkResult lorieVkFrameSubmit(struct lorie_vk *vk, struct lorie_vk_renderer *r, uint32_t index, bool present,
                            volatile struct lorie_shared_server_state *st);
int lorieVkPresent(struct lorie_vk *vk, struct lorie_vk_renderer *r, uint32_t index, volatile struct lorie_shared_server_state *st);

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
