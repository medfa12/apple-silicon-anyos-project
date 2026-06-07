/*
 * apple-gfx-vk.h — PVG -> host Vulkan translator vtable (SOFTWARE path = lavapipe).
 *
 * The accelerated PVG stream (ExecIndirect2/3) needs a GPU to execute the
 * Metal-derived command buffers. On a GPU-less non-Apple ARM64 Linux host
 * (software Vulkan, no GPU) the only way to run that work is a software
 * rasterizer — Mesa lavapipe (the LLVMpipe Vulkan ICD). This header is the seam
 * apple-gfx-native.c reaches it through, WITHOUT depending on any Vulkan header:
 * it sees only the opaque PGVkContext and the PGVkOps vtable.
 *
 *   pg_vk_ops()  -> &impl   when built with CONFIG_PVG_VULKAN (Vulkan loader found)
 *                -> NULL     otherwise (build with no Vulkan, or stub fallback)
 *
 * NULL is the load-bearing contract: op_exec_indirect, on a NULL vtable, keeps its
 * current immediate pg_signal_stamp() stub (existing behaviour preserved). Only
 * when pg_vk_ops() is non-NULL does it call exec_indirect() and let software-GPU
 * completion drive the stamp.
 *
 * +A2 of docs/metal-vulkan-translator.md; per docs/metal-vulkan-translator.md §0/§1.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_DISPLAY_APPLE_GFX_VK_H
#define HW_DISPLAY_APPLE_GFX_VK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "apple-gfx-native.h"   /* PGNativeHostOps, PGNativeDevice (opaque here) */

/* ----------------------------------------------------------------------------
 * Opaque host-Vulkan context. The real definition (VkInstance/VkPhysicalDevice/
 * VkDevice/VkQueue/timeline VkSemaphore/readback VkBuffer/completion thread)
 * lives in apple-gfx-vk.c under #ifdef CONFIG_PVG_VULKAN, so this header never
 * pulls in <vulkan/vulkan.h> and apple-gfx-native.c stays Vulkan-agnostic.
 * --------------------------------------------------------------------------- */
typedef struct PGVkContext PGVkContext;

/* ----------------------------------------------------------------------------
 * The vtable apple-gfx-native.c calls through. Pointer is NULL when Vulkan was
 * not compiled in (CONFIG_PVG_VULKAN off) OR the lavapipe context failed to come
 * up — either way the native backend falls back to its immediate stamp-ack stub.
 * --------------------------------------------------------------------------- */
typedef struct PGVkOps {
    /*
     * Stand up the lavapipe host context. `host`/`host_opaque` are the SAME
     * PGNativeHostOps the device already owns, so the translator can resolve
     * GPA->hostVA (host_ptr/read_mem) and present (present_bgra8). `dev` is the
     * owning PGNativeDevice, opaque to this TU except as the argument passed
     * back to pg_signal_stamp on completion (via a back-reference the native
     * side wires, or a callback — see exec_indirect contract below).
     *
     * Returns a context, or NULL if no software (or any) Vulkan device exists.
     * On NULL the caller MUST keep its existing immediate-stamp behaviour.
     */
    PGVkContext *(*init)(PGNativeDevice *dev,
                         const PGNativeHostOps *host, void *host_opaque);

    /*
     * The ExecIndirect entry point: hand the inner Metal command stream to the
     * software-GPU translator. Records a Vulkan command buffer (render the
     * present quad / blit / etc.), submits with a timeline-semaphore signal, and
     * enqueues a pending-completion record. Returns the timeline value the
     * submission will signal at (monotonic, per channel), or 0 if nothing was
     * submitted (in which case the caller should fall back to its immediate
     * pg_signal_stamp).
     *
     * `stamp_value` is hdr->stampValue: the completion thread signals it (via the
     * BQL-safe bottom-half) once the submission's timeline value is reached, so
     * the guest fence wait is satisfied by REAL (software) GPU completion rather
     * than synchronously in the FIFO drain.
     *
     * `payload`/`payload_len` is the ExecIndirect payload (header + residency
     * ENTRY[] + trailing innerStreamGPA/innerStreamLen); the translator parses
     * the inner stream itself (the op-0x37 render-subset walker).
     */
    uint64_t (*exec_indirect)(PGVkContext *ctx, uint32_t task_id,
                              const uint8_t *payload, uint32_t payload_len,
                              uint32_t stamp_value);

    /*
     * Explicit present of a resolved surface through the software-GPU path:
     * stage/upload the surface, (optionally) run the composite, read it back to a
     * HOST_VISIBLE buffer and deliver via present_bgra8 (and/or mirror to the
     * iosfc plane[0] GPA). Returns true if it presented. Used by the bring-up
     * (test-image present over VNC) and by the composite present path. May be
     * NULL/stubbed in the first cut — present is normally appended to the
     * exec_indirect command buffer and driven by completion.
     */
    bool (*present)(PGVkContext *ctx, uint32_t surface_id);

    /*
     * Present the GUEST's real composited pixels through the software-Vulkan
     * pipeline (guest present). `pixels` is the host pointer to the guest's
     * FBSCAN'd BGRA8 framebuffer: `width`x`height` px, `stride` bytes/row (which
     * MAY exceed width*4). The implementation uploads those pixels into a sampled
     * source VkImage (sized to the guest surface), then renders the SAME
     * fullscreen-quad pipeline as the synthetic-texture present — sampling THIS
     * guest texture instead — into the cached 1920x1080 target, reads it back and
     * delivers it via present_bgra8. Returns true if it presented.
     *
     * apple-gfx-native.c's vblank FBSCAN path calls this (gated on
     * APPLE_GFX_VK_GUEST) once it has located the guest framebuffer GPA + geometry
     * and resolved a host pointer; on success it skips the direct FBSCAN
     * present_bgra8 for that frame. NULL/absent => the native side keeps the direct
     * FBSCAN present. May be NULL when Vulkan is not compiled in.
     */
    bool (*present_guest)(PGVkContext *ctx, uint32_t width, uint32_t height,
                          uint32_t stride, const void *pixels);

    /* Tear down the context (join the completion thread, free Vulkan objects). */
    void (*shutdown)(PGVkContext *ctx);
} PGVkOps;

/*
 * Returns the software-Vulkan implementation when built with CONFIG_PVG_VULKAN,
 * else NULL. apple-gfx-native.c uses it like:
 *
 *     const PGVkOps *vk = pg_vk_ops();
 *     if (vk && d->vk_ctx) {
 *         uint64_t v = vk->exec_indirect(d->vk_ctx, task_id, payload, len, stamp);
 *         if (v) return;                 // completion will drive the stamp
 *     }
 *     pg_signal_stamp(d, hdr->stampValue);   // NULL-safe fallback (unchanged)
 *
 * This symbol is ALWAYS defined (both halves of the #ifdef compile a body), so
 * the native backend links the same way whether or not Vulkan is present.
 */
const PGVkOps *pg_vk_ops(void);

#endif /* HW_DISPLAY_APPLE_GFX_VK_H */
