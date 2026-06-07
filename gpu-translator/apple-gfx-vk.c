/*
 * apple-gfx-vk.c — PVG -> host Vulkan translator, SOFTWARE path (Mesa lavapipe).
 *
 * Backs the accelerated PVG stream (ExecIndirect2/3) on a GPU-less non-Apple ARM
 * host by executing the Metal-derived GPU work on the CPU via the LLVMpipe Vulkan
 * ICD (lavapipe). apple-gfx-native.c reaches this through the PGVkOps vtable in
 * apple-gfx-vk.h and never sees a Vulkan symbol.
 *
 * Build gating: the ENTIRE real body is under #ifdef CONFIG_PVG_VULKAN, set by
 * meson when dependency('vulkan', required:false) is found. When the macro is OFF
 * (no Vulkan loader — e.g. a macOS host, or a minimal build) the file still
 * compiles to a single pg_vk_ops() that returns NULL, so the native backend keeps
 * its immediate pg_signal_stamp() stub and links unchanged.
 *
 * Scope (the full path described in docs/metal-vulkan-translator.md):
 *   Host context bring-up:
 *     - VkInstance creation (headless; debug utils only under APPLE_GFX_VK_DEBUG)
 *     - software/llvmpipe VkPhysicalDevice selection
 *       (deviceType==CPU || deviceName ~ "llvmpipe")
 *     - VkDevice + single graphics/compute/transfer VkQueue
 *     - timeline VkSemaphore (the completion fence)
 *     - HOST_VISIBLE|HOST_COHERENT readback VkBuffer (persistently mapped)
 *     - completion thread (vkWaitSemaphores loop) + submit lock
 *     - pg_vk_ops() returning the impl; clean teardown
 *   Translator + present (the op-0x37 -> Vulkan path):
 *     - exec_indirect: resolves the inner Metal stream, walks the 9-opcode
 *       render subset, recognizes the textured-quad composite signature, and
 *       replays it through the embedded SPIR-V pipeline into a BGRA8 target.
 *       Opt-in via APPLE_GFX_VK_EXEC; returns 0 (native side keeps the immediate
 *       stamp) for any submit that is not a recognizable composite.
 *     - present / present_guest: the textured-quad present (synthetic or supplied
 *       BGRA8 source) and the guest-surface present, both via dynamic rendering +
 *       readback to present_bgra8.
 *     - the timeline -> bottom-half -> stamp marshalling (BQL-safe completion).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/* QEMU-only TU (compiled into the QEMU link, not the standalone replay harness),
 * so it MAY use the QEMU portability layer + thread primitives, unlike the
 * framework-free, header-dependency-free apple-gfx-native.c.
 * osdep.h MUST precede the #if: it pulls in config-host.h where meson defines
 * CONFIG_PVG_VULKAN (else the guard is never visible and the ON path is dead). */
#include "qemu/osdep.h"

#if CONFIG_PVG_VULKAN

#include "qemu/thread.h"
#include "qemu/main-loop.h"   /* qemu_bh_new / qemu_bh_schedule (BQL marshalling) */

#include <vulkan/vulkan.h>

#include "apple-gfx-vk.h"
#include "pvg_vk_shaders.h"   /* SimpleVertex + SimpleTextureFragment SPIR-V (quad) */

/* ------------------------------------------------------------------ logging */

/* Route through the device's structured log sink when present, else stderr.
 * pg_log() in apple-gfx-native.c is static; this TU has its own thin wrapper. */
static void vk_log(PGVkContext *c, const char *fmt, ...);

#define VK_LOGF(c, ...) vk_log((c), __VA_ARGS__)

/* ----------------------------------------------------------------------------
 * A pending GPU submission awaiting completion. The completion
 * thread blocks until the timeline reaches `value`, then presents the readback
 * and signals `stamp_value`. A fixed ring keeps the skeleton allocation-free.
 * --------------------------------------------------------------------------- */
typedef struct PGVkPending {
    uint64_t value;          /* timeline value this submission signals at      */
    uint32_t stamp_value;    /* hdr->stampValue to ack on completion           */
    uint32_t surface_id;     /* presented surface (0 = none)                   */
    VkCommandBuffer cb;      /* recycle to the pool after completion           */
} PGVkPending;

#define PG_VK_PENDING_RING   256

/* ----------------------------------------------------------------------------
 * The real context (opaque to apple-gfx-native.c). See docs/metal-vulkan-translator.md §1.1.
 * --------------------------------------------------------------------------- */
struct PGVkContext {
    /* --- core Vulkan, lavapipe-backed --- */
    VkInstance        inst;
    VkPhysicalDevice  phys;          /* the lavapipe (llvmpipe) / CPU device    */
    VkDevice          dev;
    uint32_t          qfam;          /* the one universal queue family          */
    VkQueue           queue;         /* graphics+compute+transfer, single queue */
    VkCommandPool     cmd_pool;      /* transient; guarded by submit_lock       */

    /* --- completion (docs/metal-vulkan-translator.md §3) --- */
    VkSemaphore       timeline;      /* VK_KHR_timeline_semaphore               */
    uint64_t          tl_next;       /* next value to signal (monotonic)        */
    QemuThread        completion_thr;
    QemuMutex         submit_lock;
    QemuCond          pending_cond;  /* completion thread waits on new submits  */
    bool              shutting_down;
    /* pending submission ring (single-producer FIFO drain thread,
     * single-consumer completion thread; guarded by submit_lock) */
    PGVkPending       pending[PG_VK_PENDING_RING];
    uint32_t          pend_head;     /* next to consume (completion thread)     */
    uint32_t          pend_tail;     /* next to fill (submit side)              */

    /* --- present readback (docs/metal-vulkan-translator.md §2) --- */
    VkBuffer          readback_buf;  /* HOST_VISIBLE|HOST_COHERENT staging      */
    VkDeviceMemory    readback_mem;
    void             *readback_map;  /* persistently mapped                     */
    size_t            readback_cap;

    /* --- clear-present: a cached BGRA8 color image cleared to a recognizable
     * pattern, copied to the readback buffer and handed to present_bgra8.
     * Created lazily on the first present(); reused after. --- */
    VkImage           a3_img;        /* 1920x1080 B8G8R8A8_UNORM target         */
    VkDeviceMemory    a3_mem;        /* device memory backing a3_img            */
    uint32_t          a3_w, a3_h;    /* geometry of the cached image            */
    VkImageView       a3_view;       /* color-attachment view of a3_img         */

    /* --- textured-quad present: the real present mechanism — a
     * graphics pipeline running the 2 embedded SPIR-V shaders (SimpleVertex +
     * SimpleTextureFragment) to draw a fullscreen textured quad sampling a
     * recognizable procedural source texture, rendered into a3_img via dynamic
     * rendering, then copied to readback and presented. All objects are created
     * lazily on the first textured present and cached for reuse. --- */
    bool              quad_mode;       /* APPLE_GFX_VK_QUAD set at init -> quad  */
    bool              quad_ready;      /* cached assets built (lazy, once)       */
    /* source test texture (sampled BGRA8, 256x256 procedural pattern) */
    VkImage           q_tex_img;
    VkDeviceMemory    q_tex_mem;
    VkImageView       q_tex_view;
    uint32_t          q_tex_w, q_tex_h;
    VkSampler         q_sampler;       /* linear / clamp-to-edge                 */
    /* fullscreen-quad vertex buffer (SimpleVertex: vec2 pos + vec2 uv)         */
    VkBuffer          q_vtx_buf;
    VkDeviceMemory    q_vtx_mem;
    uint32_t          q_vtx_count;     /* 4 (triangle strip)                     */
    /* shader modules + pipeline (descriptor set 0 binding 0 = combined sampler,
     * 64-byte VERTEX push constant = mvp) */
    VkShaderModule    q_vs;
    VkShaderModule    q_fs;
    VkDescriptorSetLayout q_dsl;
    VkDescriptorPool  q_dpool;
    VkDescriptorSet   q_dset;
    VkPipelineLayout  q_pl_layout;
    VkPipeline        q_pipeline;

    /* --- guest present: the GUEST's REAL composited surface, uploaded into a
     * SEPARATE, dynamically-sized sampled source image and drawn through the SAME
     * fullscreen-quad pipeline (q_pipeline / q_pl_layout / q_sampler / q_vtx_buf
     * / q_dset) so the FBSCAN'd guest pixels — not the procedural 256x256 texture —
     * become the sampled source. Created lazily on the first guest present and
     * recreated when the guest geometry (g_tex_w/g_tex_h) changes. A dedicated
     * HOST_VISIBLE staging buffer (g_stage_*) holds the per-frame upload; it is
     * grown on demand. --- */
    VkImage           g_tex_img;       /* sampled BGRA8 guest source image        */
    VkDeviceMemory    g_tex_mem;       /* device memory backing g_tex_img         */
    VkImageView       g_tex_view;      /* sampled view of g_tex_img               */
    uint32_t          g_tex_w, g_tex_h;/* geometry of the cached guest source     */
    VkBuffer          g_stage_buf;     /* HOST_VISIBLE staging for the upload     */
    VkDeviceMemory    g_stage_mem;     /* device memory backing g_stage_buf       */
    void             *g_stage_map;     /* persistently mapped staging             */
    size_t            g_stage_cap;     /* staging buffer capacity (bytes)         */

    /* --- back-reference to the existing seam for GPA + present + stamp --- */
    PGNativeDevice        *dev_back;   /* owning device (for pg_signal_stamp)   */
    const PGNativeHostOps *host;
    void                  *host_opaque;
    bool                   host_ptr_import_ok;  /* probed; usually FALSE on lvp */

    /* --- debug --- */
    bool              debug;
};

/* ----------------------------------------------------------------------------
 * vk_log: structured log via the host ops sink, else stderr.
 * --------------------------------------------------------------------------- */
static void vk_log(PGVkContext *c, const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (c && c->host && c->host->log) {
        c->host->log(c->host_opaque, line);
    } else {
        fprintf(stderr, "[apple-gfx-vk] %s\n", line);
    }
}

/* ------------------------------------------------------------- memory helper */

/* findMemoryType over the device's memory types (docs/metal-vulkan-translator.md §1.5).
 * On lavapipe the types are effectively all HOST_VISIBLE|HOST_COHERENT|
 * DEVICE_LOCAL (system RAM), so the first match almost always satisfies. */
static uint32_t pg_vk_mem_type(PGVkContext *c, uint32_t type_bits,
                               VkMemoryPropertyFlags need)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(c->phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & need) == need) {
            return i;
        }
    }
    return UINT32_MAX;
}

/* ----------------------------------------------------------------------------
 * INSTANCE — headless, software case. docs/metal-vulkan-translator.md §1.2.
 * No WSI/swapchain extensions (we present via readback -> present_bgra8). Debug
 * utils enabled only under APPLE_GFX_VK_DEBUG (huge slowdown on a CPU ICD).
 * --------------------------------------------------------------------------- */
static bool pg_vk_create_instance(PGVkContext *c)
{
    const char *want_exts[2];
    uint32_t want_n = 0;
    if (c->debug) {
        want_exts[want_n++] = "VK_EXT_debug_utils";
    }

    /* Filter the desired extensions against what the loader actually advertises,
     * so a missing optional extension never fails instance creation. */
    uint32_t avail_n = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &avail_n, NULL);
    VkExtensionProperties *avail = g_new0(VkExtensionProperties, avail_n ? avail_n : 1);
    vkEnumerateInstanceExtensionProperties(NULL, &avail_n, avail);

    const char *use_exts[2];
    uint32_t use_n = 0;
    for (uint32_t i = 0; i < want_n; i++) {
        for (uint32_t j = 0; j < avail_n; j++) {
            if (!strcmp(want_exts[i], avail[j].extensionName)) {
                use_exts[use_n++] = want_exts[i];
                break;
            }
        }
    }
    g_free(avail);

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "anyos-pvg-vk",
        .apiVersion = VK_API_VERSION_1_3,   /* request 1.3; tolerate 1.2 device */
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledExtensionCount = use_n,
        .ppEnabledExtensionNames = use_n ? use_exts : NULL,
    };
    VkResult r = vkCreateInstance(&ici, NULL, &c->inst);
    if (r != VK_SUCCESS) {
        VK_LOGF(c, "vkCreateInstance failed: %d", (int)r);
        return false;
    }
    return true;
}

/* ----------------------------------------------------------------------------
 * PHYSICAL DEVICE — force/confirm the software rasterizer. §1.3.
 * Prefer deviceType==CPU || deviceName ~ "llvmpipe". Last resort: device[0].
 * Fail loud (return false) when there is no device at all.
 * --------------------------------------------------------------------------- */
static bool pg_vk_pick_physical(PGVkContext *c)
{
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(c->inst, &n, NULL);
    if (n == 0) {
        VK_LOGF(c, "no VkPhysicalDevice (no Vulkan ICD / no lavapipe)");
        return false;
    }
    VkPhysicalDevice *all = g_new(VkPhysicalDevice, n);
    vkEnumeratePhysicalDevices(c->inst, &n, all);

    c->phys = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(all[i], &p);
        bool is_cpu = (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU);
        bool is_lvp = (strstr(p.deviceName, "llvmpipe") != NULL);
        if (is_cpu || is_lvp) {
            c->phys = all[i];
            VK_LOGF(c, "selected SW device: %s (type=%d, apiVersion=%u.%u.%u)",
                    p.deviceName, (int)p.deviceType,
                    VK_API_VERSION_MAJOR(p.apiVersion),
                    VK_API_VERSION_MINOR(p.apiVersion),
                    VK_API_VERSION_PATCH(p.apiVersion));
            break;
        }
    }
    if (c->phys == VK_NULL_HANDLE) {
        c->phys = all[0];   /* last resort: take the first device */
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(all[0], &p);
        VK_LOGF(c, "WARNING no llvmpipe/CPU device; falling back to %s",
                p.deviceName);
    }
    g_free(all);
    return c->phys != VK_NULL_HANDLE;
}

/* Find the single queue family that supports GRAPHICS (lavapipe exposes one
 * family with GRAPHICS|COMPUTE|TRANSFER). §1.4. */
static bool pg_vk_pick_queue_family(PGVkContext *c)
{
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(c->phys, &n, NULL);
    if (n == 0) {
        return false;
    }
    VkQueueFamilyProperties *qf = g_new(VkQueueFamilyProperties, n);
    vkGetPhysicalDeviceQueueFamilyProperties(c->phys, &n, qf);
    bool found = false;
    for (uint32_t i = 0; i < n; i++) {
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            c->qfam = i;
            found = true;
            break;
        }
    }
    g_free(qf);
    return found;
}

/* ----------------------------------------------------------------------------
 * LOGICAL DEVICE + single VkQueue + feature chain. §1.4.
 * Enable timeline_semaphore + dynamic_rendering + descriptor_indexing, filtering
 * device extensions against what is actually present (descriptor_buffer/
 * external_memory_host deliberately omitted — stage instead, §1.6).
 * --------------------------------------------------------------------------- */
static bool pg_vk_create_device(PGVkContext *c)
{
    /* Enumerate device extensions so we only request promoted-or-present ones. */
    uint32_t avail_n = 0;
    vkEnumerateDeviceExtensionProperties(c->phys, NULL, &avail_n, NULL);
    VkExtensionProperties *avail = g_new0(VkExtensionProperties, avail_n ? avail_n : 1);
    vkEnumerateDeviceExtensionProperties(c->phys, NULL, &avail_n, avail);

    static const char *want[] = {
        "VK_KHR_timeline_semaphore",
        "VK_KHR_dynamic_rendering",
        "VK_EXT_descriptor_indexing",
    };
    const char *use[3];
    uint32_t use_n = 0;
    for (uint32_t i = 0; i < ARRAY_SIZE(want); i++) {
        for (uint32_t j = 0; j < avail_n; j++) {
            if (!strcmp(want[i], avail[j].extensionName)) {
                use[use_n++] = want[i];
                break;
            }
        }
    }
    g_free(avail);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = c->qfam,
        .queueCount = 1,
        .pQueuePriorities = &prio,
    };

    /* Feature chain (request what lavapipe supports). Timeline semaphore is the
     * load-bearing one for §3 completion. */
    VkPhysicalDeviceTimelineSemaphoreFeatures tl = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
        .timelineSemaphore = VK_TRUE,
    };
    VkPhysicalDeviceDynamicRenderingFeatures dr = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES,
        .pNext = &tl,
        .dynamicRendering = VK_TRUE,
    };
    VkPhysicalDeviceDescriptorIndexingFeatures di = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES,
        .pNext = &dr,
        .runtimeDescriptorArray = VK_TRUE,
        .shaderSampledImageArrayNonUniformIndexing = VK_TRUE,
    };

    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &di,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
        .enabledExtensionCount = use_n,
        .ppEnabledExtensionNames = use_n ? use : NULL,
    };
    VkResult r = vkCreateDevice(c->phys, &dci, NULL, &c->dev);
    if (r != VK_SUCCESS) {
        VK_LOGF(c, "vkCreateDevice failed: %d", (int)r);
        return false;
    }
    vkGetDeviceQueue(c->dev, c->qfam, 0, &c->queue);
    return true;
}

/* Transient command pool on the single queue family. */
static bool pg_vk_create_cmd_pool(PGVkContext *c)
{
    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                 VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = c->qfam,
    };
    return vkCreateCommandPool(c->dev, &pci, NULL, &c->cmd_pool) == VK_SUCCESS;
}

/* The completion fence: one timeline semaphore, initial value 0. §3. */
static bool pg_vk_create_timeline(PGVkContext *c)
{
    VkSemaphoreTypeCreateInfo sti = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
    };
    VkSemaphoreCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &sti,
    };
    c->tl_next = 0;
    return vkCreateSemaphore(c->dev, &sci, NULL, &c->timeline) == VK_SUCCESS;
}

/* ----------------------------------------------------------------------------
 * READBACK BUFFER — HOST_VISIBLE|HOST_COHERENT, persistently mapped. §2.
 * Sized for a default desktop frame; grown on demand by pg_vk_ensure_readback().
 * --------------------------------------------------------------------------- */
static bool pg_vk_alloc_readback(PGVkContext *c, size_t cap)
{
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = cap,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(c->dev, &bci, NULL, &c->readback_buf) != VK_SUCCESS) {
        VK_LOGF(c, "readback vkCreateBuffer failed");
        return false;
    }
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(c->dev, c->readback_buf, &mr);
    uint32_t mt = pg_vk_mem_type(c, mr.memoryTypeBits,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == UINT32_MAX) {
        VK_LOGF(c, "no HOST_VISIBLE|HOST_COHERENT memory type for readback");
        vkDestroyBuffer(c->dev, c->readback_buf, NULL);
        c->readback_buf = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size,
        .memoryTypeIndex = mt,
    };
    if (vkAllocateMemory(c->dev, &mai, NULL, &c->readback_mem) != VK_SUCCESS) {
        VK_LOGF(c, "readback vkAllocateMemory failed");
        vkDestroyBuffer(c->dev, c->readback_buf, NULL);
        c->readback_buf = VK_NULL_HANDLE;
        return false;
    }
    vkBindBufferMemory(c->dev, c->readback_buf, c->readback_mem, 0);
    if (vkMapMemory(c->dev, c->readback_mem, 0, VK_WHOLE_SIZE, 0,
                    &c->readback_map) != VK_SUCCESS) {
        VK_LOGF(c, "readback vkMapMemory failed");
        return false;
    }
    c->readback_cap = cap;
    return true;
}

/* ----------------------------------------------------------------------------
 * COMPLETION THREAD.
 * Block on the timeline reaching each pending submission's value, then drive the
 * present + stamp. This generic ring-drain path is used by submissions enqueued
 * via the completion ring; the recognized-composite present + stamp is driven
 * inline by exec_indirect / pg_vk_present (which do the readback and present
 * directly). The stamp pulse must be marshalled to the device through a QEMU
 * bottom-half so it runs under the BQL.
 * --------------------------------------------------------------------------- */
static void *pg_vk_completion_thread(void *arg)
{
    PGVkContext *c = arg;
    for (;;) {
        PGVkPending ps;
        bool have = false;

        qemu_mutex_lock(&c->submit_lock);
        while (!c->shutting_down && c->pend_head == c->pend_tail) {
            qemu_cond_wait(&c->pending_cond, &c->submit_lock);
        }
        if (c->shutting_down && c->pend_head == c->pend_tail) {
            qemu_mutex_unlock(&c->submit_lock);
            break;
        }
        ps = c->pending[c->pend_head % PG_VK_PENDING_RING];
        have = true;
        qemu_mutex_unlock(&c->submit_lock);

        if (!have) {
            continue;
        }

        /* Block until the timeline reaches this submission's value. Values are
         * assigned monotonically per channel, so stamps signal in guest order. */
        VkSemaphoreWaitInfo wi = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1,
            .pSemaphores = &c->timeline,
            .pValues = &ps.value,
        };
        vkWaitSemaphores(c->dev, &wi, UINT64_MAX);

        /* For ring-enqueued submissions the present + stamp are marshalled here:
         * pg_signal_stamp() is static in apple-gfx-native.c and the IRQ pulse
         * must run under the BQL, so the stamp is delivered through a qemu_bh
         * that calls a NULL-safe stamp hook on the device. Recognized-composite
         * submits do their readback/present + stamp inline in exec_indirect, so
         * this path only logs and recycles for them. */
        VK_LOGF(c, "completion: timeline reached %llu stamp=0x%x surf=%u",
                (unsigned long long)ps.value, ps.stamp_value, ps.surface_id);

        /* Recycle the command buffer and advance the ring head. */
        qemu_mutex_lock(&c->submit_lock);
        if (ps.cb != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(c->dev, c->cmd_pool, 1, &ps.cb);
        }
        c->pend_head++;
        qemu_mutex_unlock(&c->submit_lock);
    }
    return NULL;
}

/* =====================================================================
 * PGVkOps implementation
 * ===================================================================== */

static PGVkContext *pg_vk_init(PGNativeDevice *dev,
                               const PGNativeHostOps *host, void *host_opaque)
{
    PGVkContext *c = g_new0(PGVkContext, 1);
    c->dev_back    = dev;
    c->host        = host;
    c->host_opaque = host_opaque;
    c->debug       = getenv("APPLE_GFX_VK_DEBUG") != NULL;
    /* APPLE_GFX_VK_QUAD selects the quad textured-quad present (real pipeline +
     * shaders) over the clear present; present() reads this flag. */
    c->quad_mode   = getenv("APPLE_GFX_VK_QUAD") != NULL;

    qemu_mutex_init(&c->submit_lock);
    qemu_cond_init(&c->pending_cond);

    if (!pg_vk_create_instance(c))      { goto fail; }
    if (!pg_vk_pick_physical(c))        { goto fail; }
    if (!pg_vk_pick_queue_family(c))    { goto fail; }
    if (!pg_vk_create_device(c))        { goto fail; }
    if (!pg_vk_create_cmd_pool(c))      { goto fail; }
    if (!pg_vk_create_timeline(c))      { goto fail; }
    /* Default to a 1920x1080 BGRA8 frame; grown on demand later. */
    if (!pg_vk_alloc_readback(c, (size_t)1920 * 1080 * 4)) { goto fail; }

    /* external_memory_host import is generally absent/unhelpful on lavapipe;
     * stage guest GPA-backed resources via read_mem/host_ptr (§1.6). */
    c->host_ptr_import_ok = false;

    c->shutting_down = false;
    qemu_thread_create(&c->completion_thr, "pvg-vk-complete",
                       pg_vk_completion_thread, c, QEMU_THREAD_JOINABLE);

    VK_LOGF(c, "lavapipe context up: instance+phys+device+queue+timeline+"
               "readback(%zuB) + completion thread (exec/present = STUB)",
            c->readback_cap);
    return c;

fail:
    VK_LOGF(c, "lavapipe context bring-up FAILED -> pg_vk_ops effectively NULL; "
               "native backend keeps immediate stamp-ack");
    /* Tear down whatever came up; on NULL the caller preserves the stub path. */
    if (c->timeline)      { vkDestroySemaphore(c->dev, c->timeline, NULL); }
    if (c->readback_map)  { vkUnmapMemory(c->dev, c->readback_mem); }
    if (c->readback_buf)  { vkDestroyBuffer(c->dev, c->readback_buf, NULL); }
    if (c->readback_mem)  { vkFreeMemory(c->dev, c->readback_mem, NULL); }
    if (c->cmd_pool)      { vkDestroyCommandPool(c->dev, c->cmd_pool, NULL); }
    if (c->dev)           { vkDestroyDevice(c->dev, NULL); }
    if (c->inst)          { vkDestroyInstance(c->inst, NULL); }
    qemu_cond_destroy(&c->pending_cond);
    qemu_mutex_destroy(&c->submit_lock);
    g_free(c);
    return NULL;
}

/* ============================================================================
 * exec_indirect — op-0x37 Metal-stream TRANSLATOR.
 *
 * Implements the "recognize-the-quad-signature" first frame of
 * docs/metal-vulkan-translator.md: parse the ExecIndirect ring residency header to
 * find the inner Metal command buffer + the destination render-target objectID,
 * walk the inner stream's 9-opcode render vocabulary, detect the fullscreen
 * textured-quad composite signature (bind pipeline -> bind a source texture at
 * fragment slot 3 -> DrawIndexedPrimitives16 count=6, repeated), and present the
 * recognized source through the existing fullscreen-quad pipeline
 * (q_pipeline / q_tex / q_dset / a3_img / readback / present_bgra8).
 *
 * Because op-0x37 carries resource *refs* only (the texture/buffer *contents* and
 * formats live in the object-creation ring — CmdDefineTask2 0x38 / CmdMapMemory2
 * 0x39 — which this TU does not see), the recognized source is rendered with a
 * stub procedurally-tinted texture keyed by the recognized objectIDs. That is the
 * doc's recommended first milestone ("replay with stub solid-color textures into a
 * 1920x1080 BGRA8 target to validate the draw path before wiring real contents").
 * Wiring real contents is the TODO that depends on the create-side decode.
 *
 * Submission uses the c->timeline signal (V = ++c->tl_next) and enqueues a
 * PGVkPending so the completion thread drives the stamp; this returns V (!=0) per
 * the PGVkOps contract, so the native side lets software-GPU completion ack.
 * (When APPLE_GFX_VK_EXEC is unset, or the stream is not a recognizable quad
 * composite, it returns 0 -> the native side keeps its immediate stamp.)
 * ============================================================================ */

/* Forward decls: the quad asset builder + the cached quad geometry/present plumbing
 * are defined later in the file (after this translator). */
static bool pg_vk_quad_ensure(PGVkContext *c, uint32_t W, uint32_t H);

/* Little-endian operand readers over the inner Metal stream (the stream is LE on
 * every host we target; do byte-wise so an unaligned record offset is safe). */
static inline uint32_t pg_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t pg_le64(const uint8_t *p)
{
    return (uint64_t)pg_le32(p) | ((uint64_t)pg_le32(p + 4) << 32);
}

/* Inner-stream render opcodes (docs/metal-vulkan-translator.md, the 9-opcode subset). */
enum {
    PG_M_DrawIndexed16        = 0x07, /* {primType,idxBufRef,u16 count,...}        */
    PG_M_SetFragmentBuffers   = 0x6e,
    PG_M_SetFragmentBufOffset = 0x6f,
    PG_M_SetFragmentSamplers  = 0x70,
    PG_M_SetFragmentTextures  = 0x72, /* {u32 start; u32 count} + count×u32 texRef */
    PG_M_SetRenderPipeline    = 0x74, /* {u32 pipelineRef}                          */
    PG_M_SetScissorRect       = 0x75,
    PG_M_SetVertexBuffers     = 0x7d,
    PG_M_SetVertexBufOffset   = 0x7e,
};

/* What the inner-stream walk recognizes from one op-0x37 submit. */
typedef struct PGExecScan {
    bool     is_quad_composite; /* >=1 textured-quad draw seen                     */
    uint32_t draw_count;        /* DrawIndexedPrimitives16 records                 */
    uint32_t quad_draw_count;   /* draws with indexCount==6 (a textured quad)      */
    uint32_t record_count;      /* total PGCmdHeader records walked                */
    uint32_t pipeline_ref;      /* last SetRenderPipelineState ref                 */
    uint32_t src_tex_ref;       /* last SetFragmentTextures slot-3 ref (the source)*/
    bool     framing_ok;        /* walked end-to-end with no framing error         */
} PGExecScan;

/*
 * Walk the inner Metal stream (PGCmdHeader-framed: {u32 opcode; u32 length},
 * length INCLUDES the 8-byte header, records self-delimiting). Recognizes the
 * fullscreen textured-quad composite signature. `stream`/`len` are a host-readable
 * copy of the inner command buffer. Bounded + defensive: any malformed length
 * aborts the walk (framing_ok stays false) rather than over-reading.
 */
static void pg_vk_scan_inner_stream(PGVkContext *c, const uint8_t *stream,
                                    uint32_t len, PGExecScan *out)
{
    memset(out, 0, sizeof(*out));

    /* The stream may be prefixed by a u64 protectionOptions + a small
     * PGSerializerCommandSegmentHeader {u32 byteLength; u8 encoderType; u8
     * hasCommands; u8 isFinal} (docs/metal-vulkan-translator.md §0.2). We don't need
     * its fields to recognize the quad signature, but we must skip past it to land
     * on the first PGCmdHeader. Probe: if the first 8 bytes don't look like a known
     * opcode + sane length, try skipping 8 (protectionOptions) then 12 (seghdr). */
    uint32_t off = 0;
    for (uint32_t probe = 0; probe < 3; probe++) {
        uint32_t skip = (probe == 0) ? 0 : (probe == 1) ? 8 : (8 + 12);
        if (skip + 8 > len) {
            continue;
        }
        uint32_t op  = pg_le32(stream + skip);
        uint32_t rlen = pg_le32(stream + skip + 4);
        if (rlen >= 8 && rlen <= (len - skip) &&
            (op == PG_M_SetRenderPipeline || op == PG_M_SetScissorRect ||
             op == PG_M_SetFragmentBuffers || op == PG_M_SetVertexBuffers ||
             op == PG_M_SetFragmentTextures || op == PG_M_DrawIndexed16)) {
            off = skip;
            break;
        }
    }

    /* Walk records by length. Cap iterations to a generous bound (a frame is ~157
     * records; the largest captured is 0xe20 bytes) to never spin on bad data.
     *
     * MULTI-SEGMENT: one resolved inner stream may contain several concatenated
     * Render segments, each prefixed by a PGSerializerCommandSegmentHeader whose
     * first u32 is {hi16=0x0d9c | lo16=segByteLen} and whose second u32 is the
     * byte length of *that segment's records* (byte-confirmed against captured
     * inner streams: seg hdrs at 0x0/0x120/0x1f0, records start at
     * segOff+8 and run for exactly recordsLen bytes — 0x118/0xc8/0x98). When the
     * length field op-0x37 carries (innerStreamLen, e.g. 0x290) spans PAST the
     * first segment's records into the next segment header, a strict length walk
     * mis-frames on the next header's bytes. So: when the next 8 bytes look like a
     * segment header, RESYNC to its records (segOff+8) instead of aborting; and
     * when we hit a record that overruns or isn't a known opcode AFTER having
     * cleanly parsed >=1 record, treat it as the end of valid records (clean stop)
     * rather than a framing failure. This makes recognition robust to the trailing
     * data that always follows the live records. */
    uint32_t guard = 0;
    bool walked_any = false;
    while (off + 8 <= len && guard++ < 4096) {
        uint32_t op   = pg_le32(stream + off);
        uint32_t rlen = pg_le32(stream + off + 4);

        /* Segment-header resync: {hi16==0x0d9c}. Skip the 8-byte seg header and
         * continue at its records. Bound: the records length (second u32) must fit. */
        if ((op >> 16) == 0x0d9c) {
            uint32_t seg_records = rlen;            /* second u32 = records bytes  */
            if (seg_records >= 8 && off + 8 + (uint64_t)seg_records <= len) {
                off += 8;                           /* land on first PGCmdHeader   */
                continue;
            }
            /* malformed seg header after clean records => stop cleanly */
            break;
        }

        /* Known-opcode + sane-length gate. A bad length or unknown opcode after we
         * have already parsed real records is the trailing-data boundary, not a
         * hard error — stop the walk but keep what we recognized (framing_ok). */
        bool known_op =
            (op == PG_M_SetRenderPipeline || op == PG_M_SetScissorRect ||
             op == PG_M_SetFragmentBuffers || op == PG_M_SetFragmentBufOffset ||
             op == PG_M_SetFragmentSamplers || op == PG_M_SetFragmentTextures ||
             op == PG_M_SetVertexBuffers || op == PG_M_SetVertexBufOffset ||
             op == PG_M_DrawIndexed16);
        if (rlen < 8 || off + rlen > len || !known_op) {
            if (walked_any) {
                break;        /* end of valid records; recognized state preserved  */
            }
            return;           /* never found valid framing at all                  */
        }
        const uint8_t *opnd = stream + off + 8;
        uint32_t opnd_len = rlen - 8;
        out->record_count++;

        switch (op) {
        case PG_M_SetRenderPipeline:
            if (opnd_len >= 4) {
                out->pipeline_ref = pg_le32(opnd);
            }
            break;
        case PG_M_SetFragmentTextures: {
            /* {u32 start; u32 count} + count×u32 texRef. The source texture for a
             * single-quad composite binds at fragment slot 3; capture the ref that
             * lands on (or nearest) slot 3, else the first ref. */
            if (opnd_len >= 8) {
                uint32_t start = pg_le32(opnd);
                uint32_t count = pg_le32(opnd + 4);
                for (uint32_t i = 0; i < count && 8 + i * 4 + 4 <= opnd_len; i++) {
                    uint32_t ref = pg_le32(opnd + 8 + i * 4);
                    if (start + i == 3 || out->src_tex_ref == 0) {
                        out->src_tex_ref = ref;
                    }
                }
            }
            break;
        }
        case PG_M_DrawIndexed16: {
            out->draw_count++;
            /* operand: {u32 primType; u32 idxBufRef; u16 indexCount; ...}. */
            uint32_t index_count = (opnd_len >= 10) ? (uint32_t)pg_le32(opnd + 8) & 0xffff
                                 : (opnd_len >= 8)  ? 0 : 0;
            if (index_count == 6) {
                out->quad_draw_count++;
            }
            break;
        }
        default:
            break;   /* the other 6 opcodes are bind state we replay implicitly */
        }
        off += rlen;
        walked_any = true;
    }
    /* framing_ok = we parsed at least one well-framed record (the live stream is
     * followed by trailing data / further segments, so reaching exact EOF is NOT
     * the success criterion — a clean run of records is). */
    out->framing_ok = walked_any;
    /* A recognizable composite = parsed clean records AND drew at least one quad
     * (a 6-index textured draw) under a bound pipeline. */
    out->is_quad_composite = out->framing_ok && out->quad_draw_count > 0;
}

/*
 * Resolve the inner-stream bytes into a host-readable buffer. The ExecIndirect
 * residency header's trailing u64s are {innerStreamGPA, innerStreamLen}; the GPA
 * is what the native side passes as already-resolved-to-RAM (or a task VA we try
 * directly). Prefer host_ptr (zero-copy); fall back to read_mem into `bounce`.
 * Returns a readable pointer + sets *out_len, or NULL.
 */
static const uint8_t *pg_vk_resolve_inner(PGVkContext *c, uint64_t inner_gpa,
                                          uint64_t inner_len, uint8_t *bounce,
                                          size_t bounce_cap, uint32_t *out_len)
{
    if (!inner_gpa || !inner_len) {
        return NULL;
    }
    uint64_t want = inner_len > bounce_cap ? bounce_cap : inner_len;
    *out_len = (uint32_t)want;

    if (c->host && c->host->host_ptr) {
        void *p = c->host->host_ptr(c->host_opaque, inner_gpa, want, true);
        if (p) {
            return (const uint8_t *)p;
        }
    }
    if (c->host && c->host->read_mem) {
        if (c->host->read_mem(c->host_opaque, inner_gpa, want, bounce)) {
            return bounce;
        }
    }
    return NULL;
}

/*
 * Render the recognized fullscreen textured quad into the cached 1920x1080 a3_img
 * via the quad pipeline, sampling a STUB source texture tinted by the recognized
 * objectIDs (real texture contents are not in op-0x37 — see header). Submits with
 * the timeline signal V and enqueues a PGVkPending so completion drives the stamp.
 * Returns V (the signalled timeline value) on success, 0 on failure.
 *
 * NOTE: this reuses pg_vk_quad_ensure to build/cache q_pipeline/q_tex/q_dset/
 * a3_img (the SAME assets pg_vk_present_textured/_guest use); the q_dset binding 0
 * already points at the cached procedural q_tex, which we keep as the stub source.
 */
static uint64_t pg_vk_exec_replay_quad(PGVkContext *c, const PGExecScan *scan,
                                       uint32_t dst_object_id,
                                       uint32_t stamp_value, uint32_t surface_id)
{
    const uint32_t W = 1920, H = 1080;
    const VkDeviceSize out_stride = (VkDeviceSize)W * 4;
    const size_t out_need = (size_t)W * H * 4;
    uint64_t signalled = 0;

    qemu_mutex_lock(&c->submit_lock);

    VkCommandBuffer cb = VK_NULL_HANDLE;

    if (!pg_vk_quad_ensure(c, W, H)) {
        VK_LOGF(c, "exec replay: quad asset build failed");
        goto out;
    }

    /* Point the cached quad descriptor set's binding 0 back at the procedural stub
     * source (q_tex). (pg_vk_present_guest may have re-pointed it at g_tex; restore
     * it so the replay path is self-consistent.) */
    if (c->q_tex_view != VK_NULL_HANDLE) {
        VkDescriptorImageInfo dii = {
            .sampler = c->q_sampler,
            .imageView = c->q_tex_view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkWriteDescriptorSet wds = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = c->q_dset, .dstBinding = 0, .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &dii,
        };
        vkUpdateDescriptorSets(c->dev, 1, &wds, 0, NULL);
    }

    if (c->readback_cap < out_need || c->readback_buf == VK_NULL_HANDLE) {
        if (c->readback_map) { vkUnmapMemory(c->dev, c->readback_mem); c->readback_map = NULL; }
        if (c->readback_buf) { vkDestroyBuffer(c->dev, c->readback_buf, NULL); c->readback_buf = VK_NULL_HANDLE; }
        if (c->readback_mem) { vkFreeMemory(c->dev, c->readback_mem, NULL); c->readback_mem = VK_NULL_HANDLE; }
        if (!pg_vk_alloc_readback(c, out_need)) {
            VK_LOGF(c, "exec replay: readback (re)alloc failed");
            goto out;
        }
    }

    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = c->cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (vkAllocateCommandBuffers(c->dev, &cbai, &cb) != VK_SUCCESS) {
        VK_LOGF(c, "exec replay: vkAllocateCommandBuffers failed");
        cb = VK_NULL_HANDLE;
        goto out;
    }
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) {
        VK_LOGF(c, "exec replay: vkBeginCommandBuffer failed");
        goto out;
    }

    VkImageSubresourceRange full = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0, .levelCount = 1,
        .baseArrayLayer = 0, .layerCount = 1,
    };

    /* a3_img: UNDEFINED -> COLOR_ATTACHMENT (loadOp=CLEAR). */
    VkImageMemoryBarrier to_color = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = c->a3_img,
        .subresourceRange = full,
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                         0, NULL, 0, NULL, 1, &to_color);

    VkRenderingAttachmentInfo color_att = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = c->a3_view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = { .color = { .float32 = { 0.02f, 0.02f, 0.06f, 1.0f } } },
    };
    VkRenderingInfo ri = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = { { 0, 0 }, { W, H } },
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_att,
    };
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, c->q_pipeline);
    VkDeviceSize voff = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &c->q_vtx_buf, &voff);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, c->q_pl_layout,
                            0, 1, &c->q_dset, 0, NULL);
    static const float mvp_identity[16] = {
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1,
    };
    vkCmdPushConstants(cb, c->q_pl_layout, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(mvp_identity), mvp_identity);
    VkViewport vp = { 0, 0, (float)W, (float)H, 0.0f, 1.0f };
    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D sc = { { 0, 0 }, { W, H } };
    vkCmdSetScissor(cb, 0, 1, &sc);
    /* Replay the recognized quad draw(s). Each op-0x37 DrawIndexedPrimitives16
     * count=6 is one textured quad; we issue the quad fullscreen quad once per
     * recognized quad-draw (the stub source is the same; geometry/scissor of the
     * real sub-quads needs the per-draw vertex/scissor operands, a TODO). */
    uint32_t draws = scan->quad_draw_count ? scan->quad_draw_count : 1;
    if (draws > 64) { draws = 64; }   /* bound */
    for (uint32_t i = 0; i < draws; i++) {
        vkCmdDraw(cb, c->q_vtx_count, 1, 0, 0);
    }
    vkCmdEndRendering(cb);

    /* a3_img COLOR_ATTACHMENT -> TRANSFER_SRC, copy out to readback. */
    VkImageMemoryBarrier to_src = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = c->a3_img,
        .subresourceRange = full,
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 1, &to_src);
    VkBufferImageCopy copy = {
        .bufferOffset = 0, .bufferRowLength = 0, .bufferImageHeight = 0,
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1,
        },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = { W, H, 1 },
    };
    vkCmdCopyImageToBuffer(cb, c->a3_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           c->readback_buf, 1, &copy);

    if (vkEndCommandBuffer(cb) != VK_SUCCESS) {
        VK_LOGF(c, "exec replay: vkEndCommandBuffer failed");
        goto out;
    }

    /* Submit signalling the timeline at V = ++c->tl_next, so the completion thread
     * can block on it and (eventually) drive the stamp. */
    signalled = ++c->tl_next;
    VkTimelineSemaphoreSubmitInfo tssi = {
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .signalSemaphoreValueCount = 1,
        .pSignalSemaphoreValues = &signalled,
    };
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = &tssi,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &c->timeline,
    };
    VkResult r = vkQueueSubmit(c->queue, 1, &si, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) {
        VK_LOGF(c, "exec replay: vkQueueSubmit failed: %d", (int)r);
        c->tl_next--;            /* roll back the unused value */
        signalled = 0;
        goto out;
    }

    /* Deliver the rendered pixels NOW (synchronous present is fine: lavapipe; the
     * timeline value is also signalled for the completion-thread stamp path). The
     * readback is HOST_COHERENT but the GPU may not be done — wait for THIS
     * submission's timeline value before reading it back. */
    VkSemaphoreWaitInfo wi = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores = &c->timeline,
        .pValues = &signalled,
    };
    vkWaitSemaphores(c->dev, &wi, UINT64_MAX);
    if (c->host && c->host->present_bgra8) {
        c->host->present_bgra8(c->host_opaque, W, H, (uint32_t)out_stride,
                               c->readback_map);
    }

    /* Enqueue the pending-completion record (the completion thread recycles the cb
     * and logs the stamp; the timeline value is already reached). */
    {
        PGVkPending ps = {
            .value = signalled,
            .stamp_value = stamp_value,
            .surface_id = surface_id,
            .cb = cb,
        };
        c->pending[c->pend_tail % PG_VK_PENDING_RING] = ps;
        c->pend_tail++;
        qemu_cond_signal(&c->pending_cond);
        cb = VK_NULL_HANDLE;     /* ownership handed to the completion thread */
    }

    VK_LOGF(c, "exec replay OK: dst=0x%x pipe=0x%x src=0x%x quads=%u "
               "-> quad present %ux%u, timeline V=%llu stamp=0x%x",
            dst_object_id, scan->pipeline_ref, scan->src_tex_ref,
            scan->quad_draw_count, W, H,
            (unsigned long long)signalled, stamp_value);

out:
    if (cb != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(c->dev, c->cmd_pool, 1, &cb);
    }
    qemu_mutex_unlock(&c->submit_lock);
    return signalled;
}

static uint64_t pg_vk_exec_indirect(PGVkContext *c, uint32_t task_id,
                                    const uint8_t *payload, uint32_t payload_len,
                                    uint32_t stamp_value)
{
    /* Opt-in: the translator path is gated so the proven immediate-stamp behaviour
     * stays the default until the create-side resource decode lands. */
    static int en = -1;
    if (en < 0) {
        en = getenv("APPLE_GFX_VK_EXEC") ? 1 : 0;
    }
    if (!en) {
        VK_LOGF(c, "exec_indirect task=%u len=%u stamp=0x%x (translator gated off "
                   "[APPLE_GFX_VK_EXEC]; immediate stamp)",
                task_id, payload_len, stamp_value);
        return 0;
    }

    /* Parse the ExecIndirect ring residency header (docs/metal-vulkan-translator.md
     * §0.1). The trailing u64s are the inner stream
     * {GPA,len}; the dst render-target objectID is the first residency entry (the
     * variant-0/taskID=1 fixed offsets; for variant>0 we re-anchor on the triplet).
     * payload must be at least the two trailing u64s. */
    if (!payload || payload_len < 0x10) {
        VK_LOGF(c, "exec_indirect task=%u len=%u: payload too short for header",
                task_id, payload_len);
        return 0;
    }
    uint64_t inner_gpa = pg_le64(payload + payload_len - 0x10);
    uint64_t inner_len = pg_le64(payload + payload_len - 0x08);

    /* Destination render-target objectID. Variant-0 taskID=1 puts it at +0x28
     * (docs/metal-vulkan-translator.md §0.1); fall back to the first residency entry's
     * objectID (right after the fixed +0x30 header) if that offset is out of range
     * or zero. Best-effort: it only tints the stub source. */
    uint32_t dst_object_id = 0;
    if (payload_len >= 0x2c) {
        dst_object_id = pg_le32(payload + 0x28);
    }
    if (dst_object_id == 0 && payload_len >= 0x34) {
        dst_object_id = pg_le32(payload + 0x30);   /* first ENTRY objectID */
    }

    /* Resolve + walk the inner Metal stream. */
    static uint8_t bounce[8192];
    uint32_t got = 0;
    const uint8_t *stream =
        pg_vk_resolve_inner(c, inner_gpa, inner_len, bounce, sizeof(bounce), &got);
    if (!stream || got < 8) {
        VK_LOGF(c, "exec_indirect task=%u: inner stream GPA=0x%llx len=%llu "
                   "UNRESOLVED (host_ptr/read_mem); immediate stamp",
                task_id, (unsigned long long)inner_gpa,
                (unsigned long long)inner_len);
        return 0;
    }

    PGExecScan scan;
    pg_vk_scan_inner_stream(c, stream, got, &scan);
    VK_LOGF(c, "exec_indirect task=%u dst=0x%x innerGPA=0x%llx len=%llu(%u): "
               "records=%u draws=%u quads=%u pipe=0x%x src=0x%x framing=%d quad=%d",
            task_id, dst_object_id, (unsigned long long)inner_gpa,
            (unsigned long long)inner_len, got, scan.record_count,
            scan.draw_count, scan.quad_draw_count, scan.pipeline_ref,
            scan.src_tex_ref, scan.framing_ok, scan.is_quad_composite);

    if (!scan.is_quad_composite) {
        /* Not a recognizable textured-quad composite (or framing failed) -> let
         * the native side keep its immediate stamp for this submit. */
        return 0;
    }

    /* Replay -> present -> timeline-signalled submit + pending enqueue. */
    return pg_vk_exec_replay_quad(c, &scan, dst_object_id, stamp_value,
                                  /*surface_id*/ dst_object_id);
}

/* ----------------------------------------------------------------------------
 * TEST IMAGE — a 1920x1080 BGRA8 color/transfer-src VkImage, cached in the
 * context and reused across presents. Created lazily on the first present().
 * --------------------------------------------------------------------------- */
static bool pg_vk_ensure_a3_image(PGVkContext *c, uint32_t w, uint32_t h)
{
    if (c->a3_img != VK_NULL_HANDLE && c->a3_w == w && c->a3_h == h) {
        return true;   /* already have a cached image of this geometry */
    }
    /* Geometry changed (or first call): drop any previous image. The quad
     * color-attachment view (a3_view) is bound to a3_img, so drop it too — it is
     * lazily recreated in pg_vk_quad_ensure. */
    if (c->a3_view != VK_NULL_HANDLE) {
        vkDestroyImageView(c->dev, c->a3_view, NULL);
        c->a3_view = VK_NULL_HANDLE;
    }
    if (c->a3_img != VK_NULL_HANDLE) {
        vkDestroyImage(c->dev, c->a3_img, NULL);
        c->a3_img = VK_NULL_HANDLE;
    }
    if (c->a3_mem != VK_NULL_HANDLE) {
        vkFreeMemory(c->dev, c->a3_mem, NULL);
        c->a3_mem = VK_NULL_HANDLE;
    }

    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .extent = { w, h, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(c->dev, &ici, NULL, &c->a3_img) != VK_SUCCESS) {
        VK_LOGF(c, "clear vkCreateImage(%ux%u) failed", w, h);
        c->a3_img = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(c->dev, c->a3_img, &mr);
    /* lavapipe memory is system RAM; DEVICE_LOCAL is the natural choice but any
     * type satisfying the requirement bits works (findMemoryType handles it). */
    uint32_t mt = pg_vk_mem_type(c, mr.memoryTypeBits,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) {
        mt = pg_vk_mem_type(c, mr.memoryTypeBits, 0);   /* any type at all */
    }
    if (mt == UINT32_MAX) {
        VK_LOGF(c, "clear: no memory type for test image");
        vkDestroyImage(c->dev, c->a3_img, NULL);
        c->a3_img = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size,
        .memoryTypeIndex = mt,
    };
    if (vkAllocateMemory(c->dev, &mai, NULL, &c->a3_mem) != VK_SUCCESS) {
        VK_LOGF(c, "clear vkAllocateMemory failed");
        vkDestroyImage(c->dev, c->a3_img, NULL);
        c->a3_img = VK_NULL_HANDLE;
        return false;
    }
    if (vkBindImageMemory(c->dev, c->a3_img, c->a3_mem, 0) != VK_SUCCESS) {
        VK_LOGF(c, "clear vkBindImageMemory failed");
        vkFreeMemory(c->dev, c->a3_mem, NULL);
        vkDestroyImage(c->dev, c->a3_img, NULL);
        c->a3_mem = VK_NULL_HANDLE;
        c->a3_img = VK_NULL_HANDLE;
        return false;
    }
    c->a3_w = w;
    c->a3_h = h;
    return true;
}

/* ============================================================================
 * TEXTURED-QUAD PRESENT — the real present mechanism.
 *
 * Unlike the clear present (which only vkCmdClearColorImage's the cached
 * image), this drives a full graphics pipeline: the two embedded SPIR-V shaders
 * (pvg_vk_shaders.h) render a fullscreen textured quad sampling a recognizable
 * procedural source texture into the cached 1920x1080 a3_img via dynamic
 * rendering, then copies it out to the readback buffer and presents.
 *
 * Shader interface matched EXACTLY from pvg_vk_shaders.h (verified against the
 * SPIR-V decorations, not just the GLSL comment):
 *   SimpleVertex (vertex stage):
 *     - in vec2 pos  @ location 0   -> VK_FORMAT_R32G32_SFLOAT
 *     - in vec2 tex  @ location 1   -> VK_FORMAT_R32G32_SFLOAT
 *     - out vec2 uv  @ location 0
 *     - push_constant { mat4 mvp; }  : 64 bytes @ offset 0, stage VERTEX
 *       gl_Position = mvp * vec4(pos, 0, 1)
 *   SimpleTextureFragment (fragment stage):
 *     - uniform sampler2D src : set 0, binding 0 (COMBINED_IMAGE_SAMPLER),
 *       stage FRAGMENT
 *     - in vec2 uv @ location 0 ; out vec4 outColor @ location 0
 *       outColor = texture(src, uv)
 *
 * SimpleVertex layout: { float pos[2]; float uv[2]; } -> stride 16, pos @ 0,
 * uv @ 8.  Quad as a 4-vertex TRIANGLE_STRIP covering clip-space [-1,1]^2 with
 * uv [0,1]^2.  Y is NOT flipped here (the clear path presents a3_img top-down and
 * looks correct over VNC, so we keep the same orientation; the shader header's
 * flip note applies to a real WSI swapchain, not our readback->present_bgra8).
 *
 * All objects are created lazily on first textured present and cached; destroyed
 * by pg_vk_quad_destroy() from pg_vk_shutdown.
 * ============================================================================ */

/* SimpleVertex CPU mirror — must match the vertex input layout below exactly. */
typedef struct { float pos[2]; float uv[2]; } PGVkSimpleVertex;

/* Tear down the cached quad assets (idempotent; NULL-handle safe). */
static void pg_vk_quad_destroy(PGVkContext *c)
{
    if (!c->dev) {
        return;
    }
    if (c->q_pipeline)   { vkDestroyPipeline(c->dev, c->q_pipeline, NULL);   c->q_pipeline = VK_NULL_HANDLE; }
    if (c->q_pl_layout)  { vkDestroyPipelineLayout(c->dev, c->q_pl_layout, NULL); c->q_pl_layout = VK_NULL_HANDLE; }
    if (c->q_dpool)      { vkDestroyDescriptorPool(c->dev, c->q_dpool, NULL); c->q_dpool = VK_NULL_HANDLE; c->q_dset = VK_NULL_HANDLE; }
    if (c->q_dsl)        { vkDestroyDescriptorSetLayout(c->dev, c->q_dsl, NULL); c->q_dsl = VK_NULL_HANDLE; }
    if (c->q_vs)         { vkDestroyShaderModule(c->dev, c->q_vs, NULL); c->q_vs = VK_NULL_HANDLE; }
    if (c->q_fs)         { vkDestroyShaderModule(c->dev, c->q_fs, NULL); c->q_fs = VK_NULL_HANDLE; }
    if (c->q_vtx_buf)    { vkDestroyBuffer(c->dev, c->q_vtx_buf, NULL); c->q_vtx_buf = VK_NULL_HANDLE; }
    if (c->q_vtx_mem)    { vkFreeMemory(c->dev, c->q_vtx_mem, NULL); c->q_vtx_mem = VK_NULL_HANDLE; }
    if (c->q_sampler)    { vkDestroySampler(c->dev, c->q_sampler, NULL); c->q_sampler = VK_NULL_HANDLE; }
    if (c->q_tex_view)   { vkDestroyImageView(c->dev, c->q_tex_view, NULL); c->q_tex_view = VK_NULL_HANDLE; }
    if (c->q_tex_img)    { vkDestroyImage(c->dev, c->q_tex_img, NULL); c->q_tex_img = VK_NULL_HANDLE; }
    if (c->q_tex_mem)    { vkFreeMemory(c->dev, c->q_tex_mem, NULL); c->q_tex_mem = VK_NULL_HANDLE; }
    c->quad_ready = false;
}

/* Create + upload the procedural source texture (256x256 BGRA8 sampled image).
 * Fills a HOST_VISIBLE staging buffer with a recognizable pattern (an 8x8
 * checkerboard overlaid on a 2-axis R(x)/G(y) gradient), copies it into a
 * device-local SHADER_READ_ONLY_OPTIMAL image, and creates its view. The upload
 * uses its own one-shot command buffer + queue wait (build-time, once). */
static bool pg_vk_quad_make_texture(PGVkContext *c)
{
    /* --- REAL-CONTENTS SOURCE (Step 3): the op-0x37 stream carries texture
     * *refs* only; the contents live in the create-side object ring / guest RAM.
     * When APPLE_GFX_VK_REAL_TEX names a raw BGRA8 file (optionally "path:WxH",
     * default 1920x1200), we upload those REAL pixels as the sampled source
     * instead of the 256x256 procedural stub. Supply your own raw BGRA8 file
     * (you produce it offline by carving the linear, non-AGX-compressed backing
     * of the background/scanout objectID from your own guest's RAM — see
     * docs/metal-vulkan-translator.md §2). The procedural stub remains the
     * fallback for unresolved refs (env unset / load fails). */
    uint32_t TW = 256, TH = 256;
    uint8_t *real_px = NULL;          /* loaded BGRA8 bytes, freed before return */
    VkDeviceSize real_bytes = 0;
    {
        const char *spec = getenv("APPLE_GFX_VK_REAL_TEX");
        if (spec && *spec) {
            char path[1024]; uint32_t rw = 1920, rh = 1200;
            const char *colon = strrchr(spec, ':');
            if (colon && strchr(colon, 'x')) {
                size_t n = (size_t)(colon - spec);
                if (n >= sizeof(path)) { n = sizeof(path) - 1; }
                memcpy(path, spec, n); path[n] = 0;
                unsigned a = 0, b = 0;
                if (sscanf(colon + 1, "%ux%u", &a, &b) == 2 && a && b) { rw = a; rh = b; }
            } else {
                size_t n = strlen(spec);
                if (n >= sizeof(path)) { n = sizeof(path) - 1; }
                memcpy(path, spec, n); path[n] = 0;
            }
            FILE *rf = fopen(path, "rb");
            if (rf) {
                VkDeviceSize want = (VkDeviceSize)rw * rh * 4;
                real_px = malloc(want);
                size_t got = real_px ? fread(real_px, 1, want, rf) : 0;
                fclose(rf);
                if (real_px && got == want) {
                    TW = rw; TH = rh; real_bytes = want;
                    VK_LOGF(c, "quad tex: loaded REAL source %ux%u from APPLE_GFX_VK_REAL_TEX", TW, TH);
                } else {
                    VK_LOGF(c, "quad tex: REAL source load short (%zu/%llu) -> procedural stub",
                            got, (unsigned long long)want);
                    free(real_px); real_px = NULL;
                }
            } else {
                VK_LOGF(c, "quad tex: APPLE_GFX_VK_REAL_TEX open failed -> procedural stub");
            }
        }
    }
    const VkDeviceSize tex_bytes = (VkDeviceSize)TW * TH * 4;

    /* (a) the sampled device-local image. */
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .extent = { TW, TH, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(c->dev, &ici, NULL, &c->q_tex_img) != VK_SUCCESS) {
        VK_LOGF(c, "quad tex vkCreateImage failed");
        c->q_tex_img = VK_NULL_HANDLE;
        free(real_px);
        return false;
    }
    VkMemoryRequirements imr;
    vkGetImageMemoryRequirements(c->dev, c->q_tex_img, &imr);
    uint32_t imt = pg_vk_mem_type(c, imr.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (imt == UINT32_MAX) { imt = pg_vk_mem_type(c, imr.memoryTypeBits, 0); }
    if (imt == UINT32_MAX) {
        VK_LOGF(c, "quad tex no image memory type");
        free(real_px);
        return false;
    }
    VkMemoryAllocateInfo imai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = imr.size,
        .memoryTypeIndex = imt,
    };
    if (vkAllocateMemory(c->dev, &imai, NULL, &c->q_tex_mem) != VK_SUCCESS ||
        vkBindImageMemory(c->dev, c->q_tex_img, c->q_tex_mem, 0) != VK_SUCCESS) {
        VK_LOGF(c, "quad tex image alloc/bind failed");
        free(real_px);
        return false;
    }

    /* (b) a HOST_VISIBLE staging buffer holding the procedural pattern. */
    VkBuffer       stg = VK_NULL_HANDLE;
    VkDeviceMemory stg_mem = VK_NULL_HANDLE;
    VkBufferCreateInfo sbci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = tex_bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(c->dev, &sbci, NULL, &stg) != VK_SUCCESS) {
        VK_LOGF(c, "quad tex staging vkCreateBuffer failed");
        free(real_px);
        return false;
    }
    VkMemoryRequirements smr;
    vkGetBufferMemoryRequirements(c->dev, stg, &smr);
    uint32_t smt = pg_vk_mem_type(c, smr.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (smt == UINT32_MAX) {
        VK_LOGF(c, "quad tex no HOST_VISIBLE staging memory type");
        vkDestroyBuffer(c->dev, stg, NULL);
        free(real_px);
        return false;
    }
    VkMemoryAllocateInfo smai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = smr.size,
        .memoryTypeIndex = smt,
    };
    if (vkAllocateMemory(c->dev, &smai, NULL, &stg_mem) != VK_SUCCESS ||
        vkBindBufferMemory(c->dev, stg, stg_mem, 0) != VK_SUCCESS) {
        VK_LOGF(c, "quad tex staging alloc/bind failed");
        vkDestroyBuffer(c->dev, stg, NULL);
        if (stg_mem) { vkFreeMemory(c->dev, stg_mem, NULL); }
        free(real_px);
        return false;
    }
    void *map = NULL;
    if (vkMapMemory(c->dev, stg_mem, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS) {
        VK_LOGF(c, "quad tex staging vkMapMemory failed");
        vkDestroyBuffer(c->dev, stg, NULL);
        vkFreeMemory(c->dev, stg_mem, NULL);
        free(real_px);
        return false;
    }
    if (real_px) {
        /* REAL extracted BGRA8 source (already in B,G,R,A byte order). */
        memcpy(map, real_px, real_bytes);
    } else {
        /* Procedural BGRA8 stub: checkerboard tints a 2-axis color gradient. */
        uint8_t *px = map;            /* byte order: B, G, R, A */
        for (uint32_t y = 0; y < TH; y++) {
            for (uint32_t x = 0; x < TW; x++) {
                uint8_t r = (uint8_t)x;                 /* R ramps with x */
                uint8_t g = (uint8_t)y;                 /* G ramps with y */
                uint8_t b = 128;
                bool checker = (((x >> 5) ^ (y >> 5)) & 1) != 0; /* 32px cells */
                if (checker) { r ^= 0xff; g ^= 0xff; b = 0xff; }
                uint8_t *p = px + ((size_t)y * TW + x) * 4;
                p[0] = b; p[1] = g; p[2] = r; p[3] = 0xff;
            }
        }
    }
    vkUnmapMemory(c->dev, stg_mem);

    /* (c) one-shot cmd buffer: UNDEFINED->TRANSFER_DST, copy, ->SHADER_READ. */
    bool ok = false;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = c->cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (vkAllocateCommandBuffers(c->dev, &cbai, &cb) != VK_SUCCESS) {
        VK_LOGF(c, "quad tex vkAllocateCommandBuffers failed");
        cb = VK_NULL_HANDLE;
        goto tex_out;
    }
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) {
        VK_LOGF(c, "quad tex vkBeginCommandBuffer failed");
        goto tex_out;
    }

    VkImageSubresourceRange full = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0, .levelCount = 1,
        .baseArrayLayer = 0, .layerCount = 1,
    };
    VkImageMemoryBarrier to_dst = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = c->q_tex_img,
        .subresourceRange = full,
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 1, &to_dst);

    VkBufferImageCopy bic = {
        .bufferOffset = 0,
        .bufferRowLength = 0,        /* tightly packed: stride == TW*4 */
        .bufferImageHeight = 0,
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1,
        },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = { TW, TH, 1 },
    };
    vkCmdCopyBufferToImage(cb, stg, c->q_tex_img,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);

    VkImageMemoryBarrier to_read = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = c->q_tex_img,
        .subresourceRange = full,
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                         0, NULL, 0, NULL, 1, &to_read);

    if (vkEndCommandBuffer(cb) != VK_SUCCESS) {
        VK_LOGF(c, "quad tex vkEndCommandBuffer failed");
        goto tex_out;
    }
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
    };
    if (vkQueueSubmit(c->queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) {
        VK_LOGF(c, "quad tex vkQueueSubmit failed");
        goto tex_out;
    }
    vkQueueWaitIdle(c->queue);

    /* (d) the sampled view. */
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = c->q_tex_img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .components = {
            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
        },
        .subresourceRange = full,
    };
    if (vkCreateImageView(c->dev, &vci, NULL, &c->q_tex_view) != VK_SUCCESS) {
        VK_LOGF(c, "quad tex vkCreateImageView failed");
        c->q_tex_view = VK_NULL_HANDLE;
        goto tex_out;
    }
    c->q_tex_w = TW;
    c->q_tex_h = TH;
    ok = true;

tex_out:
    if (real_px) { free(real_px); }
    if (cb != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(c->dev, c->cmd_pool, 1, &cb);
    }
    if (stg)     { vkDestroyBuffer(c->dev, stg, NULL); }
    if (stg_mem) { vkFreeMemory(c->dev, stg_mem, NULL); }
    return ok;
}

/* Create + upload the fullscreen-quad vertex buffer (4 SimpleVertex, strip). */
static bool pg_vk_quad_make_vertices(PGVkContext *c)
{
    /* Clip-space [-1,1]^2, uv [0,1]^2. TRIANGLE_STRIP order: TL, BL, TR, BR.
     * uv.y follows clip.y (top=-1 -> v=0) so the texture is not flipped. */
    static const PGVkSimpleVertex quad[4] = {
        { { -1.0f, -1.0f }, { 0.0f, 0.0f } },   /* top-left     */
        { { -1.0f,  1.0f }, { 0.0f, 1.0f } },   /* bottom-left  */
        { {  1.0f, -1.0f }, { 1.0f, 0.0f } },   /* top-right    */
        { {  1.0f,  1.0f }, { 1.0f, 1.0f } },   /* bottom-right */
    };
    const VkDeviceSize bytes = sizeof(quad);

    /* HOST_VISIBLE|HOST_COHERENT vertex buffer (lavapipe is system RAM anyway;
     * mapping + memcpy avoids a second staging copy for these 64 bytes). */
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(c->dev, &bci, NULL, &c->q_vtx_buf) != VK_SUCCESS) {
        VK_LOGF(c, "quad vtx vkCreateBuffer failed");
        c->q_vtx_buf = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(c->dev, c->q_vtx_buf, &mr);
    uint32_t mt = pg_vk_mem_type(c, mr.memoryTypeBits,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == UINT32_MAX) {
        VK_LOGF(c, "quad vtx no HOST_VISIBLE memory type");
        return false;
    }
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size,
        .memoryTypeIndex = mt,
    };
    if (vkAllocateMemory(c->dev, &mai, NULL, &c->q_vtx_mem) != VK_SUCCESS ||
        vkBindBufferMemory(c->dev, c->q_vtx_buf, c->q_vtx_mem, 0) != VK_SUCCESS) {
        VK_LOGF(c, "quad vtx alloc/bind failed");
        return false;
    }
    void *map = NULL;
    if (vkMapMemory(c->dev, c->q_vtx_mem, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS) {
        VK_LOGF(c, "quad vtx vkMapMemory failed");
        return false;
    }
    memcpy(map, quad, (size_t)bytes);
    vkUnmapMemory(c->dev, c->q_vtx_mem);
    c->q_vtx_count = 4;
    return true;
}

/* Create the descriptor set layout/pool/set + sampler + pipeline-layout +
 * graphics pipeline (the 2 shader modules), matching the shader interface. */
static bool pg_vk_quad_make_pipeline(PGVkContext *c)
{
    /* (a) static sampler: linear filter, clamp-to-edge. */
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 0.0f,
        .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
    };
    if (vkCreateSampler(c->dev, &sci, NULL, &c->q_sampler) != VK_SUCCESS) {
        VK_LOGF(c, "quad vkCreateSampler failed");
        c->q_sampler = VK_NULL_HANDLE;
        return false;
    }

    /* (b) descriptor set layout: set 0, binding 0 = COMBINED_IMAGE_SAMPLER,
     * stage FRAGMENT (SimpleTextureFragment `src`). */
    VkDescriptorSetLayoutBinding dslb = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .pImmutableSamplers = NULL,
    };
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &dslb,
    };
    if (vkCreateDescriptorSetLayout(c->dev, &dslci, NULL, &c->q_dsl) != VK_SUCCESS) {
        VK_LOGF(c, "quad vkCreateDescriptorSetLayout failed");
        c->q_dsl = VK_NULL_HANDLE;
        return false;
    }

    /* (c) descriptor pool + set, then point binding 0 at the texture+sampler. */
    VkDescriptorPoolSize dps = {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
    };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &dps,
    };
    if (vkCreateDescriptorPool(c->dev, &dpci, NULL, &c->q_dpool) != VK_SUCCESS) {
        VK_LOGF(c, "quad vkCreateDescriptorPool failed");
        c->q_dpool = VK_NULL_HANDLE;
        return false;
    }
    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = c->q_dpool,
        .descriptorSetCount = 1,
        .pSetLayouts = &c->q_dsl,
    };
    if (vkAllocateDescriptorSets(c->dev, &dsai, &c->q_dset) != VK_SUCCESS) {
        VK_LOGF(c, "quad vkAllocateDescriptorSets failed");
        c->q_dset = VK_NULL_HANDLE;
        return false;
    }
    VkDescriptorImageInfo dii = {
        .sampler = c->q_sampler,
        .imageView = c->q_tex_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet wds = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = c->q_dset,
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &dii,
    };
    vkUpdateDescriptorSets(c->dev, 1, &wds, 0, NULL);

    /* (d) pipeline layout: the one DSL + a 64-byte VERTEX push constant (mvp). */
    VkPushConstantRange pcr = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = 16 * sizeof(float),   /* mat4 mvp = 64 bytes */
    };
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &c->q_dsl,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcr,
    };
    if (vkCreatePipelineLayout(c->dev, &plci, NULL, &c->q_pl_layout) != VK_SUCCESS) {
        VK_LOGF(c, "quad vkCreatePipelineLayout failed");
        c->q_pl_layout = VK_NULL_HANDLE;
        return false;
    }

    /* (e) the two shader modules. */
    VkShaderModuleCreateInfo vmci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(pvg_vk_simple_vertex_spv),
        .pCode = pvg_vk_simple_vertex_spv,
    };
    if (vkCreateShaderModule(c->dev, &vmci, NULL, &c->q_vs) != VK_SUCCESS) {
        VK_LOGF(c, "quad vkCreateShaderModule(vertex) failed");
        c->q_vs = VK_NULL_HANDLE;
        return false;
    }
    VkShaderModuleCreateInfo fmci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(pvg_vk_simple_texture_fragment_spv),
        .pCode = pvg_vk_simple_texture_fragment_spv,
    };
    if (vkCreateShaderModule(c->dev, &fmci, NULL, &c->q_fs) != VK_SUCCESS) {
        VK_LOGF(c, "quad vkCreateShaderModule(fragment) failed");
        c->q_fs = VK_NULL_HANDLE;
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = c->q_vs,
            .pName = PVG_VK_SIMPLE_VERTEX_ENTRY,           /* "SimpleVertex" */
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = c->q_fs,
            .pName = PVG_VK_SIMPLE_TEXTURE_FRAGMENT_ENTRY, /* "SimpleTextureFragment" */
        },
    };

    /* (f) vertex input: one binding (stride 16), two attributes matching
     * SimpleVertex — loc 0 pos vec2 @ off 0, loc 1 tex vec2 @ off 8. */
    VkVertexInputBindingDescription vib = {
        .binding = 0,
        .stride = sizeof(PGVkSimpleVertex),    /* 16 */
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };
    VkVertexInputAttributeDescription via[2] = {
        {
            .location = 0,
            .binding = 0,
            .format = VK_FORMAT_R32G32_SFLOAT,      /* vec2 pos */
            .offset = offsetof(PGVkSimpleVertex, pos),
        },
        {
            .location = 1,
            .binding = 0,
            .format = VK_FORMAT_R32G32_SFLOAT,      /* vec2 tex */
            .offset = offsetof(PGVkSimpleVertex, uv),
        },
    };
    VkPipelineVertexInputStateCreateInfo vis = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &vib,
        .vertexAttributeDescriptionCount = 2,
        .pVertexAttributeDescriptions = via,
    };

    VkPipelineInputAssemblyStateCreateInfo ias = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
        .primitiveRestartEnable = VK_FALSE,
    };

    /* Viewport/scissor are dynamic (set at draw to 1920x1080), but the state
     * still declares the counts. */
    VkPipelineViewportStateCreateInfo vps = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    VkDynamicState dyn_states[2] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    VkPipelineDynamicStateCreateInfo dyn = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dyn_states,
    };

    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,           /* draw both windings */
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState cba = {
        .blendEnable = VK_FALSE,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &cba,
    };

    /* Dynamic rendering: one B8G8R8A8_UNORM color attachment, no depth. */
    VkFormat color_fmt = VK_FORMAT_B8G8R8A8_UNORM;
    VkPipelineRenderingCreateInfo prci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_fmt,
    };

    VkGraphicsPipelineCreateInfo gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &prci,                  /* dynamic rendering (no VkRenderPass) */
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vis,
        .pInputAssemblyState = &ias,
        .pViewportState = &vps,
        .pRasterizationState = &rs,
        .pMultisampleState = &ms,
        .pDepthStencilState = NULL,      /* no depth */
        .pColorBlendState = &cb,
        .pDynamicState = &dyn,
        .layout = c->q_pl_layout,
        .renderPass = VK_NULL_HANDLE,    /* dynamic rendering */
        .subpass = 0,
    };
    if (vkCreateGraphicsPipelines(c->dev, VK_NULL_HANDLE, 1, &gpci, NULL,
                                  &c->q_pipeline) != VK_SUCCESS) {
        VK_LOGF(c, "quad vkCreateGraphicsPipelines failed");
        c->q_pipeline = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

/* Build (once) ALL the cached quad assets: texture, vertex buffer, pipeline +
 * the color-attachment view of the cached a3_img. */
static bool pg_vk_quad_ensure(PGVkContext *c, uint32_t W, uint32_t H)
{
    if (!pg_vk_ensure_a3_image(c, W, H)) {
        return false;
    }
    /* The render-target view tracks a3_img; (re)create if image changed. */
    if (c->a3_view == VK_NULL_HANDLE) {
        VkImageViewCreateInfo vci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = c->a3_img,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_B8G8R8A8_UNORM,
            .components = {
                VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
            },
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0, .levelCount = 1,
                .baseArrayLayer = 0, .layerCount = 1,
            },
        };
        if (vkCreateImageView(c->dev, &vci, NULL, &c->a3_view) != VK_SUCCESS) {
            VK_LOGF(c, "quad a3_img vkCreateImageView failed");
            c->a3_view = VK_NULL_HANDLE;
            return false;
        }
    }
    if (c->quad_ready) {
        return true;
    }
    if (!pg_vk_quad_make_texture(c))   { goto fail; }
    if (!pg_vk_quad_make_vertices(c))  { goto fail; }
    if (!pg_vk_quad_make_pipeline(c))  { goto fail; }   /* needs q_tex_view */
    c->quad_ready = true;
    return true;

fail:
    pg_vk_quad_destroy(c);
    return false;
}

/* ----------------------------------------------------------------------------
 * pg_vk_present_textured — the quad present: render the textured fullscreen quad
 * (2 real shaders) into the cached a3_img, copy out, hand to present_bgra8.
 * Mirrors the clear present's submit+wait+present pattern; differs in the body
 * (dynamic-rendering draw instead of vkCmdClearColorImage).
 * --------------------------------------------------------------------------- */
static bool pg_vk_present_textured(PGVkContext *c)
{
    const uint32_t W = 1920, H = 1080;
    const VkDeviceSize stride = (VkDeviceSize)W * 4;
    const size_t need = (size_t)W * H * 4;

    VK_LOGF(c, "quad present: %ux%u textured-quad render (2-shader pipeline)", W, H);

    if (!c->host || !c->host->present_bgra8) {
        VK_LOGF(c, "quad present: no host->present_bgra8 sink; aborting");
        return false;
    }

    qemu_mutex_lock(&c->submit_lock);

    bool ok = false;
    VkCommandBuffer cb = VK_NULL_HANDLE;

    /* Build/cache the texture, vertex buffer, pipeline + a3 view + a3 image. */
    if (!pg_vk_quad_ensure(c, W, H)) {
        VK_LOGF(c, "quad present: asset build failed");
        goto out;
    }

    /* Ensure the readback buffer is large enough (defensive; init sizes it). */
    if (c->readback_cap < need || c->readback_buf == VK_NULL_HANDLE) {
        if (c->readback_map) { vkUnmapMemory(c->dev, c->readback_mem); c->readback_map = NULL; }
        if (c->readback_buf) { vkDestroyBuffer(c->dev, c->readback_buf, NULL); c->readback_buf = VK_NULL_HANDLE; }
        if (c->readback_mem) { vkFreeMemory(c->dev, c->readback_mem, NULL); c->readback_mem = VK_NULL_HANDLE; }
        if (!pg_vk_alloc_readback(c, need)) {
            VK_LOGF(c, "quad present: readback (re)alloc failed");
            goto out;
        }
    }

    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = c->cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (vkAllocateCommandBuffers(c->dev, &cbai, &cb) != VK_SUCCESS) {
        VK_LOGF(c, "quad present: vkAllocateCommandBuffers failed");
        cb = VK_NULL_HANDLE;
        goto out;
    }
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) {
        VK_LOGF(c, "quad present: vkBeginCommandBuffer failed");
        goto out;
    }

    VkImageSubresourceRange full = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0, .levelCount = 1,
        .baseArrayLayer = 0, .layerCount = 1,
    };

    /* (1) a3_img: UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL (loadOp=CLEAR discards
     * prior contents, so UNDEFINED old layout is correct). */
    VkImageMemoryBarrier to_color = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = c->a3_img,
        .subresourceRange = full,
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                         0, NULL, 0, NULL, 1, &to_color);

    /* (2) dynamic rendering: clear to a dark background, then draw the quad. */
    VkRenderingAttachmentInfo color_att = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = c->a3_view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = { .color = { .float32 = { 0.02f, 0.02f, 0.06f, 1.0f } } },
    };
    VkRenderingInfo ri = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = { { 0, 0 }, { W, H } },
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_att,
    };
    vkCmdBeginRendering(cb, &ri);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, c->q_pipeline);

    VkDeviceSize voff = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &c->q_vtx_buf, &voff);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, c->q_pl_layout,
                            0, 1, &c->q_dset, 0, NULL);

    /* mvp = identity (column-major mat4, 64 bytes), VERTEX push constant. */
    static const float mvp_identity[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    vkCmdPushConstants(cb, c->q_pl_layout, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(mvp_identity), mvp_identity);

    VkViewport vp = {
        .x = 0.0f, .y = 0.0f,
        .width = (float)W, .height = (float)H,
        .minDepth = 0.0f, .maxDepth = 1.0f,
    };
    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D sc = { { 0, 0 }, { W, H } };
    vkCmdSetScissor(cb, 0, 1, &sc);

    vkCmdDraw(cb, c->q_vtx_count, 1, 0, 0);   /* 4-vertex fullscreen quad strip */

    vkCmdEndRendering(cb);

    /* (3) COLOR_ATTACHMENT_OPTIMAL -> TRANSFER_SRC for the copy-out. */
    VkImageMemoryBarrier to_src = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = c->a3_img,
        .subresourceRange = full,
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 1, &to_src);

    /* (4) copy the rendered image into the HOST_VISIBLE readback buffer. */
    VkBufferImageCopy copy = {
        .bufferOffset = 0,
        .bufferRowLength = 0,        /* tightly packed: stride == W*4 */
        .bufferImageHeight = 0,
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1,
        },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = { W, H, 1 },
    };
    vkCmdCopyImageToBuffer(cb, c->a3_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           c->readback_buf, 1, &copy);

    if (vkEndCommandBuffer(cb) != VK_SUCCESS) {
        VK_LOGF(c, "quad present: vkEndCommandBuffer failed");
        goto out;
    }

    /* (5) submit + wait idle (one-shot, same as the clear path). */
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
    };
    VkResult r = vkQueueSubmit(c->queue, 1, &si, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) {
        VK_LOGF(c, "quad present: vkQueueSubmit failed: %d", (int)r);
        goto out;
    }
    vkQueueWaitIdle(c->queue);

    /* (6) deliver the rendered pixels to the host present sink. */
    c->host->present_bgra8(c->host_opaque, W, H, (uint32_t)stride,
                           c->readback_map);
    ok = true;
    VK_LOGF(c, "quad present OK: presented %ux%u textured-quad frame via "
               "present_bgra8 (src tex %ux%u)", W, H, c->q_tex_w, c->q_tex_h);

out:
    if (cb != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(c->dev, c->cmd_pool, 1, &cb);
    }
    qemu_mutex_unlock(&c->submit_lock);
    if (!ok) {
        VK_LOGF(c, "quad present FAILED");
    }
    return ok;
}

/* ============================================================================
 * GUEST PRESENT — render the guest's real composited surface
 * through the fullscreen-quad pipeline.
 *
 * Identical pixel path to pg_vk_present_textured (same q_pipeline / q_pl_layout /
 * q_sampler / q_vtx_buf / a3_img target / readback / present_bgra8), EXCEPT the
 * sampled source is the guest's FBSCAN'd framebuffer uploaded each frame into a
 * dedicated, dynamically-sized source image (g_tex_*) rather than the procedural
 * 256x256 q_tex_*. The cached quad descriptor set (c->q_dset) binding 0 is re-pointed
 * at g_tex_view before the draw (vkUpdateDescriptorSets); this is safe because the
 * queue is idle from the previous present (we vkQueueWaitIdle at the end of every
 * present and serialize on submit_lock).
 * ============================================================================ */

/* Destroy the cached guest source-texture + staging objects (idempotent). */
static void pg_vk_guest_tex_destroy(PGVkContext *c)
{
    if (!c->dev) {
        return;
    }
    if (c->g_stage_map) { vkUnmapMemory(c->dev, c->g_stage_mem); c->g_stage_map = NULL; }
    if (c->g_stage_buf) { vkDestroyBuffer(c->dev, c->g_stage_buf, NULL); c->g_stage_buf = VK_NULL_HANDLE; }
    if (c->g_stage_mem) { vkFreeMemory(c->dev, c->g_stage_mem, NULL); c->g_stage_mem = VK_NULL_HANDLE; }
    c->g_stage_cap = 0;
    if (c->g_tex_view) { vkDestroyImageView(c->dev, c->g_tex_view, NULL); c->g_tex_view = VK_NULL_HANDLE; }
    if (c->g_tex_img)  { vkDestroyImage(c->dev, c->g_tex_img, NULL); c->g_tex_img = VK_NULL_HANDLE; }
    if (c->g_tex_mem)  { vkFreeMemory(c->dev, c->g_tex_mem, NULL); c->g_tex_mem = VK_NULL_HANDLE; }
    c->g_tex_w = c->g_tex_h = 0;
}

/* Ensure a SAMPLED+TRANSFER_DST guest source VkImage of exactly WxH (BGRA8) and a
 * HOST_VISIBLE staging buffer of at least `stage_need` bytes. Recreates the image
 * on geometry change; grows the staging buffer on demand. Mirrors
 * pg_vk_quad_make_texture's image creation but with caller-driven dimensions and
 * NO procedural fill (the upload happens per-frame in pg_vk_present_guest). */
static bool pg_vk_guest_tex_ensure(PGVkContext *c, uint32_t w, uint32_t h,
                                   size_t stage_need)
{
    /* (a) the sampled device-local image — recreate on geometry change. */
    if (c->g_tex_img == VK_NULL_HANDLE || c->g_tex_w != w || c->g_tex_h != h) {
        if (c->g_tex_view) { vkDestroyImageView(c->dev, c->g_tex_view, NULL); c->g_tex_view = VK_NULL_HANDLE; }
        if (c->g_tex_img)  { vkDestroyImage(c->dev, c->g_tex_img, NULL); c->g_tex_img = VK_NULL_HANDLE; }
        if (c->g_tex_mem)  { vkFreeMemory(c->dev, c->g_tex_mem, NULL); c->g_tex_mem = VK_NULL_HANDLE; }

        VkImageCreateInfo ici = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_B8G8R8A8_UNORM,
            .extent = { w, h, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        if (vkCreateImage(c->dev, &ici, NULL, &c->g_tex_img) != VK_SUCCESS) {
            VK_LOGF(c, "guest tex vkCreateImage(%ux%u) failed", w, h);
            c->g_tex_img = VK_NULL_HANDLE;
            return false;
        }
        VkMemoryRequirements imr;
        vkGetImageMemoryRequirements(c->dev, c->g_tex_img, &imr);
        uint32_t imt = pg_vk_mem_type(c, imr.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (imt == UINT32_MAX) { imt = pg_vk_mem_type(c, imr.memoryTypeBits, 0); }
        if (imt == UINT32_MAX) {
            VK_LOGF(c, "guest tex no image memory type");
            vkDestroyImage(c->dev, c->g_tex_img, NULL);
            c->g_tex_img = VK_NULL_HANDLE;
            return false;
        }
        VkMemoryAllocateInfo imai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = imr.size,
            .memoryTypeIndex = imt,
        };
        if (vkAllocateMemory(c->dev, &imai, NULL, &c->g_tex_mem) != VK_SUCCESS ||
            vkBindImageMemory(c->dev, c->g_tex_img, c->g_tex_mem, 0) != VK_SUCCESS) {
            VK_LOGF(c, "guest tex image alloc/bind failed");
            if (c->g_tex_mem) { vkFreeMemory(c->dev, c->g_tex_mem, NULL); c->g_tex_mem = VK_NULL_HANDLE; }
            vkDestroyImage(c->dev, c->g_tex_img, NULL);
            c->g_tex_img = VK_NULL_HANDLE;
            return false;
        }

        VkImageViewCreateInfo vci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = c->g_tex_img,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_B8G8R8A8_UNORM,
            .components = {
                VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
            },
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0, .levelCount = 1,
                .baseArrayLayer = 0, .layerCount = 1,
            },
        };
        if (vkCreateImageView(c->dev, &vci, NULL, &c->g_tex_view) != VK_SUCCESS) {
            VK_LOGF(c, "guest tex vkCreateImageView failed");
            c->g_tex_view = VK_NULL_HANDLE;
            vkFreeMemory(c->dev, c->g_tex_mem, NULL); c->g_tex_mem = VK_NULL_HANDLE;
            vkDestroyImage(c->dev, c->g_tex_img, NULL); c->g_tex_img = VK_NULL_HANDLE;
            return false;
        }
        c->g_tex_w = w;
        c->g_tex_h = h;
    }

    /* (b) the HOST_VISIBLE upload staging buffer — grow on demand. */
    if (c->g_stage_buf == VK_NULL_HANDLE || c->g_stage_cap < stage_need) {
        if (c->g_stage_map) { vkUnmapMemory(c->dev, c->g_stage_mem); c->g_stage_map = NULL; }
        if (c->g_stage_buf) { vkDestroyBuffer(c->dev, c->g_stage_buf, NULL); c->g_stage_buf = VK_NULL_HANDLE; }
        if (c->g_stage_mem) { vkFreeMemory(c->dev, c->g_stage_mem, NULL); c->g_stage_mem = VK_NULL_HANDLE; }
        c->g_stage_cap = 0;

        VkBufferCreateInfo sbci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = stage_need,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        if (vkCreateBuffer(c->dev, &sbci, NULL, &c->g_stage_buf) != VK_SUCCESS) {
            VK_LOGF(c, "guest staging vkCreateBuffer(%zuB) failed", stage_need);
            c->g_stage_buf = VK_NULL_HANDLE;
            return false;
        }
        VkMemoryRequirements smr;
        vkGetBufferMemoryRequirements(c->dev, c->g_stage_buf, &smr);
        uint32_t smt = pg_vk_mem_type(c, smr.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (smt == UINT32_MAX) {
            VK_LOGF(c, "guest staging no HOST_VISIBLE memory type");
            vkDestroyBuffer(c->dev, c->g_stage_buf, NULL);
            c->g_stage_buf = VK_NULL_HANDLE;
            return false;
        }
        VkMemoryAllocateInfo smai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = smr.size,
            .memoryTypeIndex = smt,
        };
        if (vkAllocateMemory(c->dev, &smai, NULL, &c->g_stage_mem) != VK_SUCCESS ||
            vkBindBufferMemory(c->dev, c->g_stage_buf, c->g_stage_mem, 0) != VK_SUCCESS) {
            VK_LOGF(c, "guest staging alloc/bind failed");
            if (c->g_stage_mem) { vkFreeMemory(c->dev, c->g_stage_mem, NULL); c->g_stage_mem = VK_NULL_HANDLE; }
            vkDestroyBuffer(c->dev, c->g_stage_buf, NULL); c->g_stage_buf = VK_NULL_HANDLE;
            return false;
        }
        if (vkMapMemory(c->dev, c->g_stage_mem, 0, VK_WHOLE_SIZE, 0,
                        &c->g_stage_map) != VK_SUCCESS) {
            VK_LOGF(c, "guest staging vkMapMemory failed");
            c->g_stage_map = NULL;
            return false;
        }
        c->g_stage_cap = stage_need;
    }
    return true;
}

/* ----------------------------------------------------------------------------
 * pg_vk_present_guest — upload the guest's FBSCAN'd BGRA8 pixels into the guest
 * source image and render them through the fullscreen-quad pipeline.
 * --------------------------------------------------------------------------- */
static bool pg_vk_present_guest(PGVkContext *c, uint32_t width, uint32_t height,
                                uint32_t stride, const void *pixels)
{
    const uint32_t W = 1920, H = 1080;          /* fixed quad present target       */
    const VkDeviceSize out_stride = (VkDeviceSize)W * 4;
    const size_t out_need = (size_t)W * H * 4;

    VK_LOGF(c, "guest present: upload guest %ux%u stride=%u -> %ux%u quad",
            width, height, stride, W, H);

    if (!c->host || !c->host->present_bgra8) {
        VK_LOGF(c, "guest present: no host->present_bgra8 sink; aborting");
        return false;
    }
    if (!pixels || !width || !height || stride < (uint64_t)width * 4) {
        VK_LOGF(c, "guest present: bad guest params (px=%p %ux%u stride=%u)",
                pixels, width, height, stride);
        return false;
    }

    qemu_mutex_lock(&c->submit_lock);

    bool ok = false;
    VkCommandBuffer cb = VK_NULL_HANDLE;

    /* Build/cache the quad pipeline/sampler/quad + a3 view + a3 image (reused). */
    if (!pg_vk_quad_ensure(c, W, H)) {
        VK_LOGF(c, "guest present: quad asset build failed");
        goto out;
    }

    /* Build/cache the guest source image (WxH) + a staging buffer holding the
     * upload. The staging buffer is filled with `stride` bytes/row of the guest
     * surface; vkCmdCopyBufferToImage uses bufferRowLength=stride/4 so a stride
     * that exceeds width*4 is honored without repacking. */
    const size_t stage_need = (size_t)stride * height;
    if (!pg_vk_guest_tex_ensure(c, width, height, stage_need)) {
        VK_LOGF(c, "guest present: guest source build failed");
        goto out;
    }

    /* Copy the guest pixels into the mapped HOST_VISIBLE staging buffer. The
     * source `pixels` host pointer was resolved by the native FBSCAN path. */
    memcpy(c->g_stage_map, pixels, stage_need);

    /* Re-point the cached quad descriptor set's binding 0 at the GUEST texture
     * (safe: queue idle from the previous present, serialized on submit_lock). */
    VkDescriptorImageInfo gdii = {
        .sampler = c->q_sampler,
        .imageView = c->g_tex_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet gwds = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = c->q_dset,
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &gdii,
    };
    vkUpdateDescriptorSets(c->dev, 1, &gwds, 0, NULL);

    /* Ensure the readback buffer is large enough (defensive; init sizes it). */
    if (c->readback_cap < out_need || c->readback_buf == VK_NULL_HANDLE) {
        if (c->readback_map) { vkUnmapMemory(c->dev, c->readback_mem); c->readback_map = NULL; }
        if (c->readback_buf) { vkDestroyBuffer(c->dev, c->readback_buf, NULL); c->readback_buf = VK_NULL_HANDLE; }
        if (c->readback_mem) { vkFreeMemory(c->dev, c->readback_mem, NULL); c->readback_mem = VK_NULL_HANDLE; }
        if (!pg_vk_alloc_readback(c, out_need)) {
            VK_LOGF(c, "guest present: readback (re)alloc failed");
            goto out;
        }
    }

    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = c->cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (vkAllocateCommandBuffers(c->dev, &cbai, &cb) != VK_SUCCESS) {
        VK_LOGF(c, "guest present: vkAllocateCommandBuffers failed");
        cb = VK_NULL_HANDLE;
        goto out;
    }
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) {
        VK_LOGF(c, "guest present: vkBeginCommandBuffer failed");
        goto out;
    }

    VkImageSubresourceRange full = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0, .levelCount = 1,
        .baseArrayLayer = 0, .layerCount = 1,
    };

    /* (A) guest source: UNDEFINED -> TRANSFER_DST, copy the staged pixels in,
     * then -> SHADER_READ_ONLY_OPTIMAL for sampling. The copy honors a row stride
     * wider than width*4 via bufferRowLength (px units). */
    VkImageMemoryBarrier g_to_dst = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,    /* discard prior frame */
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = c->g_tex_img,
        .subresourceRange = full,
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 1, &g_to_dst);

    VkBufferImageCopy gbic = {
        .bufferOffset = 0,
        .bufferRowLength = stride / 4,   /* px/row from byte stride (BGRA8) */
        .bufferImageHeight = 0,
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1,
        },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = { width, height, 1 },
    };
    vkCmdCopyBufferToImage(cb, c->g_stage_buf, c->g_tex_img,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &gbic);

    VkImageMemoryBarrier g_to_read = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = c->g_tex_img,
        .subresourceRange = full,
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                         0, NULL, 0, NULL, 1, &g_to_read);

    /* (B) a3_img: UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL (loadOp=CLEAR). */
    VkImageMemoryBarrier to_color = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = c->a3_img,
        .subresourceRange = full,
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                         0, NULL, 0, NULL, 1, &to_color);

    /* (C) dynamic rendering: clear, then draw the SAME quad sampling g_tex. */
    VkRenderingAttachmentInfo color_att = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = c->a3_view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = { .color = { .float32 = { 0.02f, 0.02f, 0.06f, 1.0f } } },
    };
    VkRenderingInfo ri = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = { { 0, 0 }, { W, H } },
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_att,
    };
    vkCmdBeginRendering(cb, &ri);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, c->q_pipeline);

    VkDeviceSize voff = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &c->q_vtx_buf, &voff);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, c->q_pl_layout,
                            0, 1, &c->q_dset, 0, NULL);   /* now points at g_tex */

    static const float mvp_identity[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    vkCmdPushConstants(cb, c->q_pl_layout, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(mvp_identity), mvp_identity);

    VkViewport vp = {
        .x = 0.0f, .y = 0.0f,
        .width = (float)W, .height = (float)H,
        .minDepth = 0.0f, .maxDepth = 1.0f,
    };
    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D sc = { { 0, 0 }, { W, H } };
    vkCmdSetScissor(cb, 0, 1, &sc);

    vkCmdDraw(cb, c->q_vtx_count, 1, 0, 0);   /* 4-vertex fullscreen quad strip */

    vkCmdEndRendering(cb);

    /* (D) a3_img COLOR_ATTACHMENT -> TRANSFER_SRC for the copy-out. */
    VkImageMemoryBarrier to_src = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = c->a3_img,
        .subresourceRange = full,
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 1, &to_src);

    /* (E) copy the rendered image into the HOST_VISIBLE readback buffer. */
    VkBufferImageCopy copy = {
        .bufferOffset = 0,
        .bufferRowLength = 0,        /* tightly packed: stride == W*4 */
        .bufferImageHeight = 0,
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1,
        },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = { W, H, 1 },
    };
    vkCmdCopyImageToBuffer(cb, c->a3_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           c->readback_buf, 1, &copy);

    if (vkEndCommandBuffer(cb) != VK_SUCCESS) {
        VK_LOGF(c, "guest present: vkEndCommandBuffer failed");
        goto out;
    }

    /* (F) submit + wait idle (one-shot, same pattern as quad/clear). */
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
    };
    VkResult r = vkQueueSubmit(c->queue, 1, &si, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) {
        VK_LOGF(c, "guest present: vkQueueSubmit failed: %d", (int)r);
        goto out;
    }
    vkQueueWaitIdle(c->queue);

    /* (G) deliver the rendered pixels to the host present sink. */
    c->host->present_bgra8(c->host_opaque, W, H, (uint32_t)out_stride,
                           c->readback_map);
    ok = true;
    VK_LOGF(c, "guest present OK: presented guest %ux%u (stride=%u) through "
               "quad -> %ux%u via present_bgra8", width, height, stride, W, H);

out:
    if (cb != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(c->dev, c->cmd_pool, 1, &cb);
    }
    qemu_mutex_unlock(&c->submit_lock);
    if (!ok) {
        VK_LOGF(c, "guest present FAILED");
    }
    return ok;
}

/* ----------------------------------------------------------------------------
 * present — clear/test render.
 *
 * Self-contained, lavapipe-only bring-up present: clear a cached 1920x1080
 * BGRA8 image to a recognizable solid teal, copy it to the HOST_VISIBLE readback
 * buffer, and deliver it via the host present_bgra8 hook the context saved at
 * init() time. Submit + vkQueueWaitIdle (simple + correct for a one-shot test).
 *
 * Proves the lavapipe device, queue, command recording, image-to-buffer copy and
 * the host present seam all work end-to-end (image reaches VNC) BEFORE the real
 * exec_indirect translator (stream walk + quad render) lands. Per-call command
 * buffer is freed; the image/buffer are cached in the context for reuse.
 * --------------------------------------------------------------------------- */
static bool pg_vk_present(PGVkContext *c, uint32_t surface_id)
{
    const uint32_t W = 1920, H = 1080;
    const VkDeviceSize stride = (VkDeviceSize)W * 4;
    const size_t need = (size_t)W * H * 4;

    /* Mode select: APPLE_GFX_VK_QUAD (set in init -> c->quad_mode) routes to the
     * quad textured-quad present (real pipeline + 2 shaders); else the clear. */
    if (c->quad_mode) {
        return pg_vk_present_textured(c);
    }

    VK_LOGF(c, "clear present surf=%u: %ux%u BGRA8 test render", surface_id, W, H);

    if (!c->host || !c->host->present_bgra8) {
        VK_LOGF(c, "clear present: no host->present_bgra8 sink; aborting");
        return false;
    }

    /* Serialize against the completion thread / exec submits sharing the pool. */
    qemu_mutex_lock(&c->submit_lock);

    bool ok = false;
    VkCommandBuffer cb = VK_NULL_HANDLE;

    if (!pg_vk_ensure_a3_image(c, W, H)) {
        goto out;
    }

    /* Ensure the readback buffer is large enough (init sizes it to 1920x1080x4;
     * this is a defensive grow path if it was ever smaller). */
    if (c->readback_cap < need || c->readback_buf == VK_NULL_HANDLE) {
        if (c->readback_map) { vkUnmapMemory(c->dev, c->readback_mem); c->readback_map = NULL; }
        if (c->readback_buf) { vkDestroyBuffer(c->dev, c->readback_buf, NULL); c->readback_buf = VK_NULL_HANDLE; }
        if (c->readback_mem) { vkFreeMemory(c->dev, c->readback_mem, NULL); c->readback_mem = VK_NULL_HANDLE; }
        if (!pg_vk_alloc_readback(c, need)) {
            VK_LOGF(c, "clear present: readback (re)alloc failed");
            goto out;
        }
    }

    /* Allocate a one-shot primary command buffer from the transient pool. */
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = c->cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (vkAllocateCommandBuffers(c->dev, &cbai, &cb) != VK_SUCCESS) {
        VK_LOGF(c, "clear present: vkAllocateCommandBuffers failed");
        cb = VK_NULL_HANDLE;
        goto out;
    }

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) {
        VK_LOGF(c, "clear present: vkBeginCommandBuffer failed");
        goto out;
    }

    VkImageSubresourceRange full = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0, .levelCount = 1,
        .baseArrayLayer = 0, .layerCount = 1,
    };

    /* (1) UNDEFINED -> TRANSFER_DST for the clear. */
    VkImageMemoryBarrier to_dst = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = c->a3_img,
        .subresourceRange = full,
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 1, &to_dst);

    /* (2) clear to a recognizable solid TEAL (NOT black/garbage). BGRA8 image,
     * but vkCmdClearColorImage takes RGBA float components mapped to the image's
     * format channels, so .float32 = {R,G,B,A}; teal = R0 G0.5 B0.5 A1. */
    VkClearColorValue teal = { .float32 = { 0.0f, 0.5f, 0.5f, 1.0f } };
    vkCmdClearColorImage(cb, c->a3_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &teal, 1, &full);

    /* (3) TRANSFER_DST -> TRANSFER_SRC for the copy-out. */
    VkImageMemoryBarrier to_src = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = c->a3_img,
        .subresourceRange = full,
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 1, &to_src);

    /* (4) copy the cleared image into the HOST_VISIBLE readback buffer. The
     * buffer is tightly packed BGRA8 (bufferRowLength=0 => stride == W*4). */
    VkBufferImageCopy copy = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1,
        },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = { W, H, 1 },
    };
    vkCmdCopyImageToBuffer(cb, c->a3_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           c->readback_buf, 1, &copy);

    if (vkEndCommandBuffer(cb) != VK_SUCCESS) {
        VK_LOGF(c, "clear present: vkEndCommandBuffer failed");
        goto out;
    }

    /* (5) submit and wait idle — simple + correct for a one-shot test. */
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
    };
    VkResult r = vkQueueSubmit(c->queue, 1, &si, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) {
        VK_LOGF(c, "clear present: vkQueueSubmit failed: %d", (int)r);
        goto out;
    }
    vkQueueWaitIdle(c->queue);

    /* (6) deliver the readback pixels to the host present sink. The readback
     * buffer is HOST_COHERENT + persistently mapped (c->readback_map). */
    c->host->present_bgra8(c->host_opaque, W, H, (uint32_t)stride,
                           c->readback_map);
    ok = true;
    VK_LOGF(c, "clear present OK: presented %ux%u teal test frame via present_bgra8",
            W, H);

out:
    if (cb != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(c->dev, c->cmd_pool, 1, &cb);
    }
    qemu_mutex_unlock(&c->submit_lock);
    if (!ok) {
        VK_LOGF(c, "clear present FAILED surf=%u", surface_id);
    }
    return ok;
}

static void pg_vk_shutdown(PGVkContext *c)
{
    if (!c) {
        return;
    }
    /* Stop the completion thread. */
    qemu_mutex_lock(&c->submit_lock);
    c->shutting_down = true;
    qemu_cond_signal(&c->pending_cond);
    qemu_mutex_unlock(&c->submit_lock);
    qemu_thread_join(&c->completion_thr);

    if (c->dev) {
        vkDeviceWaitIdle(c->dev);
    }
    /* quad textured-quad assets (pipeline/desc/sampler/tex/vtx/shaders) + the
     * a3_img color-attachment view, before a3_img/device teardown below. */
    pg_vk_quad_destroy(c);
    /* guest source image + staging buffer (drawn through the quad pipeline). */
    pg_vk_guest_tex_destroy(c);
    if (c->a3_view)      { vkDestroyImageView(c->dev, c->a3_view, NULL); }
    if (c->a3_img)       { vkDestroyImage(c->dev, c->a3_img, NULL); }
    if (c->a3_mem)       { vkFreeMemory(c->dev, c->a3_mem, NULL); }
    if (c->timeline)     { vkDestroySemaphore(c->dev, c->timeline, NULL); }
    if (c->readback_map) { vkUnmapMemory(c->dev, c->readback_mem); }
    if (c->readback_buf) { vkDestroyBuffer(c->dev, c->readback_buf, NULL); }
    if (c->readback_mem) { vkFreeMemory(c->dev, c->readback_mem, NULL); }
    if (c->cmd_pool)     { vkDestroyCommandPool(c->dev, c->cmd_pool, NULL); }
    if (c->dev)          { vkDestroyDevice(c->dev, NULL); }
    if (c->inst)         { vkDestroyInstance(c->inst, NULL); }

    qemu_cond_destroy(&c->pending_cond);
    qemu_mutex_destroy(&c->submit_lock);
    g_free(c);
}

static const PGVkOps pg_vk_ops_impl = {
    .init          = pg_vk_init,
    .exec_indirect = pg_vk_exec_indirect,
    .present       = pg_vk_present,
    .present_guest = pg_vk_present_guest,
    .shutdown      = pg_vk_shutdown,
};

const PGVkOps *pg_vk_ops(void)
{
    return &pg_vk_ops_impl;
}

#else  /* !CONFIG_PVG_VULKAN */

#include "apple-gfx-vk.h"

/*
 * Vulkan not compiled in: the vtable is NULL, so apple-gfx-native.c keeps its
 * immediate pg_signal_stamp() stub. This single function is the entire TU when
 * the macro is off, so the native backend links identically with or without
 * Vulkan present on the build host.
 */
const PGVkOps *pg_vk_ops(void)
{
    return NULL;
}

#endif /* CONFIG_PVG_VULKAN */
