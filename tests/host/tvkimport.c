/* TVKIMPORT: importing the X server's AHardwareBuffers as Vulkan images, from the real code (vk.h, vk.c: gen.py
 * "vk"), on the fake Vulkan of fakevk.h, after the real lorieVkCreateDevice made the device.
 *
 *   An importable buffer: the format Vulkan reports for it (R8G8B8A8 for RGBA and RGBX, B8G8R8A8 for BGRA) is
 *   used; vkGetPhysicalDeviceImageFormatProperties2 is asked about exactly the image then created (format,
 *   2D, optimal tiling, usage, AHB handle type) and must allow importing it at its size; the image has the
 *   AHB handle type and initial layout UNDEFINED; its memory is a dedicated import of that buffer, with the
 *   allocation size and a memory type the buffer allows (device-local first); the sampling view reads RGBX
 *   alpha as one, the attachment view is identity and exists only where the buffer allows rendering. Counted
 *   as attempt and success; imported once per id; removable without touching the others.
 *   Not importable - an unsupported or external format, a buffer that is not GPU-sampleable, no allowed
 *   memory type, the format query refusing, any failed call, an injected failure: no image, nothing left
 *   behind, exactly one fallback (RESOURCE_LOCAL, or INJECTED) with its reason and the buffer described; the
 *   next buffer still imports. */
#include "fakevk.h"

static struct lorie_shared_server_state st;
static struct lorie_vk vk;
static struct lorie_vk_images images;

#define X_USAGE (AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | \
                 AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER)

static AHardwareBuffer buffer(uint32_t ahbFormat, VkFormat vkFormat, uint64_t usage) {
    return (AHardwareBuffer) { .desc = { 640, 480, 1, ahbFormat, usage, 640, 0, 0 }, .vkFormat = vkFormat,
                               .memoryTypeBits = 0x7, .allocationSize = 640 * 480 * 4, .propertiesResult = VK_SUCCESS };
}

/* A phone with a device, an empty image cache and no fallback yet. */
static void device(void) {
    lorieVkImagesClear(&vk, &images);
    if (vk.device)
        lorieVkDestroyDevice(&vk);
    memset(&st, 0, sizeof st);
    good();
    CHECK(lorieVkCreateDevice(&vk, fGetInstanceProcAddr, &st), "no device to import into");
    vk.verifiedFormats = 7;     /* as after an ownership self-test that passed every format */
    memset(&imp, 0, sizeof imp);
}

/* Not imported: no image, nothing left behind, one fallback with this class and reason, the buffer described. */
static void refused(const char *what, struct lorie_vk_image *img, const AHardwareBuffer *b, uint64_t id, uint8_t cls, uint8_t reason) {
    CHECK(!img, "%s: imported", what);
    CHECK(!lorieVkImageFind(&images, id), "%s: left in the cache", what);
    CHECK(imp.images == 0 && imp.memories == 0 && imp.views == 0, "%s: left %d images, %d memories, %d views", what,
          imp.images, imp.memories, imp.views);
    CHECK(st.diag.fallbackTotal == 1 && st.diag.fallbackByClass[cls] == 1 && st.diag.fallbackByReason[reason] == 1 &&
          st.diag.lastFallbackClass == cls && st.diag.lastFallbackReason == reason,
          "%s: fallback total %u class %u reason %u (want class %u reason %u)", what, st.diag.fallbackTotal,
          st.diag.lastFallbackClass, st.diag.lastFallbackReason, cls, reason);
    CHECK(st.diag.lastResourceId == id && st.diag.lastResourceFormat == b->desc.format && st.diag.lastResourceUsage == b->desc.usage,
          "%s: buffer not described (id %llu format %u usage %#llx)", what, (unsigned long long) st.diag.lastResourceId,
          st.diag.lastResourceFormat, (unsigned long long) st.diag.lastResourceUsage);
    CHECK(st.diag.vkAhbImportSuccess == 0, "%s: counted as imported", what);
}

static bool identity(VkComponentMapping c) {
    return c.r == VK_COMPONENT_SWIZZLE_IDENTITY && c.g == VK_COMPONENT_SWIZZLE_IDENTITY &&
           c.b == VK_COMPONENT_SWIZZLE_IDENTITY && c.a == VK_COMPONENT_SWIZZLE_IDENTITY;
}

static void viewOf(const char *what, const VkImageViewCreateInfo *v, VkImage image, VkFormat format) {
    CHECK(v->image == image && v->viewType == VK_IMAGE_VIEW_TYPE_2D && v->format == format &&
          v->subresourceRange.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && v->subresourceRange.baseMipLevel == 0 &&
          v->subresourceRange.levelCount == 1 && v->subresourceRange.baseArrayLayer == 0 && v->subresourceRange.layerCount == 1,
          "%s: view not of the whole image in its format", what);
}

int main(void) {
    struct lorie_vk_image *img;
    AHardwareBuffer b, b2, b3;

    /* RGBX, as the X server allocates them */
    device();
    b = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, X_USAGE);
    img = lorieVkImport(&vk, &images, 7, &b, 0, &st);
    CHECK(img && img->id == 7 && img->image && img->memory && img->sampleView && img->attachView, "RGBX: not imported");
    if (img) {
        CHECK(img->format == VK_FORMAT_R8G8B8A8_UNORM && img->width == 640 && img->height == 480 && img->rgbaOrder && img->rgbx &&
              img->usage == (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT),
              "RGBX: format %d, %ux%u, rgbaOrder %d, rgbx %d, usage %#x", img->format, img->width, img->height,
              img->rgbaOrder, img->rgbx, img->usage);
        CHECK(imp.formatQueries == 1 && imp.query.format == VK_FORMAT_R8G8B8A8_UNORM && imp.query.type == VK_IMAGE_TYPE_2D &&
              imp.query.tiling == VK_IMAGE_TILING_OPTIMAL && imp.query.usage == img->usage && imp.query.flags == 0 &&
              imp.queryHandleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
              "RGBX: format query not about the image made (format %d usage %#x handle %#x)", imp.query.format,
              imp.query.usage, imp.queryHandleType);
        CHECK(imp.imageHandleTypes == VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID &&
              imp.image.imageType == VK_IMAGE_TYPE_2D && imp.image.format == VK_FORMAT_R8G8B8A8_UNORM &&
              imp.image.extent.width == 640 && imp.image.extent.height == 480 && imp.image.extent.depth == 1 &&
              imp.image.mipLevels == 1 && imp.image.arrayLayers == 1 && imp.image.samples == VK_SAMPLE_COUNT_1_BIT &&
              imp.image.tiling == VK_IMAGE_TILING_OPTIMAL && imp.image.usage == img->usage &&
              imp.image.sharingMode == VK_SHARING_MODE_EXCLUSIVE && imp.image.initialLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
              imp.image.flags == 0, "RGBX: image not created as an AHB import");
        CHECK(imp.importedBuffer == &b && imp.dedicatedImage == imp.createdImage && imp.allocationSize == b.allocationSize &&
              imp.memoryType == 1 && imp.boundImage == imp.createdImage && img->image == imp.createdImage,
              "RGBX: memory not a dedicated import of the buffer (buffer %p, dedicated %p, size %llu, type %u)",
              (const void *) imp.importedBuffer, (void *) imp.dedicatedImage, (unsigned long long) imp.allocationSize, imp.memoryType);
        CHECK(imp.viewCount == 2, "RGBX: %d views", imp.viewCount);
        viewOf("RGBX sampling view", &imp.viewInfo[0], img->image, img->format);
        viewOf("RGBX attachment view", &imp.viewInfo[1], img->image, img->format);
        CHECK(imp.viewInfo[0].components.r == VK_COMPONENT_SWIZZLE_IDENTITY && imp.viewInfo[0].components.g == VK_COMPONENT_SWIZZLE_IDENTITY &&
              imp.viewInfo[0].components.b == VK_COMPONENT_SWIZZLE_IDENTITY && imp.viewInfo[0].components.a == VK_COMPONENT_SWIZZLE_ONE,
              "RGBX: sampling view does not read alpha as one");
        CHECK(identity(imp.viewInfo[1].components), "RGBX: attachment view not identity");
        CHECK(st.diag.vkAhbImportAttempts == 1 && st.diag.vkAhbImportSuccess == 1 && st.diag.fallbackTotal == 0,
              "RGBX: attempts %u success %u fallback %u", st.diag.vkAhbImportAttempts, st.diag.vkAhbImportSuccess, st.diag.fallbackTotal);
        CHECK(lorieVkImport(&vk, &images, 7, &b, 0, &st) == img && imp.properties == 1 && st.diag.vkAhbImportAttempts == 1,
              "RGBX: imported again");
    }

    /* RGBA: the same format, alpha sampled as stored */
    device();
    b = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, X_USAGE);
    img = lorieVkImport(&vk, &images, 8, &b, 0, &st);
    CHECK(img && img->rgbaOrder && !img->rgbx && identity(imp.viewInfo[0].components) && identity(imp.viewInfo[1].components),
          "RGBA: not imported with identity views");

    /* BGRA: Vulkan's B8G8R8A8, not RGBA order */
    device();
    b = buffer(AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, X_USAGE);
    img = lorieVkImport(&vk, &images, 9, &b, 0, &st);
    CHECK(img && img->format == VK_FORMAT_B8G8R8A8_UNORM && !img->rgbaOrder && !img->rgbx && imp.query.format == VK_FORMAT_B8G8R8A8_UNORM &&
          identity(imp.viewInfo[0].components), "BGRA: not imported as B8G8R8A8");

    /* only sampleable: no attachment usage, no attachment view */
    device();
    b = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
               AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN);
    img = lorieVkImport(&vk, &images, 10, &b, 0, &st);
    CHECK(img && img->usage == VK_IMAGE_USAGE_SAMPLED_BIT && !img->attachView && imp.viewCount == 1 && imp.views == 1,
          "sampled-only buffer: usage %#x, %d views", img ? img->usage : 0, imp.viewCount);

    /* extra usage asked for is asked about and given */
    device();
    b = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, X_USAGE);
    img = lorieVkImport(&vk, &images, 11, &b, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &st);
    CHECK(img && img->usage == (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
          VK_IMAGE_USAGE_TRANSFER_DST_BIT) && imp.query.usage == img->usage && imp.image.usage == img->usage,
          "extra usage not asked about and given");

    /* the memory type: whichever the buffer allows, device-local first */
    device();
    b = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, X_USAGE);
    b.memoryTypeBits = 0x1;
    CHECK(lorieVkImport(&vk, &images, 12, &b, 0, &st) && imp.memoryType == 0, "only type 0 allowed: type %u used", imp.memoryType);
    b2 = b;
    b2.memoryTypeBits = 0x5;
    CHECK(lorieVkImport(&vk, &images, 13, &b2, 0, &st) && imp.memoryType == 2, "types 0 and 2 allowed: type %u used, not the device-local one",
          imp.memoryType);

    /* unsupported formats */
    device();
    b = buffer(AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM, VK_FORMAT_R5G6B5_UNORM_PACK16, X_USAGE);
    refused("R5G6B5", lorieVkImport(&vk, &images, 20, &b, 0, &st), &b, 20, LORIE_FALLBACK_CLASS_RESOURCE_LOCAL,
            LORIE_FALLBACK_AHB_FORMAT_UNSUPPORTED_BY_STAGE1);
    device();
    b = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, X_USAGE);
    refused("RGBA reported as sRGB", lorieVkImport(&vk, &images, 21, &b, 0, &st), &b, 21, LORIE_FALLBACK_CLASS_RESOURCE_LOCAL,
            LORIE_FALLBACK_AHB_FORMAT_UNSUPPORTED_BY_STAGE1);

    /* external format */
    device();
    b = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED, X_USAGE);
    refused("external format", lorieVkImport(&vk, &images, 22, &b, 0, &st), &b, 22, LORIE_FALLBACK_CLASS_RESOURCE_LOCAL,
            LORIE_FALLBACK_AHB_EXTERNAL_FORMAT_UNSUPPORTED_BY_STAGE1);
    CHECK(imp.formatQueries == 0, "external format: asked about as an image anyway");

    /* not usable as an image the renderer samples */
    device();
    b = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN);
    refused("not GPU-sampleable", lorieVkImport(&vk, &images, 23, &b, 0, &st), &b, 23, LORIE_FALLBACK_CLASS_RESOURCE_LOCAL,
            LORIE_FALLBACK_AHB_IMAGE_USAGE_UNSUPPORTED);
    device();
    b = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, X_USAGE);
    b.desc.layers = 2;
    refused("two layers", lorieVkImport(&vk, &images, 24, &b, 0, &st), &b, 24, LORIE_FALLBACK_CLASS_RESOURCE_LOCAL,
            LORIE_FALLBACK_AHB_IMAGE_USAGE_UNSUPPORTED);

    /* memoryTypeBits */
    device();
    b = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, X_USAGE);
    b.memoryTypeBits = 0;
    refused("no memory type allowed", lorieVkImport(&vk, &images, 25, &b, 0, &st), &b, 25, LORIE_FALLBACK_CLASS_RESOURCE_LOCAL,
            LORIE_FALLBACK_AHB_MEMORY_TYPE);
    device();
    b.memoryTypeBits = 0x8;     /* a type the device does not have */
    refused("only a missing memory type allowed", lorieVkImport(&vk, &images, 26, &b, 0, &st), &b, 26,
            LORIE_FALLBACK_CLASS_RESOURCE_LOCAL, LORIE_FALLBACK_AHB_MEMORY_TYPE);

    /* the image-format query */
    device();
    b = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, X_USAGE);
    cfg.formatQuery = VK_ERROR_FORMAT_NOT_SUPPORTED;
    refused("format query refuses", lorieVkImport(&vk, &images, 27, &b, 0, &st), &b, 27, LORIE_FALLBACK_CLASS_RESOURCE_LOCAL,
            LORIE_FALLBACK_AHB_IMAGE_USAGE_UNSUPPORTED);
    CHECK(st.diag.lastVkResult == VK_ERROR_FORMAT_NOT_SUPPORTED, "format query refuses: VkResult %d", st.diag.lastVkResult);
    device();
    cfg.externalFeatures = VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT;
    refused("not importable", lorieVkImport(&vk, &images, 28, &b, 0, &st), &b, 28, LORIE_FALLBACK_CLASS_RESOURCE_LOCAL,
            LORIE_FALLBACK_AHB_IMAGE_USAGE_UNSUPPORTED);
    device();
    cfg.maxExtent = (VkExtent3D) { 512, 512, 1 };
    refused("larger than the format allows", lorieVkImport(&vk, &images, 29, &b, 0, &st), &b, 29,
            LORIE_FALLBACK_CLASS_RESOURCE_LOCAL, LORIE_FALLBACK_AHB_IMAGE_USAGE_UNSUPPORTED);
    CHECK(imp.properties == 1 && imp.formatQueries == 1 && imp.images == 0, "larger than allowed: image created anyway");

    /* the buffer's properties, and every call that makes the image */
    device();
    b = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, X_USAGE);
    b.propertiesResult = VK_ERROR_INVALID_EXTERNAL_HANDLE;
    refused("properties refused", lorieVkImport(&vk, &images, 30, &b, 0, &st), &b, 30, LORIE_FALLBACK_CLASS_RESOURCE_LOCAL,
            LORIE_FALLBACK_AHB_PROPERTIES);
    CHECK(st.diag.lastVkResult == VK_ERROR_INVALID_EXTERNAL_HANDLE, "properties refused: VkResult %d", st.diag.lastVkResult);
    b.propertiesResult = VK_SUCCESS;
    device(); cfg.createImage = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    refused("vkCreateImage fails", lorieVkImport(&vk, &images, 31, &b, 0, &st), &b, 31, LORIE_FALLBACK_CLASS_RESOURCE_LOCAL,
            LORIE_FALLBACK_AHB_IMPORT);
    device(); cfg.allocateMemory = VK_ERROR_INVALID_EXTERNAL_HANDLE;
    refused("vkAllocateMemory fails", lorieVkImport(&vk, &images, 32, &b, 0, &st), &b, 32, LORIE_FALLBACK_CLASS_RESOURCE_LOCAL,
            LORIE_FALLBACK_AHB_IMPORT);
    device(); cfg.bindMemory = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    refused("vkBindImageMemory fails", lorieVkImport(&vk, &images, 33, &b, 0, &st), &b, 33, LORIE_FALLBACK_CLASS_RESOURCE_LOCAL,
            LORIE_FALLBACK_AHB_IMPORT);
    device(); cfg.failViewNumber = 1;
    refused("sampling view fails", lorieVkImport(&vk, &images, 34, &b, 0, &st), &b, 34, LORIE_FALLBACK_CLASS_RESOURCE_LOCAL,
            LORIE_FALLBACK_AHB_IMPORT);
    device(); cfg.failViewNumber = 2;
    refused("attachment view fails", lorieVkImport(&vk, &images, 35, &b, 0, &st), &b, 35, LORIE_FALLBACK_CLASS_RESOURCE_LOCAL,
            LORIE_FALLBACK_AHB_IMPORT);
    device();
    refused("no buffer", lorieVkImport(&vk, &images, 36, NULL, 0, &st), &(AHardwareBuffer) {0}, 36,
            LORIE_FALLBACK_CLASS_RESOURCE_LOCAL, LORIE_FALLBACK_AHB_PROPERTIES);

    /* a format the ownership self-test did not verify (BGRA unavailable to it): refused, the others not */
    device();
    vk.verifiedFormats = 3;
    b = buffer(AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, X_USAGE);
    refused("BGRA not verified", lorieVkImport(&vk, &images, 50, &b, 0, &st), &b, 50, LORIE_FALLBACK_CLASS_RESOURCE_LOCAL,
            LORIE_FALLBACK_AHB_FORMAT_UNSUPPORTED_BY_STAGE1);
    b2 = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, X_USAGE);
    CHECK(lorieVkImport(&vk, &images, 51, &b2, 0, &st), "RGBX refused with only BGRA unverified");
    device();
    vk.verifiedFormats = 0;
    b = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, X_USAGE);
    refused("nothing verified", lorieVkImport(&vk, &images, 52, &b, 0, &st), &b, 52, LORIE_FALLBACK_CLASS_RESOURCE_LOCAL,
            LORIE_FALLBACK_AHB_FORMAT_UNSUPPORTED_BY_STAGE1);

    /* injected: no call made */
    device();
    st.backend.vkFailAt = LORIE_VK_FAIL_AT_IMPORT;
    refused("injected at import", lorieVkImport(&vk, &images, 37, &b, 0, &st), &b, 37, LORIE_FALLBACK_CLASS_INJECTED,
            LORIE_FALLBACK_AHB_IMPORT);
    CHECK(imp.properties == 0 && imp.formatQueries == 0 && st.diag.lastVkResult == VK_SUCCESS && st.diag.vkAhbImportAttempts == 1,
          "injected at import: calls made (properties %d, queries %d), VkResult %d", imp.properties, imp.formatQueries,
          st.diag.lastVkResult);
    st.backend.vkFailAt = LORIE_VK_FAIL_AT_SUBMIT;
    CHECK(lorieVkImport(&vk, &images, 38, &b, 0, &st) && st.diag.fallbackTotal == 1, "injection at another point stopped an import");

    /* resource-local: the next buffer imports */
    device();
    b = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED, X_USAGE);
    b2 = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, X_USAGE);
    CHECK(!lorieVkImport(&vk, &images, 40, &b, 0, &st) && lorieVkImport(&vk, &images, 41, &b2, 0, &st) &&
          images.count == 1 && st.diag.vkAhbImportAttempts == 2 && st.diag.vkAhbImportSuccess == 1 && st.diag.fallbackTotal == 1,
          "a refused buffer stopped the next one");

    /* removal: only that image, all of it */
    device();
    b = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, X_USAGE);
    b2 = b;
    b3 = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE);
    CHECK(lorieVkImport(&vk, &images, 1, &b, 0, &st) && lorieVkImport(&vk, &images, 2, &b2, 0, &st) &&
          lorieVkImport(&vk, &images, 3, &b3, 0, &st) && imp.images == 3 && imp.memories == 3 && imp.views == 5,
          "three imports: %d images %d memories %d views", imp.images, imp.memories, imp.views);
    lorieVkImageRemove(&vk, &images, 2);
    CHECK(imp.images == 2 && imp.memories == 2 && imp.views == 3 && !lorieVkImageFind(&images, 2) &&
          lorieVkImageFind(&images, 1) && lorieVkImageFind(&images, 3) && lorieVkImageFind(&images, 3)->usage == VK_IMAGE_USAGE_SAMPLED_BIT,
          "removing one: %d images %d memories %d views left", imp.images, imp.memories, imp.views);
    lorieVkImageRemove(&vk, &images, 99);
    CHECK(images.count == 2, "removing an unknown id changed the cache");
    lorieVkImagesClear(&vk, &images);
    CHECK(imp.images == 0 && imp.memories == 0 && imp.views == 0 && images.count == 0 && !images.items,
          "clear: %d images %d memories %d views left", imp.images, imp.memories, imp.views);

    lorieVkDestroyDevice(&vk);
    printf("TVKIMPORT AHardwareBuffer import against a fake Vulkan: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
