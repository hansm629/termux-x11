/* TVKBARRIER: the barriers that take the X server's AHardwareBuffers over from, and give them back to, the
 * foreign owner (vk.h, vk.c: gen.py "vk") - the Stage 1 policy FOREIGN/GENERAL, field by field.
 *
 *   Acquire, for every use: VK_QUEUE_FAMILY_FOREIGN_EXT to the renderer's family, GENERAL to the use's layout,
 *   source access 0 and TOP_OF_PIPE (ignored for an acquire, should be 0), the use's stage and accesses as the
 *   destination. Release: the use's layout to GENERAL, the renderer's family to FOREIGN, the use's writes made
 *   available, destination access 0 and BOTTOM_OF_PIPE. Within the queue: the use's layouts, the earlier use's
 *   writes made available to the later use's accesses, no ownership change. No barrier ever has
 *   VK_IMAGE_LAYOUT_UNDEFINED; any acquire, uses, release chain their layouts from GENERAL back to GENERAL. */
#include "fakevk.h"

static const char *const names[] = { "sample", "render", "transfer-src", "transfer-dst" };
static const VkImageLayout layouts[] = { VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL };
static const VkPipelineStageFlags stages[] = { VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT };
static const VkAccessFlags accesses[] = { VK_ACCESS_SHADER_READ_BIT,
                                          VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                          VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT };
static const VkAccessFlags writes[] = { 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT };

/* The accesses each stage can perform (the specification's table of supported access types, for these stages). */
static bool supported(VkPipelineStageFlags stage, VkAccessFlags access) {
    VkAccessFlags ok = 0;
    if (stage & VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)
        ok |= VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    if (stage & VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT)
        ok |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    if (stage & VK_PIPELINE_STAGE_TRANSFER_BIT)
        ok |= VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    return (access & ~ok) == 0;
}

static void common(const char *what, const struct lorie_vk_barrier *b, VkImage image) {
    const VkImageMemoryBarrier *m = &b->barrier;
    CHECK(m->sType == VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER && !m->pNext && m->image == image, "%s: not a barrier on the image", what);
    CHECK(m->subresourceRange.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && m->subresourceRange.baseMipLevel == 0 &&
          m->subresourceRange.levelCount == 1 && m->subresourceRange.baseArrayLayer == 0 && m->subresourceRange.layerCount == 1,
          "%s: not the whole image", what);
    CHECK(m->oldLayout != VK_IMAGE_LAYOUT_UNDEFINED && m->newLayout != VK_IMAGE_LAYOUT_UNDEFINED, "%s: VK_IMAGE_LAYOUT_UNDEFINED", what);
    CHECK(b->srcStage && b->dstStage, "%s: an empty stage mask", what);
    CHECK(supported(b->srcStage, m->srcAccessMask) && supported(b->dstStage, m->dstAccessMask),
          "%s: access %#x/%#x not supported by stages %#x/%#x", what, m->srcAccessMask, m->dstAccessMask, b->srcStage, b->dstStage);
}

int main(void) {
    struct lorie_vk vk = { .queueFamily = 5 };
    VkImage image = (VkImage) (uintptr_t) 0x4242;
    char what[96];

    for (int u = LORIE_VK_USE_SAMPLE; u <= LORIE_VK_USE_TRANSFER_DST; u++) {
        struct lorie_vk_barrier a = lorieVkAcquireBarrier(&vk, image, u), r = lorieVkReleaseBarrier(&vk, image, u);

        snprintf(what, sizeof what, "acquire for %s", names[u]);
        common(what, &a, image);
        CHECK(a.barrier.srcQueueFamilyIndex == VK_QUEUE_FAMILY_FOREIGN_EXT && a.barrier.dstQueueFamilyIndex == 5,
              "%s: families %u -> %u", what, a.barrier.srcQueueFamilyIndex, a.barrier.dstQueueFamilyIndex);
        CHECK(a.barrier.oldLayout == VK_IMAGE_LAYOUT_GENERAL && a.barrier.newLayout == layouts[u],
              "%s: layouts %d -> %d", what, a.barrier.oldLayout, a.barrier.newLayout);
        CHECK(a.barrier.srcAccessMask == 0 && a.srcStage == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
              "%s: source access %#x stage %#x", what, a.barrier.srcAccessMask, a.srcStage);
        CHECK(a.barrier.dstAccessMask == accesses[u] && a.dstStage == stages[u],
              "%s: destination access %#x stage %#x", what, a.barrier.dstAccessMask, a.dstStage);

        snprintf(what, sizeof what, "release after %s", names[u]);
        common(what, &r, image);
        CHECK(r.barrier.srcQueueFamilyIndex == 5 && r.barrier.dstQueueFamilyIndex == VK_QUEUE_FAMILY_FOREIGN_EXT,
              "%s: families %u -> %u", what, r.barrier.srcQueueFamilyIndex, r.barrier.dstQueueFamilyIndex);
        CHECK(r.barrier.oldLayout == layouts[u] && r.barrier.newLayout == VK_IMAGE_LAYOUT_GENERAL,
              "%s: layouts %d -> %d", what, r.barrier.oldLayout, r.barrier.newLayout);
        CHECK(r.barrier.srcAccessMask == writes[u] && r.srcStage == stages[u],
              "%s: source access %#x stage %#x", what, r.barrier.srcAccessMask, r.srcStage);
        CHECK(r.barrier.dstAccessMask == 0 && r.dstStage == VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
              "%s: destination access %#x stage %#x", what, r.barrier.dstAccessMask, r.dstStage);

        for (int v = LORIE_VK_USE_SAMPLE; v <= LORIE_VK_USE_TRANSFER_DST; v++) {
            struct lorie_vk_barrier w = lorieVkUseBarrier(image, u, v), a2, r2;
            snprintf(what, sizeof what, "%s then %s", names[u], names[v]);
            common(what, &w, image);
            CHECK(w.barrier.srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED && w.barrier.dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED,
                  "%s: changes ownership", what);
            CHECK(w.barrier.oldLayout == layouts[u] && w.barrier.newLayout == layouts[v], "%s: layouts %d -> %d", what,
                  w.barrier.oldLayout, w.barrier.newLayout);
            CHECK(w.barrier.srcAccessMask == writes[u] && w.srcStage == stages[u] && w.barrier.dstAccessMask == accesses[v] &&
                  w.dstStage == stages[v], "%s: access %#x -> %#x, stages %#x -> %#x", what, w.barrier.srcAccessMask,
                  w.barrier.dstAccessMask, w.srcStage, w.dstStage);
            /* acquire for u, then v, release: GENERAL ... GENERAL with no gap */
            a2 = lorieVkAcquireBarrier(&vk, image, u);
            r2 = lorieVkReleaseBarrier(&vk, image, v);
            CHECK(a2.barrier.oldLayout == VK_IMAGE_LAYOUT_GENERAL && a2.barrier.newLayout == w.barrier.oldLayout &&
                  w.barrier.newLayout == r2.barrier.oldLayout && r2.barrier.newLayout == VK_IMAGE_LAYOUT_GENERAL,
                  "%s: layouts do not chain from GENERAL back to GENERAL", what);
        }
    }

    printf("TVKBARRIER foreign ownership barriers (FOREIGN/GENERAL policy): %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
