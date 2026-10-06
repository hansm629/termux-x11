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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_ANDROID_KHR 1
#include <vulkan/vulkan.h>

#include "diagtypes_src.inc"
struct lorie_shared_server_state { struct lorie_renderer_diag diag; struct lorie_backend_control backend; };
#define log(...) ((void) 0)
#define loge(...) ((void) 0)
#include "vkinit_src.inc"

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
    memset(p, 0, sizeof *p);
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
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fGetDeviceProcAddr(VkDevice d, const char *name) {
    (void) d;
#define F(n) if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction) f ## n;
    F(DestroyDevice) F(GetDeviceQueue) F(DeviceWaitIdle)
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
    if (!cfg.noGetDeviceProcAddr)
        F(GetDeviceProcAddr)
#undef F
    return NULL;
}

#define SWAPCHAIN VK_KHR_SWAPCHAIN_EXTENSION_NAME
#define AHB VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME
#define FOREIGN VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME

/* A phone: Vulkan 1.1 loader and device, a compute-only family before the graphics one, all extensions. */
static void good(void) {
    memset(&cfg, 0, sizeof cfg);
    cfg.instanceVersion = VK_API_VERSION_1_1;
    cfg.instanceExt[0] = VK_KHR_SURFACE_EXTENSION_NAME;
    cfg.instanceExt[1] = VK_KHR_ANDROID_SURFACE_EXTENSION_NAME;
    cfg.ninstanceExt = 2;
    cfg.dev[0] = (struct fake_device) { VK_API_VERSION_1_1, { VK_QUEUE_COMPUTE_BIT, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT }, 2,
                                        { SWAPCHAIN, AHB, FOREIGN, "VK_KHR_other" }, 4, "Adreno (TM) 840" };
    cfg.ndev = 1;
}

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
    CHECK(!strcmp(calls.deviceExt, SWAPCHAIN " " AHB " " FOREIGN), "device extensions: %s", calls.deviceExt);
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
    cfg.dev[1] = (struct fake_device) { VK_API_VERSION_1_1, { VK_QUEUE_GRAPHICS_BIT }, 1, { FOREIGN, AHB, SWAPCHAIN }, 3, "second" };
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
