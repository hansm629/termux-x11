/* TVKINIT: the Vulkan backend's instance and device, from the real code (vk.h, vk.c: gen.py "vkinit"), over a
 * fake Vulkan reached only through the vkGetInstanceProcAddr it is given - as libvulkan.so's would be.
 *
 *   A usable device: instance asked for 1.1 with VK_KHR_surface and VK_KHR_android_surface, the device with
 *   exactly VK_KHR_swapchain, VK_ANDROID_external_memory_android_hardware_buffer and
 *   VK_EXT_queue_family_foreign, on a graphics queue family; counters move after the calls succeed; the device
 *   is named in the diagnostics; destroying it waits for idle, then destroys device and instance.
 *   Anything missing - 1.1 loader or device, the surface extensions, a device, a graphics queue, any of the
 *   three device extensions (FOREIGN included: no substitute) - or a failed call: no device, nothing left
 *   behind, exactly one fallback recorded with its class and reason; a second device is used when the first
 *   lacks something. Injection at instance or device: the call is not made, the fallback is INJECTED. */
#include "fakevk.h"

static struct lorie_shared_server_state st;
static struct lorie_vk vk;

static bool attempt(uint8_t failAt) {
    memset(&st, 0, sizeof st);
    memset(&calls, 0, sizeof calls);
    st.backend.vkFailAt = failAt;
    return lorieVkCreateDevice(&vk, fGetInstanceProcAddr, &st);
}

/* Failed: nothing created that is still there, exactly one fallback with this class and reason. */
static void failed(const char *what, bool ok, uint8_t cls, uint8_t reason) {
    CHECK(!ok, "%s: device created", what);
    CHECK(!vk.instance && !vk.device, "%s: handles left behind", what);
    CHECK(calls.destroyInstance == (st.diag.vkInstanceCreated ? 1 : 0), "%s: %d instances created, %d destroyed", what,
          st.diag.vkInstanceCreated, calls.destroyInstance);
    CHECK(calls.destroyDevice == (st.diag.vkDeviceCreated ? 1 : 0), "%s: device not destroyed", what);
    CHECK(st.diag.fallbackTotal == 1 && st.diag.fallbackByClass[cls] == 1 && st.diag.fallbackByReason[reason] == 1 &&
          st.diag.lastFallbackClass == cls && st.diag.lastFallbackReason == reason,
          "%s: fallback recorded as total %u, class %u, reason %u (want class %u reason %u)", what, st.diag.fallbackTotal,
          st.diag.lastFallbackClass, st.diag.lastFallbackReason, cls, reason);
}

int main(void) {
    bool ok;

    good();
    ok = attempt(LORIE_VK_FAIL_AT_NONE);
    CHECK(ok && vk.instance && vk.device && vk.queue == (VkQueue) &queueObj, "usable device: not created");
    CHECK(calls.apiVersion == VK_API_VERSION_1_1, "instance asked for API %#x", calls.apiVersion);
    CHECK(!strcmp(calls.instanceExt, VK_KHR_SURFACE_EXTENSION_NAME " " VK_KHR_ANDROID_SURFACE_EXTENSION_NAME),
          "instance extensions: %s", calls.instanceExt);
    CHECK(!strcmp(calls.deviceExt, SWAPCHAIN " " EXT_AHB " " FOREIGN), "device extensions: %s", calls.deviceExt);
    CHECK(calls.family == 1 && vk.queueFamily == 1, "queue family %u, not the graphics one", calls.family);
    CHECK(st.diag.vkInstanceCreated == 1 && st.diag.vkDeviceCreated == 1 && st.diag.fallbackTotal == 0,
          "usable device: counters instance %u device %u fallback %u", st.diag.vkInstanceCreated, st.diag.vkDeviceCreated,
          st.diag.fallbackTotal);
    CHECK(!strcmp((const char *) st.diag.vkDevice, "Adreno (TM) 840") && st.diag.vkApiVersion == VK_API_VERSION_1_1 &&
          st.diag.vkDriverVersion == 0x8040000u, "device not named in the diagnostics");
    lorieVkDestroyDevice(&vk);
    CHECK(calls.waitIdle == 1 && calls.destroyDevice == 1 && calls.destroyInstance == 1 && !vk.instance && !vk.device,
          "destroy: wait %d, device %d, instance %d", calls.waitIdle, calls.destroyDevice, calls.destroyInstance);

    good(); cfg.noEnumerateInstanceVersion = true;
    failed("1.0 loader", attempt(LORIE_VK_FAIL_AT_NONE), LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_VERSION);
    CHECK(!calls.createInstance, "1.0 loader: instance created");
    good(); cfg.instanceVersion = VK_API_VERSION_1_0;
    failed("1.0 instance", attempt(LORIE_VK_FAIL_AT_NONE), LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_VERSION);
    good(); cfg.ninstanceExt = 1;
    failed("no VK_KHR_android_surface", attempt(LORIE_VK_FAIL_AT_NONE), LORIE_FALLBACK_CLASS_FATAL_BACKEND,
           LORIE_FALLBACK_VK_EXT_ANDROID_SURFACE);
    CHECK(!calls.createInstance, "no android surface: instance created");
    good(); cfg.createInstance = VK_ERROR_INITIALIZATION_FAILED;
    failed("vkCreateInstance fails", attempt(LORIE_VK_FAIL_AT_NONE), LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_INSTANCE);
    CHECK(st.diag.lastVkResult == VK_ERROR_INITIALIZATION_FAILED && st.diag.vkInstanceCreated == 0,
          "vkCreateInstance fails: VkResult %d, instance counted %u", st.diag.lastVkResult, st.diag.vkInstanceCreated);
    good(); cfg.noGetDeviceProcAddr = true;
    failed("loader without vkGetDeviceProcAddr", attempt(LORIE_VK_FAIL_AT_NONE), LORIE_FALLBACK_CLASS_FATAL_BACKEND,
           LORIE_FALLBACK_VK_LOADER);
    good(); cfg.ndev = 0;
    failed("no device", attempt(LORIE_VK_FAIL_AT_NONE), LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_NO_DEVICE);
    good(); cfg.dev[0].api = VK_API_VERSION_1_0;
    failed("1.0 device", attempt(LORIE_VK_FAIL_AT_NONE), LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_VERSION);
    good(); cfg.dev[0].families[1] = VK_QUEUE_TRANSFER_BIT;
    failed("no graphics queue", attempt(LORIE_VK_FAIL_AT_NONE), LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_NO_DEVICE);
    good(); cfg.dev[0].ext[0] = "VK_KHR_other";
    failed("no VK_KHR_swapchain", attempt(LORIE_VK_FAIL_AT_NONE), LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_EXT_SWAPCHAIN);
    good(); cfg.dev[0].ext[1] = "VK_KHR_other";
    failed("no AHB import", attempt(LORIE_VK_FAIL_AT_NONE), LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_EXT_AHB);
    good(); cfg.dev[0].ext[2] = "VK_KHR_external_memory";
    failed("no VK_EXT_queue_family_foreign", attempt(LORIE_VK_FAIL_AT_NONE), LORIE_FALLBACK_CLASS_FATAL_BACKEND,
           LORIE_FALLBACK_VK_EXT_FOREIGN);
    CHECK(!calls.createDevice, "no FOREIGN: device created anyway");

    good(); cfg.dev[0].ext[2] = "VK_KHR_other"; cfg.dev[0].name = "first";
    cfg.dev[1] = (struct fake_device) { VK_API_VERSION_1_1, { VK_QUEUE_GRAPHICS_BIT }, 1, { FOREIGN, EXT_AHB, SWAPCHAIN }, 3, "second" };
    cfg.ndev = 2;
    ok = attempt(LORIE_VK_FAIL_AT_NONE);
    CHECK(ok && calls.deviceUsed == (VkPhysicalDevice) &cfg.dev[1] && vk.queueFamily == 0 &&
          !strcmp((const char *) st.diag.vkDevice, "second") && st.diag.fallbackTotal == 0,
          "second device not used when the first lacks FOREIGN");
    lorieVkDestroyDevice(&vk);
    cfg.dev[1].api = VK_API_VERSION_1_0;
    failed("two unusable devices", attempt(LORIE_VK_FAIL_AT_NONE), LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_EXT_FOREIGN);

    good(); cfg.createDevice = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    failed("vkCreateDevice fails", attempt(LORIE_VK_FAIL_AT_NONE), LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_DEVICE);
    CHECK(st.diag.lastVkResult == VK_ERROR_OUT_OF_DEVICE_MEMORY && st.diag.vkInstanceCreated == 1 && st.diag.vkDeviceCreated == 0,
          "vkCreateDevice fails: VkResult %d, instance %u device %u counted", st.diag.lastVkResult,
          st.diag.vkInstanceCreated, st.diag.vkDeviceCreated);

    good();
    failed("injected at instance", attempt(LORIE_VK_FAIL_AT_INSTANCE), LORIE_FALLBACK_CLASS_INJECTED, LORIE_FALLBACK_VK_INSTANCE);
    CHECK(!calls.createInstance && st.diag.lastVkResult == VK_SUCCESS, "injected at instance: the call was made");
    failed("injected at device", attempt(LORIE_VK_FAIL_AT_DEVICE), LORIE_FALLBACK_CLASS_INJECTED, LORIE_FALLBACK_VK_DEVICE);
    CHECK(calls.createInstance == 1 && !calls.createDevice, "injected at device: vkCreateDevice was called");
    ok = attempt(LORIE_VK_FAIL_AT_SWAPCHAIN);
    CHECK(ok && st.diag.fallbackTotal == 0, "injection at a later point stopped the device");
    lorieVkDestroyDevice(&vk);

    printf("TVKINIT Vulkan instance and device against a fake loader: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
