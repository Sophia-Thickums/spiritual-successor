/* spirit_present_counter.c — a tiny Vulkan layer that COUNTS vkQueuePresentKHR calls.
 *
 * WHY THIS EXISTS
 * ---------------
 * Frame generation inserts frames BELOW the application. Everything the app can see — its
 * own FPS counter, its benchmark score, a frame-time CSV it writes — counts only REAL frames
 * and is structurally blind to generated ones. An overlay (MangoHud) sits ABOVE the frame-gen
 * layer for the same reason, and the compositor's own scan-out counter reports the display
 * refresh, which never changes.
 *
 * This layer is placed in the chain BELOW the frame-gen layer, so it sees every image that
 * actually leaves for the compositor. That is the only place the question "did the presented
 * rate go up?" can be answered honestly.
 *
 * DESIGN
 * ------
 *   - INERT unless SPIRIT_PRESENT_COUNT names a file. Installed and forgotten is safe.
 *   - Counts ONE function. It does not alter any image, mode, or present behaviour.
 *   - Writes the running count to the file on every present (pwrite at offset 0, fixed
 *     width), so a reader can sample it at any instant and a crash loses at most one frame.
 *   - SPIRIT_PRESENT_COUNTER_DISABLE=1 kills it (the loader's own convention).
 *   - SPIRIT_PRESENT_COUNTER_VERBOSE=1 logs what it hooked, to stderr.
 *
 * Build:  cc -O2 -shared -fPIC -o libspirit_present_counter.so spirit_present_counter.c
 */

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>

#ifndef VK_LAYER_EXPORT
#define VK_LAYER_EXPORT __attribute__((visibility("default")))
#endif

#define MAX_TRACKED 64
#define COUNT_WIDTH 20

static PFN_vkGetInstanceProcAddr g_next_gipa = NULL;

typedef struct {
    VkDevice                  dev;
    PFN_vkGetDeviceProcAddr   gdpa;   /* the layer BELOW us */
    PFN_vkQueuePresentKHR     present;
} DevEnt;

static DevEnt g_dev[MAX_TRACKED];
static int    g_ndev = 0;

typedef struct { VkQueue q; VkDevice dev; } QEnt;
static QEnt g_q[MAX_TRACKED];
static int  g_nq = 0;

static int           g_fd = -1;
static unsigned long g_count = 0;
static int           g_verbose = 0;
static int           g_opened = 0;

static void vlog(const char *fmt, ...) {
    if (!g_verbose) return;
    va_list ap;
    va_start(ap, fmt);
    fputs("(spirit-counter) ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void count_open(void) {
    if (g_opened) return;
    g_opened = 1;
    const char *path = getenv("SPIRIT_PRESENT_COUNT");
    if (!path || !*path) return;
    g_fd = open(path, O_RDWR | O_CREAT, 0644);
    if (g_fd < 0) { fprintf(stderr, "(spirit-counter) cannot open %s\n", path); return; }
    g_verbose = getenv("SPIRIT_PRESENT_COUNTER_VERBOSE") != NULL;
    char init[COUNT_WIDTH];
    memset(init, '0', COUNT_WIDTH);
    if (pwrite(g_fd, init, COUNT_WIDTH, 0) != COUNT_WIDTH)
        fprintf(stderr, "(spirit-counter) initial write failed\n");
    vlog("counting presents to %s", path);
}

static void count_flush(void) {
    if (g_fd < 0) return;
    char buf[COUNT_WIDTH + 1];
    int n = snprintf(buf, sizeof buf, "%0*lu", COUNT_WIDTH, g_count);
    if (n > COUNT_WIDTH) { memset(buf, '9', COUNT_WIDTH); n = COUNT_WIDTH; }
    if (pwrite(g_fd, buf, (size_t)n, 0) != n) return;   /* best effort; never throw */
}

static DevEnt *ent_for(VkDevice d) {
    if (!d) return NULL;
    for (int i = 0; i < g_ndev; i++) if (g_dev[i].dev == d) return &g_dev[i];
    return NULL;   /* unknown device: fall through to the instance chain, never guess */
}

static VkDevice dev_for_queue(VkQueue q) {
    for (int i = 0; i < g_nq; i++) if (g_q[i].q == q) return g_q[i].dev;
    return g_ndev ? g_dev[g_ndev - 1].dev : VK_NULL_HANDLE;
}

static void remember_queue(VkQueue q, VkDevice d) {
    if (g_nq >= MAX_TRACKED) return;
    g_q[g_nq].q = q; g_q[g_nq].dev = d; g_nq++;
}

/* ---------------- the hook ---------------- */

static VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(VkQueue queue,
                                                      const VkPresentInfoKHR *pPresentInfo) {
    VkDevice d = dev_for_queue(queue);
    DevEnt *e = ent_for(d);
    if (!e || !e->present) return VK_SUCCESS;

    g_count++;
    count_flush();          /* every present: a reader can sample at any instant */
    return e->present(queue, pPresentInfo);
}

static VKAPI_ATTR void VKAPI_CALL GetDeviceQueue(VkDevice device, uint32_t family,
                                                 uint32_t index, VkQueue *pQueue) {
    DevEnt *e = ent_for(device);
    if (!e || !e->gdpa) return;
    PFN_vkGetDeviceQueue f = (PFN_vkGetDeviceQueue)e->gdpa(device, "vkGetDeviceQueue");
    if (!f) return;
    f(device, family, index, pQueue);
    if (pQueue && *pQueue) remember_queue(*pQueue, device);
}

static VKAPI_ATTR void VKAPI_CALL GetDeviceQueue2(VkDevice device,
                                                  const VkDeviceQueueInfo2 *pInfo,
                                                  VkQueue *pQueue) {
    DevEnt *e = ent_for(device);
    if (!e || !e->gdpa) return;
    PFN_vkGetDeviceQueue2 f = (PFN_vkGetDeviceQueue2)e->gdpa(device, "vkGetDeviceQueue2");
    if (!f) return;
    f(device, pInfo, pQueue);
    if (pQueue && *pQueue) remember_queue(*pQueue, device);
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice gpu,
                                                   const VkDeviceCreateInfo *pCreateInfo,
                                                   const VkAllocationCallbacks *pAllocator,
                                                   VkDevice *pDevice) {
    VkLayerDeviceCreateInfo *ci = (VkLayerDeviceCreateInfo *)pCreateInfo->pNext;
    while (ci && !(ci->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
                   ci->function == VK_LAYER_LINK_INFO))
        ci = (VkLayerDeviceCreateInfo *)ci->pNext;
    if (!ci || !ci->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;

    VkLayerDeviceLink *link         = ci->u.pLayerInfo;
    PFN_vkGetInstanceProcAddr next_gipa = link->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr   next_gdpa = link->pfnNextGetDeviceProcAddr;
    vlog("CreateDevice entered (next_gipa=%p next_gdpa=%p)", (void *)next_gipa, (void *)next_gdpa);
    PFN_vkCreateDevice next_create =
        (PFN_vkCreateDevice)(next_gipa ? next_gipa(NULL, "vkCreateDevice") : NULL);
    if (!next_create) return VK_ERROR_INITIALIZATION_FAILED;

    /* step past ourselves in the chain so the layer below sees a clean list */
    ci->u.pLayerInfo = link->pNext;

    VkResult r = next_create(gpu, pCreateInfo, pAllocator, pDevice);
    vlog("CreateDevice downstream returned %d", (int)r);
    if (r != VK_SUCCESS || !pDevice || !*pDevice) return r;

    if (!next_gdpa) { vlog("NO downstream GetDeviceProcAddr — counter inert"); return r; }
    if (g_ndev < MAX_TRACKED) {
        DevEnt *e = &g_dev[g_ndev++];
        e->dev = *pDevice;
        e->gdpa = next_gdpa;
        e->present = (PFN_vkQueuePresentKHR)next_gdpa(*pDevice, "vkQueuePresentKHR");
        vlog(e->present ? "hooked present on a device" : "NO downstream present found");
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                                                     const VkAllocationCallbacks *pAllocator,
                                                     VkInstance *pInstance) {
    count_open();
    VkLayerInstanceCreateInfo *ci = (VkLayerInstanceCreateInfo *)pCreateInfo->pNext;
    while (ci && !(ci->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                   ci->function == VK_LAYER_LINK_INFO))
        ci = (VkLayerInstanceCreateInfo *)ci->pNext;
    if (!ci || !ci->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;

    VkLayerInstanceLink *link = ci->u.pLayerInfo;
    PFN_vkGetInstanceProcAddr next = link->pfnNextGetInstanceProcAddr;
    PFN_vkCreateInstance next_create =
        (PFN_vkCreateInstance)(next ? next(NULL, "vkCreateInstance") : NULL);
    if (!next_create) return VK_ERROR_INITIALIZATION_FAILED;

    g_next_gipa = next;                 /* everything we do not hook goes downstream */
    ci->u.pLayerInfo = link->pNext;     /* step past ourselves */

    return next_create(pCreateInfo, pAllocator, pInstance);
}

/* ---------------- entry points ---------------- */

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device,
                                                                  const char *name) {
    if (!strcmp(name, "vkQueuePresentKHR"))  return (PFN_vkVoidFunction)QueuePresentKHR;
    if (!strcmp(name, "vkGetDeviceQueue"))   return (PFN_vkVoidFunction)GetDeviceQueue;
    if (!strcmp(name, "vkGetDeviceQueue2"))  return (PFN_vkVoidFunction)GetDeviceQueue2;
    DevEnt *e = ent_for(device);
    if (e && e->gdpa) return e->gdpa(device, name);
    if (g_next_gipa) return g_next_gipa(NULL, name);
    return NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance,
                                                                    const char *name) {
    if (!strcmp(name, "vkCreateInstance"))     return (PFN_vkVoidFunction)CreateInstance;
    if (!strcmp(name, "vkCreateDevice"))       return (PFN_vkVoidFunction)CreateDevice;
    if (!strcmp(name, "vkGetDeviceProcAddr"))  return (PFN_vkVoidFunction)GetDeviceProcAddr;
    if (!strcmp(name, "vkGetInstanceProcAddr"))return (PFN_vkVoidFunction)GetInstanceProcAddr;
    if (g_next_gipa) return g_next_gipa(instance, name);
    return NULL;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pvi) {
    if (!pvi || pvi->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT)
        return VK_ERROR_INITIALIZATION_FAILED;
    count_open();
    if (pvi->loaderLayerInterfaceVersion >= 2) {
        pvi->loaderLayerInterfaceVersion = 2;
        pvi->pfnGetInstanceProcAddr       = GetInstanceProcAddr;
        pvi->pfnGetDeviceProcAddr         = GetDeviceProcAddr;
        pvi->pfnGetPhysicalDeviceProcAddr = NULL;
    }
    return VK_SUCCESS;
}

/* legacy exports, for loaders that do not negotiate */
VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance i, const char *n) { return GetInstanceProcAddr(i, n); }
VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice d, const char *n) { return GetDeviceProcAddr(d, n); }
