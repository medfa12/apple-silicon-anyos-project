/*
 * apple-gfx-native.c — standalone, framework-FREE PVG (apple-gfx) device backend.
 *
 * Reimplements the *consumer* (host) half of Apple's
 * ParavirtualizedGraphics.framework protocol with NO PG* / Metal / IOSurface
 * symbols. See apple-gfx-native.h and docs/metal-vulkan-translator.md §0 for the contract.
 *
 * What this file implements (docs/metal-vulkan-translator.md §0.7 steps 1-6):
 *   1. gfx-BAR MMIO read/write decode (the §0.1 register map).
 *   2. createTask(16MB) on RootPage(0x101c); FIFO geometry on
 *      0x1030/0x1004/0x1010; ring arm on 0x1000.
 *   3. FIFO ring drain on the 0x1008 doorbell (and pollable), 12-byte header
 *      parse + opcode dispatch (§0.3).
 *   4. CmdDefineTask2 (0x38) + CmdMapMemory2 (0x39): record guest-phys ranges
 *      into task VAs.
 *   5. CmdDisplayTransaction3 (0x07): resolve the BGRA8 surface in guest RAM and
 *      present it via the host present_bgra8 hook.
 *   6. signalStamp -> set the 0x1018 IRQ-cause bit + raise the IRQ.
 *   7. ExecIndirect2/3 (0x37/0x43): logged stub (Metal stream = accel-only TODO).
 *
 * Plain host-portable C: no Apple framework, no QEMU header dependency. The QEMU
 * glue and the offline replay harness both link against this object.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "apple-gfx-native.h"
#include "apple-gfx-vk.h"   /* pg_vk_ops()/PGVkOps — ALWAYS linkable (returns NULL
                            * when CONFIG_PVG_VULKAN is off), so no #if needed for
                            * the native-side calls below. */

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>   /* getenv (APPLE_GFX_NO_METAL force-SW toggle) */

/* ------------------------------------------------------------------ logging */

static void pg_log(PGNativeDevice *d, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (d->ops && d->ops->log) {
        d->ops->log(d->ops->opaque, buf);
    } else {
        fprintf(stderr, "[pvg-native] %s\n", buf);
    }
}

/* ----------------------------------------------------- little-endian readers
 *
 * The wire format is little-endian and NOT naturally aligned (the 12-byte
 * header is followed by packed payloads with u64 fields at odd offsets, e.g.
 * DefineTask2.taskRoot @4). Read byte-wise to stay portable + alignment-safe.
 */
static inline uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static inline uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t rd_u64(const uint8_t *p) {
    return (uint64_t)rd_u32(p) | ((uint64_t)rd_u32(p + 4) << 32);
}
static inline void wr_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v);       p[1] = (uint8_t)(v >> 8);
}
static inline void wr_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v);       p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* ----------------------------------------------------------------- lifecycle */

void pg_native_init(PGNativeDevice *d, const PGNativeHostOps *ops)
{
    memset(d, 0, sizeof(*d));
    d->ops = ops;
    /* 0x122c reads 1 at rest — the cross-validated version/EFI-mode fact. */

    /* Stand up the software-GPU (lavapipe) translator context if Vulkan was
     * compiled in (pg_vk_ops() is NULL otherwise). NULL-safe: a failed bring-up
     * leaves d->vk_ctx NULL and the backend keeps its immediate-stamp stub. */
    if (pg_vk_ops()) {
        d->vk_ctx = pg_vk_ops()->init(d, d->ops, d->ops->opaque);
    }
}

void pg_native_reset(PGNativeDevice *d)
{
    const PGNativeHostOps *ops = d->ops;
    /* Tear down the software-Vulkan context (join completion thread, free Vulkan
     * objects) before zeroing the device. NULL-safe. */
    if (pg_vk_ops() && d->vk_ctx) {
        pg_vk_ops()->shutdown(d->vk_ctx);
        d->vk_ctx = NULL;
    }
    /* Release heap owned by the device before zeroing (gather buffers). */
    for (uint32_t i = 0; i < PVG_MAX_SURFACES; i++) {
        free(d->surfaces[i].page_gpas);
    }
    free(d->present_scratch);
    memset(d, 0, sizeof(*d));
    d->ops = ops;
}

/* ------------------------------------------------------------- task helpers */

static PGTask *pg_task_alloc(PGNativeDevice *d, uint32_t task_id,
                             uint64_t vm_size, uint64_t task_root)
{
    /* Reuse an existing slot for this task_id if present. */
    for (uint32_t i = 0; i < PVG_MAX_TASKS; i++) {
        if (d->tasks[i].in_use && d->tasks[i].task_id == task_id) {
            return &d->tasks[i];
        }
    }
    for (uint32_t i = 0; i < PVG_MAX_TASKS; i++) {
        if (!d->tasks[i].in_use) {
            PGTask *t = &d->tasks[i];
            memset(t, 0, sizeof(*t));
            t->in_use = true;
            t->task_id = task_id;
            t->vm_size = vm_size;
            t->task_root = task_root;
            d->task_count++;
            return t;
        }
    }
    return NULL;
}

PGTask *pg_native_find_task(PGNativeDevice *d, uint32_t task_id)
{
    for (uint32_t i = 0; i < PVG_MAX_TASKS; i++) {
        if (d->tasks[i].in_use && d->tasks[i].task_id == task_id) {
            return &d->tasks[i];
        }
    }
    return NULL;
}

bool pg_native_task_resolve(const PGTask *t, uint64_t virtual_offset,
                            uint64_t *gpa)
{
    if (!t) {
        return false;
    }
    for (uint32_t i = 0; i < t->range_count; i++) {
        const PGRange *r = &t->ranges[i];
        if (virtual_offset >= r->virtual_offset &&
            virtual_offset < r->virtual_offset + r->length) {
            if (gpa) {
                *gpa = r->phys_addr + (virtual_offset - r->virtual_offset);
            }
            return true;
        }
    }
    return false;
}

/* --------------------------------------------------------- display helpers */

PGDisplay *pg_native_find_display(PGNativeDevice *d, uint32_t port)
{
    for (uint32_t i = 0; i < PVG_MAX_DISPLAYS; i++) {
        if (d->displays[i].valid && d->displays[i].port == port) {
            return &d->displays[i];
        }
    }
    return NULL;
}

static PGDisplay *pg_display_slot(PGNativeDevice *d, uint32_t port)
{
    PGDisplay *e = pg_native_find_display(d, port);
    if (e) {
        return e;
    }
    for (uint32_t i = 0; i < PVG_MAX_DISPLAYS; i++) {
        if (!d->displays[i].valid) {
            d->display_count++;
            return &d->displays[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------ child-FIFO helpers */

PGChildFIFO *pg_native_find_child_fifo(PGNativeDevice *d, uint32_t channel_id)
{
    for (uint32_t i = 0; i < PVG_MAX_CHILD_FIFOS; i++) {
        if (d->child_fifos[i].valid && d->child_fifos[i].channel_id == channel_id) {
            return &d->child_fifos[i];
        }
    }
    return NULL;
}

static PGChildFIFO *pg_child_fifo_slot(PGNativeDevice *d, uint32_t channel_id)
{
    PGChildFIFO *e = pg_native_find_child_fifo(d, channel_id);
    if (e) {
        return e;
    }
    for (uint32_t i = 0; i < PVG_MAX_CHILD_FIFOS; i++) {
        if (!d->child_fifos[i].valid) {
            d->child_fifo_count++;
            return &d->child_fifos[i];
        }
    }
    return NULL;
}

/* ----------------------------------------------- stamp / IRQ completion (§0.5)
 *
 * signalStamp: set bit `idx` in the read+clear IRQ-cause bitmask exposed at
 * 0x1018, then pulse the device IRQ. The guest reads 0x1018 to learn which
 * stamps fired and reads per-stamp values from the shared root page. Here we
 * model the bitmask + the edge IRQ; per-stamp value writes into the root page
 * are a backend concern only once a guest reads them back (TODO once the live
 * stamp table layout is exercised).
 */
/*
 * Write the completion stamp value into the guest stamp table in RAM. This is the
 * load-bearing step that clears the stamp wedge: the kernel's
 * IOGPUEventMachine::waitForStamp / finishEvent / testEvent poll this RAM value
 * (via AppleParavirtGPU::getStampBaseAddress = [gpu+0x370]) and a pure-RAM poll
 * never traps — so pulsing the IRQ alone is not enough; the host MUST publish the
 * value here.
 *
 * *** RE CORRECTION (live-proven) ***
 * The prior code wrote root_page_gpa(0x101c) + 0x1c8 + idx*4. That was derived
 * from the x86 PGRootFIFO host-framework layout and is WRONG for the ARM guest
 * driver. Live measurement (gdbstub gva2gpa on the address waitForStamp polls)
 * shows the stamp table the ARM guest reads is a SEPARATE buffer allocated in
 * AppleParavirtGPU::setupRoot ([gpu+0x348], mapped VA -> [gpu+0x370]); its GPA
 * equals the **setFifoBasePage (0x1030) page** (== d->ring_gpa) at **offset 0**,
 * with **4-byte stride** indexed by stamp index (IOGPUEventMachine::getStampOffset
 * == idx<<2). So slot idx lives at ring_gpa + idx*4. The wait that wedged
 * setupDeviceInfo was slot 0, target 0x800, polled at ring_gpa+0 (read as 0
 * because we wrote root_page+0x1c8 instead).
 *
 * We publish to ring_gpa + idx*4 (the location the kernel actually polls). We
 * also mirror to the legacy root_page+0x1c8 slot (harmless; the display/swap
 * mailbox path may still reference it) when the root page is mappable.
 */
static void pg_write_root_stamp(PGNativeDevice *d, uint32_t idx,
                                uint32_t stamp_value)
{
    if (idx >= PVG_ROOT_STAMP_MAX_SLOTS || !d->ops || !d->ops->host_ptr) {
        return;
    }

    /* PRIMARY: the stamp table at the setFifoBasePage page (ring_gpa), offset 0,
     * 4-byte stride — the location AppleParavirtGPU::getStampBaseAddress yields
     * and waitForStamp/testEvent poll. */
    if (d->ring_gpa) {
        uint64_t need = (uint64_t)PVG_ROOT_STAMP_MAX_SLOTS *
                        PVG_ROOT_STAMP_SLOT_STRIDE;
        uint8_t *stbl = (uint8_t *)d->ops->host_ptr(d->ops->opaque,
                                                    d->ring_gpa, need,
                                                    false /* writable */);
        if (stbl) {
            wr_u32(stbl + idx * PVG_ROOT_STAMP_SLOT_STRIDE, stamp_value);
            pg_log(d, "signalStamp: published stampTable[%u]=0x%x "
                      "@stampGPA=0x%llx (ring base, off 0)",
                   idx, stamp_value, (unsigned long long)d->ring_gpa);
        } else {
            pg_log(d, "signalStamp: stamp page 0x%llx not host-writable; "
                      "RAM stamp NOT published (IRQ only)",
                   (unsigned long long)d->ring_gpa);
        }
    }

    /* SECONDARY (legacy/compat): mirror the value + pending bitmask into the
     * root-page +0x1c8/+0x1c0 layout in case any consumer still references it.
     * Best-effort; never fatal. */
    if (d->root_page_gpa) {
        uint64_t need = PVG_ROOT_STAMP_TABLE_OFF +
                        (uint64_t)PVG_ROOT_STAMP_MAX_SLOTS *
                            PVG_ROOT_STAMP_SLOT_STRIDE;
        uint8_t *root = (uint8_t *)d->ops->host_ptr(d->ops->opaque,
                                                    d->root_page_gpa, need,
                                                    false /* writable */);
        if (root) {
            wr_u32(root + PVG_ROOT_STAMP_TABLE_OFF +
                       idx * PVG_ROOT_STAMP_SLOT_STRIDE, stamp_value);
            uint32_t mask = rd_u32(root + PVG_ROOT_STAMP_BITMASK_OFF);
            mask |= (1u << idx);
            wr_u32(root + PVG_ROOT_STAMP_BITMASK_OFF, mask);
        }
    }
}

/*
 * Stamp completion. The 32-bit stampValue carried in the FIFO command header
 * (and armed by IOGPUEventMachine::incrementStampNoRelease, +0x100 per submit)
 * packs (targetValue & 0xffffff00) | (stampIndex & 0xff). The kernel's
 * IOGPUEventMachine::testEvent / waitForStamp index the per-slot stamp pointer
 * array stampBaseArray = [EventMachine+0x20] by that low-byte index (sign-
 * extended via sxtb), then compare live >= target. Historically this backend
 * hardcoded slot 0; that is correct only for the root channel (low byte 0x00).
 * We now decode the index so a command targeting a non-zero stamp slot advances
 * the RIGHT slot (and sets the matching 0x1018 IRQ-cause bit).
 */
static void pg_signal_stamp_idx(PGNativeDevice *d, uint32_t idx,
                                uint32_t stamp_value)
{
    if (idx >= PVG_ROOT_STAMP_MAX_SLOTS) {
        idx = 0; /* keep within the mirrored slot range; root is the safe default */
    }
    /* Keep the slot monotonically non-decreasing in the top-24-bit domain the
     * kernel compares (testEvent / waitForStamp use value & 0xffffff00). A slot
     * is a generation counter; never regress it below an already-satisfied
     * fence. The low byte (stamp index/tag) is irrelevant to the magnitude. This
     * runs even when the root page is not yet host-mappable, so the tracked
     * value (and the replay assertions) stay correct. */
    if ((stamp_value & 0xffffff00u) >
        (d->stamp_slot_value[idx] & 0xffffff00u)) {
        d->stamp_slot_value[idx] = stamp_value;
    } else {
        stamp_value = d->stamp_slot_value[idx];
    }
    d->irq_cause |= (1u << idx);
    /* Publish the stamp value into the root-page RAM table FIRST (the kernel
     * polls this in RAM), then make the IRQ-cause bit visible + pulse the IRQ. */
    pg_write_root_stamp(d, idx, stamp_value);
    d->stats.stamps_signaled++;
    if (d->ops && d->ops->raise_irq) {
        d->ops->raise_irq(d->ops->opaque, PVG_IRQ_GFX);
    }
    d->stats.irqs_raised++;
    pg_log(d, "signalStamp idx=%u value=0x%x -> irq_cause=0x%x, IRQ pulsed",
           idx, stamp_value, d->irq_cause);
}

static void pg_signal_stamp(PGNativeDevice *d, uint32_t stamp_value)
{
    /* Decode the stamp index from the command header's stampValue low byte
     * (the kernel sxtb's it; we mask to 0..0x1f to stay in the mirrored range).
     * For the root channel this is 0, preserving the prior behaviour. */
    uint32_t idx = stamp_value & 0xffu;
    if (idx >= PVG_ROOT_STAMP_MAX_SLOTS) {
        idx = 0;
    }
    pg_signal_stamp_idx(d, idx, stamp_value);
}

/*
 * per-channel completion-stamp fences.
 *
 * After CmdGetDeviceInfo, AppleParavirtGPU::setupDeviceInfo issues
 * AppleParavirtChannel::addCommand on the device-info channel ([gpu+0x400]),
 * which arms a fence on that channel's stamp index ([chan+0x18], a NON-zero
 * slot, target = old+0x100) WITHOUT emitting a new FIFO doorbell, then blocks in
 * IOGPUEventMachine::finishEvent -> waitForStamp until stampBaseArray[idx] >=
 * target. The device-info channel is one of the virtual channels the guest
 * defined via CmdDefineChildFIFO (ids 1,2,3,4,12 per the live trace); the kernel
 * maps channel ids to fixed stamp slots (root=0, exec=1, object=2, memory=3,
 * info=0xc, plus the per-virtual-channel slots). Because the exact slot index is
 * obscured by PAC'd vtable init, we advance EVERY plausible channel-fence slot
 * to a monotonic high generation value when the device finishes a per-channel
 * fenced operation. This satisfies live >= target for whichever slot the wait
 * polls, and the monotonic guard in pg_write_root_stamp guarantees we never
 * regress the root slot 0 (which the normal command path keeps current).
 */
static void pg_advance_channel_fences(PGNativeDevice *d, uint32_t base_value)
{
    /* Fixed kernel channel->stamp indices (the completion contract §2):
     * 0=root, 1=exec, 2=object, 3=memory, 0xc=info. setupDeviceInfo's addCommand
     * arms its fence on the device-info channel's stamp index via
     * incrementStampNoRelease == current_shadow + 0x100. The shadow tracks the
     * SAME generation the FIFO commands already bumped (the device-info channel
     * may even be the root slot 0, [chan+0x18]==0). So the fence target is
     * (last seen gen) + 0x100; we must advance the slot strictly ABOVE that, not
     * merely to the command's own value. Push one extra generation step so
     * live >= target holds regardless of which slot/target the kernel armed. */
    static const uint32_t kFixedSlots[] = { 0, 1, 2, 3, 0xc };
    uint32_t hi = (base_value & 0xffffff00u) + 0x100u;
    if (hi == 0) {
        hi = 0x100; /* first generation; the kernel arms old(0)+0x100 = 0x100 */
    }

    uint32_t advanced = 0, top = 0;
    for (size_t i = 0; i < sizeof(kFixedSlots) / sizeof(kFixedSlots[0]); i++) {
        uint32_t slot = kFixedSlots[i];
        if (slot >= PVG_ROOT_STAMP_MAX_SLOTS) {
            continue;
        }
        pg_signal_stamp_idx(d, slot, hi | slot);
        advanced++;
        if (slot > top) {
            top = slot;
        }
    }
    /* Also advance the stamp slot for each defined virtual channel id (the
     * CmdDefineChildFIFO channels), in case the device-info channel's [chan+0x18]
     * equals its channel id rather than a fixed role index. */
    for (uint32_t i = 0; i < PVG_MAX_CHILD_FIFOS; i++) {
        if (!d->child_fifos[i].valid) {
            continue;
        }
        uint32_t slot = d->child_fifos[i].channel_id;
        if (slot == 0 || slot >= PVG_ROOT_STAMP_MAX_SLOTS) {
            continue;
        }
        pg_signal_stamp_idx(d, slot, hi | slot);
        advanced++;
        if (slot > top) {
            top = slot;
        }
    }
    d->stats.channel_fences_advanced += advanced;
    d->stats.last_fence_slot = top;
    d->stats.last_fence_value = hi;
    pg_log(d, "advanced %u per-channel fence slots to gen 0x%x (top slot %u)",
           advanced, hi, top);
}

/* ---------------------------------------- display / swap completion (CONTRACT 3)
 *
 * The wedge-clearing path. On a display swap-done (DisplayTransaction3 present
 * completion) the device must, per the completion contract §4.2/§7:
 *   1. write the swap seq into the display shared mailbox payload
 *      [mbox+0x200], and set the enabled swap-done/notify bit in the mailbox
 *      cause word [mbox+0x100] (bits per the enable mask [mbox+0x104]);
 *   2. set this display's bit in the DISPLAY cause register 0x14 (read+clear);
 *   3. raise the GPU device IRQ (same line as the stamp channel).
 *
 * The guest's IRQ block reads+clears 0x14, calls signalInterrupt() -> fires the
 * display event source -> handleHostInterrupt consumes the mailbox, then the
 * swap-done path (notify_swap_done_gated) clears [display+0x1790], so the next
 * clean_queue_gated() returns "complete" and swap_begin_gated drains.
 *
 * §8 SAFE FIRST ATTEMPT: bit2 + cause 0x14. The caller ALSO fires the stamp leg
 * (pg_signal_stamp) as the §8 fallback, so the swap retires via whichever leg
 * the kernel waits on.
 */
static void pg_signal_display_swap(PGNativeDevice *d, uint32_t port)
{
    PGDisplay *disp = pg_native_find_display(d, port);
    if (!disp || !disp->mbox_gpa) {
        /*
         * No mailbox recorded yet (the guest hasn't sent DisplaySetSharedStatePage
         * for this port): we still raise the display cause + IRQ so an early swap
         * is not silently dropped; the stamp leg (fired by the caller) covers the
         * completion until the mailbox is published.
         */
        d->disp_cause |= (1u << (port & 0x1f));
        d->stats.disp_cause_raised++;
        if (d->ops && d->ops->raise_irq) {
            d->ops->raise_irq(d->ops->opaque, PVG_IRQ_GFX);
            d->stats.irqs_raised++;
        }
        pg_log(d, "signalDisplaySwap port=%u: NO mailbox yet -> disp_cause=0x%x "
                  "+ IRQ (stamp leg covers completion)", port, d->disp_cause);
        return;
    }

    uint32_t seq = ++disp->swap_seq;

    /* (1) mailbox: payload + cause word, honoring the enable mask. */
    if (d->ops && d->ops->host_ptr) {
        uint8_t *mbox = (uint8_t *)d->ops->host_ptr(
            d->ops->opaque, disp->mbox_gpa, PVG_DISP_MBOX_SIZE, false);
        if (mbox) {
            uint32_t enable = rd_u32(mbox + PVG_DISP_MBOX_ENABLE_OFF);
            /*
             * The guest's start_hardware sets enable = 0xc (bits 2|3). If it has
             * not written the enable mask yet, default to 0xc so the swap-done
             * bit is still delivered (handleHostInterrupt masks cause & enable).
             */
            if (enable == 0) {
                enable = PVG_DISP_DEFAULT_ENABLE;
            }
            uint32_t notify = (1u << PVG_DISP_SWAP_DONE_BIT) & enable;
            if (notify == 0) {
                notify = (1u << PVG_DISP_SWAP_DONE_BIT); /* force the swap bit */
            }
            /* payload first (guest reads it after seeing the cause bit). */
            wr_u32(mbox + PVG_DISP_MBOX_PAYLOAD_OFF, seq);
            uint32_t cause = rd_u32(mbox + PVG_DISP_MBOX_CAUSE_OFF);
            cause |= notify;
            wr_u32(mbox + PVG_DISP_MBOX_CAUSE_OFF, cause);
            d->stats.swap_done_mailbox++;
            d->stats.last_mbox_cause = cause;
            pg_log(d, "signalDisplaySwap port=%u seq=%u mbox=0x%llx "
                      "cause=0x%x enable=0x%x payload@+0x200=%u", port, seq,
                   (unsigned long long)disp->mbox_gpa, cause, enable, seq);
        } else {
            pg_log(d, "signalDisplaySwap port=%u: mailbox 0x%llx not "
                      "host-writable; cause+IRQ only", port,
                   (unsigned long long)disp->mbox_gpa);
        }
    }

    /* (2) DISPLAY cause register 0x14: set this display's port bit. */
    d->disp_cause |= (1u << (port & 0x1f));
    d->stats.disp_cause_raised++;
    d->stats.last_swap_port = port;
    d->stats.last_swap_seq = seq;

    /* (3) raise the single GPU device IRQ (same line as the stamp channel). */
    if (d->ops && d->ops->raise_irq) {
        d->ops->raise_irq(d->ops->opaque, PVG_IRQ_GFX);
        d->stats.irqs_raised++;
    }
    pg_log(d, "signalDisplaySwap port=%u -> disp_cause(0x14)=0x%x, IRQ pulsed",
           port, d->disp_cause);
}

/* ---------------------------------------------- periodic vblank / frame-ready
 *
 * After createDisplayAttributes/DisplayAck the guest goes quiet,
 * waiting for the per-frame present to be driven. A real display device delivers
 * a periodic vblank/refresh interrupt; the guest's refresh loop (WindowServer's
 * CADisplayLink / the IOMFB swap loop) consumes it to schedule + submit the first
 * DisplayTransaction3 present. We pulse it via the SAME host->guest transport as a
 * swap-done (the completion contract §4.2): a monotonic frame counter written into
 * the display mailbox payload (+0x200) + this display's bit in the DISPLAY cause
 * register 0x14 + the GPU IRQ, which fires the per-display IOInterruptEventSource
 * -> signalInterrupt -> the refresh loop wakes.
 *
 * We deliberately do NOT set mailbox cause bits 2/3 here: those select
 * create/destroyDisplayAttributes in handleHostInterrupt (kext 0x9314), and a
 * vblank must not churn the attribute path. With cause&flags == 0, the mailbox
 * handler is a clean no-op; the wake itself (0x14 + IRQ + the +0x200 frame seq)
 * is the vblank signal. No-op until a display mailbox is live (post op 0x01).
 */
static uint64_t pg_scan_fb_gpa(PGNativeDevice *d, const PGSurface *s);

uint32_t pg_native_vblank(PGNativeDevice *d)
{
    uint32_t pulsed = 0;
    bool any_pulse = false;

    for (unsigned i = 0; i < PVG_MAX_DISPLAYS; i++) {
        PGDisplay *disp = &d->displays[i];
        if (!disp->valid || !disp->mbox_gpa) {
            continue;
        }

        uint32_t seq = ++disp->vblank_seq;

        /* (1) publish the frame/vblank seq into the mailbox payload (+0x200). */
        if (d->ops && d->ops->host_ptr) {
            uint8_t *mbox = (uint8_t *)d->ops->host_ptr(
                d->ops->opaque, disp->mbox_gpa, PVG_DISP_MBOX_SIZE, false);
            if (mbox) {
                wr_u32(mbox + PVG_DISP_MBOX_PAYLOAD_OFF, seq);
                d->stats.last_vblank_seq = seq;
            }
        }

        /* (2) set this display's bit in the DISPLAY cause register 0x14. */
        d->disp_cause |= (1u << (disp->port & 0x1f));
        d->stats.disp_cause_raised++;
        d->stats.vblank_ticks++;
        pulsed++;
        any_pulse = true;
    }

    /* (3) one GPU IRQ pulse covers all pulsed displays (shared line). */
    if (any_pulse && d->ops && d->ops->raise_irq) {
        d->ops->raise_irq(d->ops->opaque, PVG_IRQ_GFX);
        d->stats.irqs_raised++;
        /* Log sparsely (every 60th tick ~= 1/s) to avoid flooding the trace. */
        if ((d->stats.vblank_ticks % 60) == 1) {
            pg_log(d, "vblank: pulsed %u display(s) seq=%u disp_cause(0x14)=0x%x "
                      "(tick #%llu)", pulsed, d->stats.last_vblank_seq,
                   d->disp_cause,
                   (unsigned long long)d->stats.vblank_ticks);
        }
    }

    /*
     * APPLE_GFX_VK_TEST (bring-up): drive the software-Vulkan (lavapipe)
     * test present so the rendered test image reaches VNC, proving the whole
     * VkDevice/queue/command/readback/present_bgra8 seam end-to-end BEFORE the
     * real exec_indirect translator lands. Gated on the env var (cached like the
     * other checks here) and throttled to every ~30th vblank to avoid log/present
     * spam. On a successful present we RETURN early to skip the FBSCAN present
     * below, so the VK test image is what reaches the display this frame.
     */
    {
        static int vk_test = -1;
        if (vk_test < 0) {
            /* Either env drives present(); present() itself picks the clear present vs the
             * textured-quad render (APPLE_GFX_VK_QUAD -> ctx->quad_mode). */
            vk_test = (getenv("APPLE_GFX_VK_TEST") ||
                       getenv("APPLE_GFX_VK_QUAD")) ? 1 : 0;
        }
        if (vk_test && pg_vk_ops() && d->vk_ctx) {
            static uint32_t vk_tick;
            if ((vk_tick++ % 30) == 0) {
                if (pg_vk_ops()->present(d->vk_ctx, 0)) {
                    return pulsed;   /* VK test image presented; skip FBSCAN */
                }
            }
        }
    }

    /*
     * APPLE_GFX_VK_PRESENT_FILE=<path> (Track A — off-box captured-frame replay):
     * present a REAL captured Monterey BGRA frame through the SAME software-Vulkan
     * guest pipeline (pg_vk_ops()->present_guest) on a GPU-less host, so the
     * frame reaches VNC cross-platform WITHOUT needing a live guest framebuffer to
     * FBSCAN. The file is a 16-byte header { "PVGF", u32 width, u32 height, u32
     * stride } (little-endian) followed by width*height*4 BGRA8 bytes (file size
     * == 16 + stride*height). Read+parsed ONCE into a cached static buffer, then
     * re-presented every ~30th vblank (matching the VK_TEST throttle). On a
     * successful present we RETURN early — exactly like the VK_TEST path — so the
     * file frame is what reaches the display this tick, skipping FBSCAN entirely.
     * Cached/NULL-safe like the other env checks here; activates ONLY when the env
     * var is set, so VK_TEST / VK_QUAD / VK_GUEST / FBSCAN are untouched otherwise.
     */
    {
        static int vk_pfile = -1;          /* env resolved once: 1=on, 0=off */
        static int pfile_loaded;           /* 0=untried, 1=loaded ok, -1=failed */
        static uint8_t *pfile_pixels;      /* malloc'd BGRA8 payload */
        static uint32_t pfile_w, pfile_h, pfile_stride;
        if (vk_pfile < 0) {
            vk_pfile = getenv("APPLE_GFX_VK_PRESENT_FILE") ? 1 : 0;
        }
        if (vk_pfile && pg_vk_ops() && pg_vk_ops()->present_guest && d->vk_ctx) {
            /* Lazy one-shot load+parse+validate of the present file. */
            if (pfile_loaded == 0) {
                pfile_loaded = -1;   /* assume failure until fully validated */
                const char *path = getenv("APPLE_GFX_VK_PRESENT_FILE");
                FILE *f = path ? fopen(path, "rb") : NULL;
                if (f) {
                    uint8_t hdr[16];
                    if (fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr) &&
                        hdr[0] == 'P' && hdr[1] == 'V' &&
                        hdr[2] == 'G' && hdr[3] == 'F') {
                        uint32_t w = rd_u32(hdr + 4);
                        uint32_t h = rd_u32(hdr + 8);
                        uint32_t stride = rd_u32(hdr + 12);
                        /* Sanity-bound the geometry (avoid wild allocs) and require
                         * stride >= width*4 so each row's BGRA8 fits. */
                        if (w && h && stride && w <= 16384 && h <= 16384 &&
                            stride >= (uint64_t)w * 4 && stride <= 16384u * 4) {
                            uint64_t paylen = (uint64_t)stride * h;
                            /* Confirm the file is exactly header + payload. */
                            if (fseek(f, 0, SEEK_END) == 0) {
                                long fsz = ftell(f);
                                if (fsz >= 0 &&
                                    (uint64_t)fsz == 16 + paylen &&
                                    paylen <= (uint64_t)256 * 1024 * 1024) {
                                    uint8_t *buf = (uint8_t *)malloc((size_t)paylen);
                                    if (buf &&
                                        fseek(f, 16, SEEK_SET) == 0 &&
                                        fread(buf, 1, (size_t)paylen, f) == paylen) {
                                        pfile_pixels = buf;
                                        pfile_w = w; pfile_h = h;
                                        pfile_stride = stride;
                                        pfile_loaded = 1;
                                        pg_log(d, "VK_PRESENT_FILE loaded %ux%u "
                                                  "stride=%u (%llu bytes) from %s",
                                               w, h, stride,
                                               (unsigned long long)paylen, path);
                                    } else {
                                        free(buf);
                                        pg_log(d, "VK_PRESENT_FILE: payload read "
                                                  "failed from %s", path);
                                    }
                                } else {
                                    pg_log(d, "VK_PRESENT_FILE: size mismatch "
                                              "(have %ld, want %llu) in %s",
                                           fsz,
                                           (unsigned long long)(16 + paylen), path);
                                }
                            }
                        } else {
                            pg_log(d, "VK_PRESENT_FILE: bad geometry "
                                      "%ux%u stride=%u in %s", w, h, stride, path);
                        }
                    } else {
                        pg_log(d, "VK_PRESENT_FILE: bad/short header (magic) "
                                  "in %s", path ? path : "(null)");
                    }
                    fclose(f);
                } else {
                    pg_log(d, "VK_PRESENT_FILE: cannot open %s",
                           path ? path : "(null)");
                }
            }
            /* Re-present the cached file frame on the throttled tick, then RETURN
             * early to skip the FBSCAN present (mirrors the VK_TEST path). */
            if (pfile_loaded == 1 && pfile_pixels) {
                static uint32_t pfile_tick;
                if ((pfile_tick++ % 30) == 0) {
                    if (pg_vk_ops()->present_guest(d->vk_ctx, pfile_w, pfile_h,
                                                   pfile_stride, pfile_pixels)) {
                        return pulsed;   /* file frame presented; skip FBSCAN */
                    }
                }
            }
        }
    }

    /*
     * APPLE_GFX_FBSCAN (Monterey SW-composite present, ): under the kill-Metal
     * patch, SkyLight's CompositorSW CPU-composites the desktop into a plain
     * guest-RAM IOSurface (RE wf_f010e3bb: IOSurfaceLock+GetBaseAddress+CGBlit,
     * no Metal), but program_hardware_swap_gated can't register it with the stub
     * GPU mapper so the swap/present never fires (mapSurface=0, op 0x07=0). The
     * pixels are nonetheless in RAM. op_exec_indirect (where the POL scanner
     * lives) never fires under SW-compositor, so drive the scan from the vblank:
     * find the composited 1280x800 BGRA framebuffer and present it directly.
     * Cache the GPA once found (the expensive 8 GiB scan then stops); re-present
     * the cached buffer each tick so it tracks the latest composited frame.
     */
    if (any_pulse && d->ops && d->ops->host_ptr && d->ops->present_bgra8) {
        static int fbscan = -1;
        static uint64_t cached_gpa;
        if (fbscan < 0) {
            fbscan = getenv("APPLE_GFX_FBSCAN") ? 1 : 0;
        }
        /* APPLE_GFX_VK_GUEST: route the SAME FBSCAN'd guest framebuffer through
         * the software-Vulkan quad pipeline (pg_vk_ops()->present_guest) so the
         * guest's real composited pixels become the sampled source, instead of
         * present_bgra8'ing them directly. Cached like the other env checks here;
         * NULL-safe (no present_guest / no vk_ctx => keep the direct path). */
        static int vk_guest = -1;
        if (vk_guest < 0) {
            vk_guest = getenv("APPLE_GFX_VK_GUEST") ? 1 : 0;
        }
        static uint32_t cw, ch, cstride;
        if (fbscan) {
            /* The live IOSurface walk proved the SW-composited desktop is a
             * 1920x1080 BGRA scanout (NOT 1280x800/Retina). Scan ONLY that
             * geometry (avoids the 1280x800 noise that an early scan latched) and
             * RE-SCAN every ~10s instead of caching the first hit — the desktop
             * surface only appears after first-boot finalization (~10min), long
             * after the early ticks, so a one-shot latch misses it. APPLE_GFX_FBGEOM
             * =WxH overrides the geometry. */
            unsigned fw = 1920, fh = 1080;
            const char *fg = getenv("APPLE_GFX_FBGEOM");
            if (fg) {
                sscanf(fg, "%ux%u", &fw, &fh);
            }
            if ((d->stats.vblank_ticks % 600) == 5) {   /* re-scan ~every 10s */
                PGSurface ss;
                memset(&ss, 0, sizeof(ss));
                ss.surface_id = 0;
                ss.width  = fw;
                ss.height = fh;
                ss.stride = fw * 4;
                uint64_t g = pg_scan_fb_gpa(d, &ss);
                if (g) {
                    cached_gpa = g; cw = fw; ch = fh; cstride = fw * 4;
                    pg_log(d, "FBSCAN LOCKED %ux%u stride=%u @0x%llx", cw, ch,
                           cstride, (unsigned long long)g);
                }
            }
            if (cached_gpa) {
                const void *src = d->ops->host_ptr(
                    d->ops->opaque, cached_gpa, (uint64_t)cstride * ch, true);
                if (src) {
                    /* Prefer the software-Vulkan quad path (guest pixels routed
                     * through lavapipe) when APPLE_GFX_VK_GUEST is set and the
                     * present_guest op + context are live; on success skip the
                     * direct FBSCAN present_bgra8 for this frame. Otherwise (or on
                     * failure) fall through to the existing direct present. */
                    bool vk_done = false;
                    if (vk_guest && pg_vk_ops() && pg_vk_ops()->present_guest &&
                        d->vk_ctx) {
                        vk_done = pg_vk_ops()->present_guest(d->vk_ctx, cw, ch,
                                                             cstride, src);
                    }
                    if (!vk_done) {
                        d->ops->present_bgra8(d->ops->opaque, cw, ch, cstride, src);
                    }
                }
            }
        }
    }
    return pulsed;
}

/* --------------------------------------------------------- opcode handlers */

/* ---------------------------------------- display attributes IRQ (CONTRACT §3.2)
 *
 * After op 0x01 records + populates the mailbox, raise the DISPLAY interrupt with
 * mailbox cause bit2 set so the guest's AppleParavirtDisplay::handleHostInterrupt
 * (kext 0x9314) runs createDisplayAttributes (kext 0xa208). handleHostInterrupt:
 *   flags = ldar [mbox+0x104];                       (enable mask, start_hw=0xc)
 *   cause = ldclral([mbox+0x100], flags); cause &= flags;
 *   if (cause & (1<<2)) createDisplayAttributes();
 * So we OR bit2 into [mbox+0x100] (honoring the enable mask, defaulting to 0xc if
 * the guest hasn't written it yet), set this display's bit in the DISPLAY cause
 * register 0x14, and pulse the GPU IRQ line.
 */
static void pg_signal_display_attr(PGNativeDevice *d, PGDisplay *disp)
{
    if (!disp || !disp->mbox_gpa) {
        return;
    }
    if (d->ops && d->ops->host_ptr) {
        uint8_t *mbox = (uint8_t *)d->ops->host_ptr(
            d->ops->opaque, disp->mbox_gpa, PVG_DISP_MBOX_SIZE, false);
        if (mbox) {
            uint32_t enable = rd_u32(mbox + PVG_DISP_MBOX_ENABLE_OFF);
            if (enable == 0) {
                enable = PVG_DISP_DEFAULT_ENABLE;   /* bits 2|3 */
            }
            uint32_t cause = rd_u32(mbox + PVG_DISP_MBOX_CAUSE_OFF);
            cause |= ((1u << PVG_DISP_ATTR_BIT) & enable) | (1u << PVG_DISP_ATTR_BIT);
            wr_u32(mbox + PVG_DISP_MBOX_CAUSE_OFF, cause);
            d->stats.last_mbox_cause = cause;
            pg_log(d, "displayAttr port=%u: mbox[0x100]=0x%x (bit2 set, "
                      "enable=0x%x) -> createDisplayAttributes", disp->port,
                   cause, enable);
        }
    }
    d->disp_cause |= (1u << (disp->port & 0x1f));
    d->stats.disp_cause_raised++;
    d->stats.display_attr_irqs++;
    if (d->ops && d->ops->raise_irq) {
        d->ops->raise_irq(d->ops->opaque, PVG_IRQ_GFX);
        d->stats.irqs_raised++;
    }
}

/*
 * Populate the display-info block in the mailbox page (CONTRACT §3.2) so that
 * createDisplayAttributes builds a non-empty IODisplayModeList: mode count >= 1
 * at mbox[0x208], one timing element {w,h,rate} at mbox[0x218], the preferred
 * W/H at mbox[0x210/0x212], and a display number at mbox[0x0]. No EDID is needed.
 */
static void pg_populate_display_modes(PGNativeDevice *d, PGDisplay *disp,
                                      uint8_t *mbox)
{
    /* APPLE_GFX_NO_DISPLAY=1: advertise ZERO modes so createDisplayAttributes
     * bails and apple-gfx becomes a DISPLAYLESS GPU (device still alive -> no
     * dead-BAR MMIO faults). Lets a plain-PCI framebuffer (bochs/cirrus) become
     * the elected primary display instead of apple-gfx. */
    static int no_disp = -1;
    if (no_disp < 0) no_disp = getenv("APPLE_GFX_NO_DISPLAY") ? 1 : 0;
    if (no_disp) {
        wr_u32(mbox + PVG_DISP_MBOX_NUM_OFF, disp->port + 1);
        wr_u16(mbox + PVG_DISP_MBOX_MODE_COUNT_OFF, 0);  /* empty mode list */
        d->stats.mode_lists_published++;
        pg_log(d, "displayModes port=%u: NO_DISPLAY (mode count=0) -> apple-gfx "
                  "displayless; PCI framebuffer should be elected primary",
               disp->port);
        return;
    }
    /* display number = port+1 (any non-zero is fine; appended as a property). */
    wr_u32(mbox + PVG_DISP_MBOX_NUM_OFF, disp->port + 1);

    /* one mode entry: 1920x1080@60 (match the composited scanout). : override
     * via APPLE_GFX_MODE=WxH@R to iterate the mode without rebuilding. */
    uint32_t mw = PVG_DISP_DEFAULT_WIDTH, mh = PVG_DISP_DEFAULT_HEIGHT;
    uint32_t mr = PVG_DISP_DEFAULT_REFRESH;
    const char *menv = getenv("APPLE_GFX_MODE");
    if (menv) {
        unsigned ew = 0, eh = 0, er = 60;
        if (sscanf(menv, "%ux%u@%u", &ew, &eh, &er) >= 2 && ew && eh) {
            mw = ew; mh = eh; mr = er ? er : 60;
        }
    }
    wr_u16(mbox + PVG_DISP_MBOX_MODE_COUNT_OFF, 1);
    wr_u16(mbox + PVG_DISP_MBOX_PREF_W_OFF, mw);
    wr_u16(mbox + PVG_DISP_MBOX_PREF_H_OFF, mh);

    uint8_t *m0 = mbox + PVG_DISP_MBOX_MODE_ARRAY_OFF;
    wr_u16(m0 + PVG_DISP_MODE_W_OFF,    mw);
    wr_u16(m0 + PVG_DISP_MODE_H_OFF,    mh);
    wr_u32(m0 + PVG_DISP_MODE_RATE_OFF, mr);
    /* element bytes +8..+0xf left zero (createTimingElementWithDimensionsAndRate
     * reads only {w,h,rate}; the rest are filled only if the mode is rejected). */

    d->stats.mode_lists_published++;
    pg_log(d, "displayModes port=%u: mbox[0x208]=1 mode[0]=%ux%u@%u "
              "pref=%ux%u", disp->port, mw, mh, mr, mw, mh);
}

/*
 * DisplaySetSharedStatePage (op 0x01): record the per-port display shared-state
 * mailbox GPA, write the mandatory PORT ECHO that clears AppleParavirtDisplay::
 * start, and pre-populate the mode list so the follow-on display IRQ reaches
 * createDisplayAttributes (the display bring-up notes §1.4 / §3.1 / §3.2).
 *
 * The AUTHORITATIVE wire layout is the live ARM guest producer setupSharedState
 * (kext 0x9c88-0x9c90: `w8=fPort; w9=mailboxGPA>>14; stp w8,w9,[body]`), i.e.
 *   payload[0x0] u32 = port (== fPort);
 *   payload[0x4] u32 = mailbox PFN (mailbox GPA = PFN<<14 = [display+0x1d08]).
 * (The x86 framework consumer reads these reversed; we trust the ARM guest that
 * actually boots here — see the display bring-up notes §5.)
 *
 * THE GATE: setupSharedState submits this command then asserts
 *   ldrh w9,[mbox+0x12]; cmp fPort,w9; b.ne -> teardown -> return 0.
 * The device MUST write mbox[0x12] (u16) = port for start to return true; absent
 * it the display is torn down and the GPU re-matches every ~20s (SESSION-52).
 */
static void op_display_set_shared_state(PGNativeDevice *d, const PGCmdHeader *hdr,
                                        const uint8_t *payload,
                                        uint32_t payload_len)
{
    if (payload_len < 8) {
        pg_log(d, "DisplaySetSharedStatePage: short payload (%u < 8)",
               payload_len);
        d->stats.parse_errors++;
        return;
    }
    /* ARM guest producer order: { port@0 ; mailboxPFN@4 }. */
    uint32_t port = rd_u32(payload + 0x0);
    uint32_t page = rd_u32(payload + 0x4);
    uint64_t mbox_gpa = (uint64_t)page << PVG_GUEST_PAGE_SHIFT;

    PGDisplay *disp = pg_display_slot(d, port);
    if (disp) {
        disp->valid    = true;
        disp->port     = port;
        disp->mbox_gpa = mbox_gpa;
    }
    d->stats.set_shared_state++;
    pg_log(d, "DisplaySetSharedStatePage port=%u pfn=0x%x -> mailbox GPA=0x%llx "
              "(slot %s)", port, page, (unsigned long long)mbox_gpa,
           disp ? "ok" : "FULL");

    /*
     * (1) THE MANDATORY PORT ECHO + (2) the mode list. Map the mailbox page and
     * write mbox[0x12] (u16) = port to satisfy `fSharedState->port == fPort`, then
     * populate the display-info block (mode count + one timing element) so the
     * follow-on display IRQ's createDisplayAttributes accepts a non-empty list.
     */
    if (disp && mbox_gpa && d->ops && d->ops->host_ptr) {
        uint8_t *mbox = (uint8_t *)d->ops->host_ptr(
            d->ops->opaque, mbox_gpa, PVG_DISP_MBOX_SIZE, false /* writable */);
        if (mbox) {
            /* (1) the echo that clears AppleParavirtDisplay::start. */
            mbox[PVG_DISP_MBOX_PORT_ECHO_OFF + 0] = (uint8_t)(port);
            mbox[PVG_DISP_MBOX_PORT_ECHO_OFF + 1] = (uint8_t)(port >> 8);
            d->stats.port_echoes++;
            pg_log(d, "DisplaySetSharedStatePage port=%u: ECHO mbox[0x12]=%u "
                      "(clears setupSharedState gate)", port, port);

            /* (2) the mode list (for createDisplayAttributes). */
            pg_populate_display_modes(d, disp, mbox);
        } else {
            pg_log(d, "DisplaySetSharedStatePage port=%u: mailbox GPA 0x%llx not "
                      "host-writable; PORT ECHO NOT published (start will fail)",
                   port, (unsigned long long)mbox_gpa);
        }
    }

    /* Ack the command's own fence stamp first (setupSharedState's submit fence). */
    pg_signal_stamp(d, hdr->stampValue);

    /*
     * (3) Raise the display attributes IRQ (cause bit2 / display cause 0x14) so
     * handleHostInterrupt -> createDisplayAttributes runs and consumes the mode
     * list we just published. Done after the port echo + mode list are in RAM.
     */
    if (disp && mbox_gpa) {
        pg_signal_display_attr(d, disp);
    }
}

/* CmdDefineTask2 (0x38): fires the createTask host cb, then signals the stamp. */
static void op_define_task2(PGNativeDevice *d, const PGCmdHeader *hdr,
                            const uint8_t *payload, uint32_t payload_len)
{
    if (payload_len < 0x10) {
        pg_log(d, "DefineTask2: short payload (%u < 0x10)", payload_len);
        d->stats.parse_errors++;
        return;
    }
    uint32_t taskID_flag = rd_u32(payload + 0x0);
    uint64_t taskRoot    = rd_u64(payload + 0x4);
    uint32_t length      = rd_u32(payload + 0xc);
    uint32_t task_id     = taskID_flag >> 1;

    PGTask *t = pg_task_alloc(d, task_id, length ? length : PVG_CREATETASK_VMSIZE,
                              taskRoot);
    d->stats.define_task++;
    d->stats.createtask_cb++;
    pg_log(d, "CmdDefineTask2 taskID=%u flag=%u taskRoot=0x%llx length=0x%x "
              "-> createTask (slot %s)",
           task_id, taskID_flag & 1,
           (unsigned long long)taskRoot, length, t ? "ok" : "FULL");
    pg_signal_stamp(d, hdr->stampValue);
}

/* CmdMapMemory2 (0x39): record the guest-phys range into the task; fire stamp.
 *
 * The MMIO-driven bring-up (RootPage write) and the FIFO-driven MapMemory2 both
 * end up calling the mapMemory host cb in the framework; the live trace shows
 * the host pointer apple_gfx_host_ptr_for_gpa_range resolving the range. The
 * command payload carries {taskID, virtualOffset, length}; the physical ranges
 * themselves are described by the task's root page (PGPhysicalMemoryRange_t
 * array) which the host reads from guest RAM. For the structural backend we
 * record the (offset,length) and resolve the GPA from the task root page when a
 * present needs it; the harness validates against the trace's explicit ranges.
 */
static void op_map_memory2(PGNativeDevice *d, const PGCmdHeader *hdr,
                           const uint8_t *payload, uint32_t payload_len)
{
    if (payload_len < 0x14) {
        pg_log(d, "MapMemory2: short payload (%u < 0x14)", payload_len);
        d->stats.parse_errors++;
        return;
    }
    uint32_t task_id        = rd_u32(payload + 0x0);
    uint64_t virtual_offset = rd_u64(payload + 0x4);
    uint64_t length         = rd_u64(payload + 0xc);

    PGTask *t = pg_native_find_task(d, task_id);
    if (!t) {
        /* The first task is the implicit createTask(16MB) on RootPage. */
        t = pg_task_alloc(d, task_id, PVG_CREATETASK_VMSIZE, d->root_page_gpa);
    }
    if (t && t->range_count < PVG_MAX_RANGES) {
        PGRange *r = &t->ranges[t->range_count++];
        r->virtual_offset = virtual_offset;
        r->length = length;
        r->read_only = false;
        /*
         * Physical address: the wire payload references the task root page,
         * which holds the PGPhysicalMemoryRange_t table. Without that table in
         * a synthetic packet we leave phys_addr 0; the QEMU glue / harness fill
         * it from the host mapMemory cb (which carries the real GPA). See the
         * harness, which records ranges from the captured trace directly.
         */
        r->phys_addr = 0;
    }
    d->stats.map_memory++;
    d->stats.mapmemory_cb++;
    pg_log(d, "CmdMapMemory2 taskID=%u virtualOffset=0x%llx length=0x%llx "
              "-> mapMemory",
           task_id, (unsigned long long)virtual_offset,
           (unsigned long long)length);
    /* EXP-1 probe: dump the FULL payload to locate where the GPA lives (the
     * 0x14-byte head has only task/voff/len; the phys ranges may follow inline
     * or live in the task root page). Gated on APPLE_GFX_DUMP_EXEC. */
    if (getenv("APPLE_GFX_DUMP_EXEC")) {
        char hx[600]; int n = 0;
        for (uint32_t k = 0; k < payload_len && n < (int)sizeof(hx) - 3; k++)
            n += snprintf(hx + n, sizeof(hx) - n, "%02x", payload[k]);
        fprintf(stderr, "[PVGNATIVE] MapMemory2 payload_len=%u root_page_gpa=0x%llx "
                "data=%s\n", payload_len,
                (unsigned long long)d->root_page_gpa, hx);
        fflush(stderr);
    }
    pg_signal_stamp(d, hdr->stampValue);
}

/*
 * Record a resolved guest-phys range directly into a task. Used by the QEMU glue
 * (which receives the real GPA via the host mapMemory cb) and by the replay
 * harness (which has the captured GPAs from a captured live trace), so the present
 * path can resolve a surface to real guest RAM.
 */
void pg_native_record_range(PGNativeDevice *d, uint32_t task_id,
                            uint64_t virtual_offset, uint64_t phys_addr,
                            uint64_t length, bool read_only);
void pg_native_record_range(PGNativeDevice *d, uint32_t task_id,
                            uint64_t virtual_offset, uint64_t phys_addr,
                            uint64_t length, bool read_only)
{
    PGTask *t = pg_native_find_task(d, task_id);
    if (!t) {
        t = pg_task_alloc(d, task_id, PVG_CREATETASK_VMSIZE, d->root_page_gpa);
    }
    if (t && t->range_count < PVG_MAX_RANGES) {
        PGRange *r = &t->ranges[t->range_count++];
        r->virtual_offset = virtual_offset;
        r->phys_addr = phys_addr;
        r->length = length;
        r->read_only = read_only;
    }
}

/* ----------------------------------------------------- iosfc surface table
 *
 * §0.7.2: the surfaceID carried by DisplayTransaction3 indexes the IOSurface
 * registered through the iosfc BAR. A registered surface resolves to plane[0]'s
 * {host VA / GPA, width, height, stride, pixelFormat}. We build that mapping
 * here, sourced entirely from the iosfc descriptor region + the surface's own
 * u32 PTE table — no XPC, no PGMemoryMap framework dependency.
 */

/* Cache slot for surfaceID, reusing an existing slot if already mapped. */
static PGSurface *pg_surface_slot(PGNativeDevice *d, uint32_t surface_id)
{
    for (uint32_t i = 0; i < PVG_MAX_SURFACES; i++) {
        if (d->surfaces[i].valid && d->surfaces[i].surface_id == surface_id) {
            return &d->surfaces[i];
        }
    }
    for (uint32_t i = 0; i < PVG_MAX_SURFACES; i++) {
        if (!d->surfaces[i].valid) {
            return &d->surfaces[i];
        }
    }
    return NULL;
}

PGSurface *pg_native_find_surface(PGNativeDevice *d, uint32_t surface_id)
{
    for (uint32_t i = 0; i < PVG_MAX_SURFACES; i++) {
        if (d->surfaces[i].valid && d->surfaces[i].surface_id == surface_id) {
            return &d->surfaces[i];
        }
    }
    return NULL;
}

/* Read len bytes of guest RAM into dst, host_ptr first then read_mem. */
static bool pg_read_guest(PGNativeDevice *d, uint64_t gpa, uint64_t len,
                          void *dst)
{
    if (d->ops && d->ops->host_ptr) {
        void *p = d->ops->host_ptr(d->ops->opaque, gpa, len, true);
        if (p) {
            memcpy(dst, p, len);
            return true;
        }
    }
    if (d->ops && d->ops->read_mem) {
        return d->ops->read_mem(d->ops->opaque, gpa, len, dst);
    }
    return false;
}

/* True if `gpa` is backed by readable guest RAM (host_ptr/read_mem succeed). */
static bool pg_gpa_readable(PGNativeDevice *d, uint64_t gpa)
{
    uint8_t probe[4];
    return pg_read_guest(d, gpa, sizeof(probe), probe);
}

/*
 * Validate that `pte_src_gpa` is a u32 PTE array whose leading entries are
 * coherent present-PTEs (bit0 set) resolving to readable guest pages. Used to
 * auto-discover the scattered-page table location inside the descriptor.
 */
static bool pg_native_pte_array_ok(PGNativeDevice *d, uint64_t pte_src_gpa,
                                   uint32_t page_count)
{
    uint32_t n = page_count < PVG_NATIVE_PTE_VALIDATE_N
               ? page_count : PVG_NATIVE_PTE_VALIDATE_N;
    if (n == 0) {
        return false;
    }
    uint64_t seen[PVG_NATIVE_PTE_VALIDATE_N];
    for (uint32_t i = 0; i < n; i++) {
        uint8_t pb[4];
        if (!pg_read_guest(d, pte_src_gpa + (uint64_t)i * 4, sizeof(pb), pb)) {
            return false;
        }
        uint32_t p = rd_u32(pb);
        /* present bit only (bit1 = writable: scanout pages are RW, so the
         * pixel-page PTEs are 0b11 not 0b01 — don't require the strict mask). */
        if (!(p & PVG_IOSFC_PTE_PRESENT)) {
            return false;
        }
        uint64_t gpa = (uint64_t)(p >> 2) << PVG_GUEST_PAGE_SHIFT;
        if (gpa == 0 || !pg_gpa_readable(d, gpa)) {
            return false;
        }
        /* a real scattered page table has DISTINCT pages — reject coincidental
         * runs of present-looking words (kills auto-discovery false matches). */
        for (uint32_t j = 0; j < i; j++) {
            if (seen[j] == gpa) {
                return false;
            }
        }
        seen[i] = gpa;
    }
    return true;
}

/*
 *  native present: resolve surface `s`'s scattered pixel-page GPAs by walking
 * the iosfc PTE array (wire_mapping@0x83167a8 packs PFN=GPA>>14 into u32 PTEs).
 * For 1080p the 507-entry array can't be inline in the 0x200 descriptor, so it
 * lives behind an indirect pointer in the descriptor (or the DESC2 overflow
 * table). The pointer offset/encoding is auto-discovered (env-overridable) by
 * scanning the descriptor for a u64/u32 — interpreted as raw GPA or PFN<<14 —
 * whose target holds a coherent run of present PTEs. On success fills
 * s->page_gpas/page_count/contiguous/base_gpa; returns false otherwise (the
 * caller then falls back to the FBSCAN heuristic / base_gpa=0).
 */
static bool pg_native_resolve_pages(PGNativeDevice *d, PGSurface *s,
                                    uint64_t desc_gpa, const uint8_t *hdr)
{
    /* Pages spanning [base_offset, base_offset + stride*height) of the backing —
     * the PTE array indexes the whole backing from page 0. */
    uint32_t page_count =
        (uint32_t)(((uint64_t)s->base_offset + (uint64_t)s->stride * s->height
                    + 0x3FFF) >> 14);
    if (!page_count) {
        return false;
    }

    uint64_t pte_src_gpa = 0;
    const char *e;

    if ((e = getenv("APPLE_GFX_PTE_DESC2")) && atoi(e) && d->iosfc_desc2_gpa &&
        pg_native_pte_array_ok(d, d->iosfc_desc2_gpa, page_count)) {
        pte_src_gpa = d->iosfc_desc2_gpa;
    } else if ((e = getenv("APPLE_GFX_PTE_IND"))) {
        uint32_t off = (uint32_t)strtoul(e, NULL, 0);
        if (off + 8 <= 0x200) {
            pte_src_gpa = rd_u64(hdr + off);
        }
    } else if ((e = getenv("APPLE_GFX_PTE_OFF"))) {
        uint32_t off = (uint32_t)strtoul(e, NULL, 0);
        pte_src_gpa = desc_gpa + off;
    } else {
        /* (0) PRIMARY (live-confirmed on Tahoe 26.5, set-up disk): the descriptor's
         * WORD0 (hdr+0x00) is itself a PTE — PFN<<2 | present(0x1) — pointing at a
         * SEPARATE page that holds the surface's page-list ARRAY (one PFN<<2|present
         * per 16 KiB pixel page; `page_count` entries then zero). The page-list array
         * is NOT inline in the 0x200 geometry descriptor; it is one indirection away
         * via word0. Decoded: a word0 of 0x001d8909 -> (>>2)<<14 = 0x1d8908000 = a
         * clean 507-entry PFN table (alloc 0x7e9000 = 1920*1080*4). An earlier scan
         * missed this because word0 has the present bit set (cand & 0x3 != 0) and its
         * PFN<<2 form was never tried. This is the real wire encoding
         * (the on-wire mapper packs PFN=GPA>>14 | present into u32 PTEs). */
        {
            uint32_t w0 = rd_u32(hdr + 0x00);
            if (w0 & PVG_IOSFC_PTE_PRESENT) {
                uint64_t pl_gpa = (uint64_t)(w0 >> 2) << PVG_GUEST_PAGE_SHIFT;
                if (pl_gpa >= 0x10000 &&
                    pg_native_pte_array_ok(d, pl_gpa, page_count)) {
                    pte_src_gpa = pl_gpa;
                    pg_log(d, "resolve_pages: PTE array @0x%llx via desc-word0 "
                              "(PFN<<2|present=0x%x, %u pages)",
                           (unsigned long long)pl_gpa, w0, page_count);
                }
            }
        }
        /* (a) inline trailer, only if it fits the 0x200 slot. */
        if (!pte_src_gpa &&
            PVG_SDESC_PTE_INLINE_DEFAULT + (uint64_t)page_count * 4 <= 0x200 &&
            pg_native_pte_array_ok(d, desc_gpa + PVG_SDESC_PTE_INLINE_DEFAULT,
                                   page_count)) {
            pte_src_gpa = desc_gpa + PVG_SDESC_PTE_INLINE_DEFAULT;
        }
        /* (b) indirect: scan the descriptor for a u32/u64 that, read as a raw
         * GPA, a PFN (<<14), or a PFN<<2|present PTE, points at a coherent PTE
         * array. The PTE form ((v>>2)<<14, present bit set) is the on-wire mapper
         * encoding (same as word0 above) — try it for any word with the present
         * bit set, so a page-list root stored at a non-zero descriptor offset is
         * still found. */
        for (uint32_t off = 0; !pte_src_gpa && off + 4 <= 0x200; off += 4) {
            uint64_t v32 = rd_u32(hdr + off);
            uint64_t cands[5];
            const char *enc[5];
            int nc = 0;
            cands[nc] = v32;                            enc[nc] = "GPA";      nc++;
            cands[nc] = v32 << PVG_GUEST_PAGE_SHIFT;    enc[nc] = "PFN<<14";  nc++;
            if (v32 & PVG_IOSFC_PTE_PRESENT) {          /* PFN<<2|present PTE */
                cands[nc] = (uint64_t)(v32 >> 2) << PVG_GUEST_PAGE_SHIFT;
                enc[nc] = "PFN<<2|P"; nc++;
            }
            if ((off & 7) == 0 && off + 8 <= 0x200) {
                uint64_t v64 = rd_u64(hdr + off);
                cands[nc] = v64;                        enc[nc] = "u64GPA";   nc++;
            }
            for (int k = 0; k < nc; k++) {
                uint64_t cand = cands[k];
                if (cand < 0x10000 || (cand & 0x3)) {
                    continue;
                }
                if (pg_native_pte_array_ok(d, cand, page_count)) {
                    pte_src_gpa = cand;
                    pg_log(d, "resolve_pages: PTE array @0x%llx via desc+0x%x "
                              "(enc=%s)", (unsigned long long)cand, off, enc[k]);
                    break;
                }
            }
        }
    }
    if (!pte_src_gpa) {
        return false;
    }

    uint64_t *gpas = calloc(page_count, sizeof(uint64_t));
    if (!gpas) {
        return false;
    }
    bool contig = true;
    for (uint32_t i = 0; i < page_count; i++) {
        uint8_t pb[4];
        if (!pg_read_guest(d, pte_src_gpa + (uint64_t)i * 4, sizeof(pb), pb)) {
            free(gpas);
            return false;
        }
        uint32_t p = rd_u32(pb);
        if (!(p & PVG_IOSFC_PTE_PRESENT)) {   /* present bit only (see above) */
            free(gpas);
            return false;
        }
        gpas[i] = (uint64_t)(p >> 2) << PVG_GUEST_PAGE_SHIFT;
        if (i && gpas[i] != gpas[i - 1] + 0x4000) {
            contig = false;
        }
    }
    free(s->page_gpas);
    s->page_gpas  = gpas;
    s->page_count = page_count;
    s->contiguous = contig;
    s->base_gpa   = gpas[0];
    pg_log(d, "resolve_pages: surfaceID=%u %u pages from 0x%llx %s base=0x%llx",
           s->surface_id, page_count, (unsigned long long)pte_src_gpa,
           contig ? "(contiguous)" : "(scattered)",
           (unsigned long long)gpas[0]);
    return true;
}

/*
 * mapSurface (iosfc ring op==1): resolve surfaceID -> the present-ready surface.
 * REAL 26.5 layout (, RE'd from guest kext IOSurfaceParavirtMapperDevice,
 * live-confirmed): MMIO 0x1010 is a FLAT u32 PTE array; the per-surface geometry
 * descriptor is at  (pte[surfaceID/descsPerPage]>>2 << 14) +
 * (surfaceID % descsPerPage)*0x200.  The descriptor is GEOMETRY-ONLY (format/W/H/
 * stride; planeCount==0) and carries NO framebuffer GPA -- the pixel pages are
 * wired via the GPU task page table and surface only in the present path. So we
 * resolve geometry here and leave base_gpa pending; returning a valid surface is
 * what unblocks WindowServer's mapSurface retry loop so it proceeds to present.
 */
PGSurface *pg_native_map_surface(PGNativeDevice *d, uint32_t surface_id)
{
    if (surface_id < PVG_IOSFC_SURFACE_ID_MIN ||
        surface_id >= PVG_IOSFC_SURFACE_ID_MAX) {
        pg_log(d, "mapSurface: surfaceID %u out of range [%u,0x%x)", surface_id,
               PVG_IOSFC_SURFACE_ID_MIN, PVG_IOSFC_SURFACE_ID_MAX);
        d->stats.surface_map_errors++;
        return NULL;
    }
    if (!d->iosfc_desc_gpa) {
        pg_log(d, "mapSurface: no PTE-array base set (iosfc 0x1010)");
        d->stats.surface_map_errors++;
        return NULL;
    }

    /* (1) two-level lookup: PTE array (0x1010) -> descriptor page -> in-page slot. */
    uint32_t descs_per_page = PVG_IOSFC_DESCS_PER_PAGE_V1;
    uint32_t slot   = surface_id / descs_per_page;
    uint32_t inpage = surface_id % descs_per_page;
    uint8_t pteb[4];
    if (!pg_read_guest(d, d->iosfc_desc_gpa + (uint64_t)slot * 4,
                       sizeof(pteb), pteb)) {
        pg_log(d, "mapSurface: cannot read PTE[slot=%u] @0x%llx", slot,
               (unsigned long long)(d->iosfc_desc_gpa + (uint64_t)slot * 4));
        d->stats.surface_map_errors++;
        return NULL;
    }
    uint32_t pte = rd_u32(pteb);
    if ((pte & PVG_IOSFC_PTE_PRESENT_MASK) != PVG_IOSFC_PTE_PRESENT) {
        pg_log(d, "mapSurface: surfaceID %u PTE[slot=%u]=0x%x not present",
               surface_id, slot, pte);
        d->stats.surface_map_errors++;
        return NULL;
    }
    uint64_t desc_page = (uint64_t)(pte >> 2) << PVG_GUEST_PAGE_SHIFT;
    uint64_t desc_gpa  = desc_page + (uint64_t)inpage * PVG_IOSFC_DESC_STRIDE_V1;

    /* (2) parse the geometry-only descriptor (NO backing GPA, planeCount==0). */
    uint8_t hdr[0x200];   /* full descriptor stride; plane[0] info lives past 0x40 */
    if (!pg_read_guest(d, desc_gpa, sizeof(hdr), hdr)) {
        pg_log(d, "mapSurface: cannot read descriptor @0x%llx",
               (unsigned long long)desc_gpa);
        d->stats.surface_map_errors++;
        return NULL;
    }
    /* DIAGNOSTIC: dump the full descriptor so we can locate the plane[0]
     * pixel-page-table pointer empirically (the protocol notes §0.7.2 guesses desc+0x80+0x18
     * but it's UNCONFIRMED). Gated on APPLE_GFX_DUMP_EXEC. */
    {
        static int dd = -1;
        if (dd < 0) dd = getenv("APPLE_GFX_DUMP_EXEC") ? 1 : 0;
        if (dd && d->stats.surfaces_mapped < 6) {
            char h[0x200 * 2 + 8]; int n = 0;
            for (int i = 0; i < 0x200 && n < (int)sizeof(h) - 3; i++)
                n += snprintf(h + n, sizeof(h) - n, "%02x", hdr[i]);
            pg_log(d, "SDESC-DUMP surfaceID=%u desc_gpa=0x%llx [0:0x200]: %s",
                   surface_id, (unsigned long long)desc_gpa, h);
        }
    }
    uint32_t pixel_format = rd_u32(hdr + PVG_SDESC_PIXEL_FORMAT);
    uint32_t base_offset  = rd_u32(hdr + PVG_SDESC_BASE_OFFSET);
    uint64_t packed       = rd_u64(hdr + PVG_SDESC_PACKED_WH);
    uint64_t width        = (packed >> 8)  & 0xFFFFFF;
    uint64_t height       = (packed >> 40) & 0xFFFFFF;
    uint64_t stride       = rd_u32(hdr + PVG_SDESC_STRIDE);
    if (!width || !height || !stride) {
        pg_log(d, "mapSurface: surfaceID %u degenerate dims w=%llu h=%llu "
                  "stride=%llu (desc@0x%llx fmt=0x%x)", surface_id,
               (unsigned long long)width, (unsigned long long)height,
               (unsigned long long)stride, (unsigned long long)desc_gpa,
               pixel_format);
        d->stats.surface_map_errors++;
        return NULL;
    }

    /* (3) The geometry descriptor carries NO framebuffer GPA; the pixel pages are
     * wired via the GPU task page table and surface only in the present path.
     * Record geometry now with base_gpa pending; returning a valid surface
     * unblocks WindowServer to actually present (DisplayTransaction3). */
    uint8_t  num_planes = 1;
    uint8_t  flags      = 1;        /* read-only present source */
    uint64_t base_gpa   = 0;        /* pending: resolved from present/GPU backing */
    void    *host_va    = NULL;
    bool     contiguous = true;
    (void)base_offset;

    PGSurface *s = pg_surface_slot(d, surface_id);
    if (!s) {
        pg_log(d, "mapSurface: surface table full (surfaceID %u)", surface_id);
        d->stats.surface_map_errors++;
        return NULL;
    }
    bool was_new = !s->valid;
    free(s->page_gpas);                  /* release any prior gather vector */
    memset(s, 0, sizeof(*s));
    s->valid        = true;
    s->surface_id   = surface_id;
    s->pixel_format = pixel_format;
    s->width        = (uint32_t)width;
    s->height       = (uint32_t)height;
    s->stride       = (uint32_t)stride;
    s->base_gpa     = base_gpa;
    s->host_va      = host_va;
    s->read_only    = (flags & 1) != 0;
    s->desc_gpa     = desc_gpa;          /* proof-of-life: resolve fb GPA later */
    s->base_offset  = base_offset;
    if (was_new) {
        d->surface_count++;
    }
    d->stats.surfaces_mapped++;

    /*  native present: resolve the scattered pixel-page GPAs from the iosfc
     * PTE array now that geometry is known. Success sets s->page_gpas + s->base_gpa
     * (the present path gathers them); failure leaves base_gpa=0 for FBSCAN. */
    (void)num_planes; (void)host_va; (void)contiguous; (void)base_gpa;
    pg_native_resolve_pages(d, s, desc_gpa, hdr);

    pg_log(d, "mapSurface surfaceID=%u fmt=0x%x %ux%u stride=%u "
              "baseGPA=0x%llx pages=%u hostVA=%s",
           surface_id, pixel_format, s->width, s->height, s->stride,
           (unsigned long long)s->base_gpa, s->page_count,
           s->page_gpas ? (s->contiguous ? "gather(contig)" : "gather(scattered)")
                        : "pending");
    return s;
}

/* unmapSurface (iosfc ring op==2): drop the cached surface entry. */
static void pg_unmap_surface(PGNativeDevice *d, uint32_t surface_id)
{
    PGSurface *s = pg_native_find_surface(d, surface_id);
    if (s) {
        free(s->page_gpas);              /* release the gather vector */
        memset(s, 0, sizeof(*s));
        if (d->surface_count) {
            d->surface_count--;
        }
        d->stats.surfaces_unmapped++;
        pg_log(d, "unmapSurface surfaceID=%u", surface_id);
    }
}

/*
 * : resolve a surface's framebuffer GPA on the PRODUCTION present seam.
 * The iosfc descriptor carries geometry only (base_gpa pending); the real pixel
 * backing must be found. This is the production-seam version of the APPLE_GFX_POL
 * probe: scan guest RAM (1 MiB steps) for an image-coherent BGRA region matching
 * {stride,height} — a real framebuffer has a near-constant alpha/X byte AND color
 * variation AND multiple colors; noise/blank fail all three. Returns the best GPA,
 * or 0 if NO UI-like region exists (=> the scanout is GPU-only, no CPU framebuffer
 * — the decisive passthrough-vs-GPU-only answer). Bounded, env-gated by caller.
 */
static uint64_t pg_scan_fb_gpa(PGNativeDevice *d, const PGSurface *s)
{
    if (!d->ops || !d->ops->host_ptr || !s || !s->stride || !s->height) {
        return 0;
    }
    uint64_t fb_len = (uint64_t)s->stride * s->height;
    uint64_t best_gpa = 0;
    double best_score = 0.0, best_flat = 0, best_nz = 0;
    int best_colors = 0, logged = 0;
    for (uint64_t g = 0x70000000ull; g + fb_len <= 0x270000000ull;
         g += 0x100000ull) {
        const uint8_t *b = (const uint8_t *)d->ops->host_ptr(d->ops->opaque, g,
                                                             fb_len, true);
        if (!b) {
            continue;
        }
        uint64_t aff = 0, a00 = 0, nz = 0, tot = 0, flat = 0;
        uint32_t rbits = 0, gbits = 0, prev = 0;
        int have_prev = 0;
        for (uint32_t y = 0; y < s->height; y += 16) {
            const uint8_t *row = b + (uint64_t)y * s->stride;
            have_prev = 0;
            for (uint32_t x = 0; x + 4 <= s->stride; x += 16) {
                uint32_t px = row[x] | (row[x + 1] << 8) |
                              (row[x + 2] << 16) | ((uint32_t)row[x + 3] << 24);
                uint8_t al = row[x + 3];
                if (al == 0xff) {
                    aff++;
                } else if (al == 0x00) {
                    a00++;
                }
                if (row[x] | row[x + 1] | row[x + 2]) {
                    nz++;
                }
                tot++;
                rbits |= (1u << (row[x + 2] >> 4));
                gbits |= (1u << (row[x + 1] >> 4));
                /* flatness: adjacent samples identical => a flat run (solid UI
                 * background); noise/texture has ~zero such runs. */
                if (have_prev && px == prev) {
                    flat++;
                }
                prev = px;
                have_prev = 1;
            }
        }
        if (!tot) {
            continue;
        }
        double alpha = (double)(aff > a00 ? aff : a00) / (double)tot;
        double nzf = (double)nz / (double)tot;
        double flatf = (double)flat / (double)tot;
        int colors = __builtin_popcount(rbits) + __builtin_popcount(gbits);
        /*  dump-all-candidates: capture EVERY plausible framebuffer (incl.
         * detail-rich/non-flat = the composited desktop the flatness gate would
         * reject) to /tmp so we can find the real desktop among contiguous RAM
         * buffers. Gated by APPLE_GFX_FBDUMP. */
        if (getenv("APPLE_GFX_FBDUMP") && alpha >= 0.85 && nzf >= 0.5 &&
            colors >= 6) {
            static int ndump = 0;
            if (ndump < 16) {
                char path[80];
                snprintf(path, sizeof(path), "/tmp/fbcand_%02d_%llx.bgra",
                         ndump, (unsigned long long)g);
                FILE *fo = fopen(path, "wb");
                if (fo) {
                    fwrite(b, 1, fb_len, fo);
                    fclose(fo);
                    pg_log(d, "FBDUMP[%d] gpa=0x%llx flat=%.2f nz=%.0f%% "
                              "colors=%d alpha=%.2f", ndump,
                           (unsigned long long)g, flatf, 100.0 * nzf, colors,
                           alpha);
                    ndump++;
                }
            }
        }
        /* A real UI framebuffer: alpha-consistent + mostly non-black + multi-
         * color + FLAT (long identical runs). Noise/texture fails flatness. */
        if (alpha < 0.85 || nzf < 0.3 || colors < 4 || flatf < 0.15) {
            continue;
        }
        double score = flatf * (double)colors * nzf;
        if (logged < 12) {
            pg_log(d, "FBSCAN cand gpa=0x%llx flat=%.2f nz=%.0f%% colors=%d "
                      "alpha=%.2f score=%.2f", (unsigned long long)g, flatf,
                   100.0 * nzf, colors, alpha, score);
            logged++;
        }
        if (score > best_score) {
            best_score = score;
            best_gpa = g;
            best_flat = flatf;
            best_nz = nzf;
            best_colors = colors;
        }
    }
    if (best_gpa) {
        pg_log(d, "FBSCAN BEST %ux%u stride=%u -> fb_gpa=0x%llx flat=%.2f "
                  "nonzero=%.0f%% colors=%d", s->width, s->height, s->stride,
               (unsigned long long)best_gpa, best_flat, 100.0 * best_nz,
               best_colors);
    } else {
        pg_log(d, "FBSCAN: no UI-like (flat) BGRA region in RAM");
    }
    return best_gpa;
}

/*
 * CmdDisplayTransaction3 (0x07): the per-frame BGRA8 PRESENT. The surfaceID
 * indexes the iosfc surface table; resolve it to plane[0]'s {host VA, W, H,
 * stride, format} and hand the real pixels to the present hook. If the surface
 * was not pre-mapped via the iosfc ring we attempt a lazy mapSurface here (the
 * framework's retainSurfaceWithMappingID: also maps on demand). On a resolved
 * BGRA8 surface present_bgra8 receives the true pointer/dims/stride.
 */
static void op_display_transaction3(PGNativeDevice *d, const PGCmdHeader *hdr,
                                    const uint8_t *payload, uint32_t payload_len)
{
    if (payload_len < 0x24) {
        pg_log(d, "DisplayTransaction3: short payload (%u < 0x24)", payload_len);
        d->stats.parse_errors++;
        return;
    }
    PGDisplayTransaction3 dt;
    dt.port                    = rd_u32(payload + 0x0);
    dt.rsvd                    = rd_u32(payload + 0x4);
    dt.surfaceID               = rd_u32(payload + 0x8);
    dt.gammaTableVirtualOffset = rd_u64(payload + 0xc);
    dt.gammaTableMappedLength  = rd_u64(payload + 0x14);
    dt.gammaTableEntryCount    = rd_u32(payload + 0x1c);
    dt.gammaTableSum           = rd_u32(payload + 0x20);

    d->stats.present++;
    d->stats.last_present_surface_id = dt.surfaceID;
    d->stats.last_present_stamp = hdr->stampValue;

    pg_log(d, "CmdDisplayTransaction3 PRESENT port=%u surfaceID=%u "
              "gammaOff=0x%llx gammaLen=0x%llx gammaEntries=%u",
           dt.port, dt.surfaceID,
           (unsigned long long)dt.gammaTableVirtualOffset,
           (unsigned long long)dt.gammaTableMappedLength,
           dt.gammaTableEntryCount);

    /*
     * Resolve surfaceID -> real BGRA8 pixels via the iosfc surface table. Prefer
     * an already-mapped entry; otherwise map it on demand (retainSurfaceWith-
     * MappingID: semantics). Only when the table/descriptor region is set up
     * (a real guest) does this resolve; absent it we fall back to the marker.
     */
    PGSurface *s = pg_native_find_surface(d, dt.surfaceID);
    if (!s) {
        s = pg_native_map_surface(d, dt.surfaceID);
    }

    if (s && s->valid && d->ops && d->ops->present_bgra8) {
        const void *src = s->host_va;
        const char *path = "direct";
        /*
         *  native present. map_surface resolved the iosfc PTE array into
         * s->page_gpas (the scattered scanout pages). Three cases, in order:
         *  (1) scattered  -> gather the plane's pages into present_scratch;
         *  (2) contiguous -> zero-copy host_ptr over the resolved base_gpa;
         *  (3) unresolved -> legacy FBSCAN heuristic (resolve base_gpa once).
         * stride*height bytes of plane[0] start at base_offset into the backing.
         */
        size_t need = (size_t)s->stride * s->height;
        if (!src && s->page_gpas && s->page_count && !s->contiguous) {
            if (d->present_scratch_len < need) {
                free(d->present_scratch);
                d->present_scratch = malloc(need);
                d->present_scratch_len = d->present_scratch ? need : 0;
            }
            if (d->present_scratch) {
                uint8_t *dst = d->present_scratch;
                size_t copied = 0;
                uint32_t pi = (uint32_t)((uint64_t)s->base_offset >> 14);
                size_t page_off = s->base_offset & 0x3FFF;
                for (; pi < s->page_count && copied < need; pi++) {
                    size_t n = 0x4000 - page_off;
                    if (n > need - copied) {
                        n = need - copied;
                    }
                    void *p = d->ops->host_ptr
                            ? d->ops->host_ptr(d->ops->opaque,
                                  s->page_gpas[pi] + page_off, n, true)
                            : NULL;
                    if (p) {
                        memcpy(dst + copied, p, n);
                    } else if (d->ops->read_mem) {
                        d->ops->read_mem(d->ops->opaque,
                                  s->page_gpas[pi] + page_off, n, dst + copied);
                    } else {
                        memset(dst + copied, 0, n);
                    }
                    copied += n;
                    page_off = 0;
                }
                src = d->present_scratch;
                path = "gather";
            }
        }
        if (!src && d->ops->host_ptr) {
            if (s->base_gpa == 0) {
                static int fbscan = -1;
                if (fbscan < 0) {
                    fbscan = getenv("APPLE_GFX_FBSCAN") ? 1 : 0;
                }
                if (fbscan) {
                    s->base_gpa = pg_scan_fb_gpa(d, s);
                    path = "fbscan";
                }
            } else {
                path = s->page_gpas ? "contig" : "cached";
            }
            if (s->base_gpa) {
                src = d->ops->host_ptr(d->ops->opaque,
                                       s->base_gpa + s->base_offset,
                                       need, s->read_only);
            }
        }
        d->ops->present_bgra8(d->ops->opaque, s->width, s->height, s->stride,
                              src);
        d->stats.present_real_pixels++;
        d->stats.last_present_width  = s->width;
        d->stats.last_present_height = s->height;
        d->stats.last_present_stride = s->stride;
        d->stats.last_present_format = s->pixel_format;
        pg_log(d, "PRESENT resolved surfaceID=%u -> %ux%u stride=%u fmt=0x%x "
                  "via=%s src=%s", dt.surfaceID, s->width, s->height, s->stride,
               s->pixel_format, path, src ? "ptr" : "NULL");
    } else if (d->ops && d->ops->present_bgra8) {
        /*
         * No iosfc surface table yet (e.g. EFI-only pre-GUI path, or a unit run
         * exercising only the gfx ring): present a marker so the host/harness
         * records that the PRESENT path was reached. A real guest registers the
         * surface through the iosfc BAR first, which takes the branch above.
         */
        d->ops->present_bgra8(d->ops->opaque, 0, 0, 0, NULL);
    }

    /*
     * Retire the swap (the completion contract §4/§7). This is the wedge-clearing
     * step: signal the DISPLAY mailbox + cause-0x14 IRQ so notify_swap_done_gated
     * runs and clears [display+0x1790], unwedging swap_begin_gated. We fire BOTH
     * legs (§8): the display mailbox/0x14 cause (primary) AND the stamp/0x18 leg
     * (the display fence's stamp-slot fallback), so the kernel's swap waiter wakes
     * via whichever channel it is polling.
     */
    pg_signal_display_swap(d, dt.port);
    pg_signal_stamp(d, hdr->stampValue);
}

/* ----  op 0x43 in-process kernel walk (env APPLE_GFX_KWALK) -------------
 * Reliable kernel reads via read_kva (QEMU MMU). All addresses are
 * the unslid kernelcache; runtime = unslid + kslide, where
 * kslide = PC(at the op 0x43 doorbell write) - 0xfffffe00084de910 (the doorbell
 * fn). Find the AppleParavirtGPU IOService by BFS over the IOService plane,
 * matching node[0] (vtable) == AppleParavirtGPU obj-vtbl; then dump its fields
 * so the objectQueue->IOGPUTask[task_id]->heap-IOMD->GPA walk can be finished. */
#define KW_GREG_UNSLID   0xfffffe000a9bb550ULL  /* gRegistryRoot global (__DATA) */
#define KW_GSVC_UNSLID   0xfffffe000a97f320ULL  /* _gIOServicePlane (__DATA, same seg) */
#define KW_GPUVT_UNSLID  0xfffffe0007a8a560ULL  /* AppleParavirtGPU obj vtbl ptr (__ZTV+0x10) */
#define KW_DOORBELL_FN   0xfffffe00084de910ULL  /* kext-region slide anchor (== PC) */
#define KW_VBAR_UNSLID   0xfffffe0009c02000ULL  /* exception-vector base -> base_slide = VBAR_EL1 - this */
static inline uint64_t kw_canon(uint64_t p)
{
    return p ? ((p & 0x000000ffffffffffULL) | 0xfffffe0000000000ULL) : 0;
}
static uint64_t kw_r64(PGNativeDevice *d, uint64_t va)
{
    uint64_t v = 0;
    if (va && d->ops->read_kva && d->ops->read_kva(d->ops->opaque, va, 8, &v)) {
        return v;
    }
    return 0;
}
static uint32_t kw_r32(PGNativeDevice *d, uint64_t va)
{
    uint32_t v = 0;
    if (va && d->ops->read_kva && d->ops->read_kva(d->ops->opaque, va, 4, &v)) {
        return v;
    }
    return 0;
}
static uint64_t kw_find_apple_gpu(PGNativeDevice *d, uint64_t pc)
{
    static uint64_t q[4096];
    static uint64_t seen[4096];
    int qh = 0, qt = 0, ns = 0;
    /*
     * The base KC and the prelinked kexts are slid INDEPENDENTLY (the doorbell
     * PC gives the KEXT slide, which does NOT locate base-kernel symbols). The
     * BASE-kernel slide = VBAR_EL1 - KW_VBAR_UNSLID (the exception-vector base,
     * a base-kernel symbol whose unslid VA is fixed in the kernelcache). All of
     * gRegistryRoot, _gIOServicePlane, the IORegistryPlane childKey, and the
     * AppleParavirtGPU vtable are base-KC-resident -> use base_slide. Registry
     * NODES are heap objects (runtime addrs, no slide).
     */
    uint64_t vbar       = d->ops->cpu_reg(d->ops->opaque, 33);
    uint64_t base_slide = vbar - KW_VBAR_UNSLID;    /* __TEXT_EXEC (base) slide */
    /*
     * __DATA is a SEPARATE segment with its own slide (chained-fixup KC). Use
     * base_slide to place getRegistryRoot (a __TEXT_EXEC fn), then decode its
     * PC-relative adrp+ldr at runtime -> the __DATA gRegistryRoot global,
     * segment-agnostically. data_delta then carries the __DATA slide for the
     * other __DATA globals (_gIOServicePlane).
     */
    uint64_t getrr = 0xfffffe000a3cf93cULL + base_slide;   /* IORegistryEntry::getRegistryRoot */
    uint32_t i0 = kw_r32(d, getrr), i1 = kw_r32(d, getrr + 4);
    int64_t imm = (int64_t)((((i0 >> 5) & 0x7ffff) << 2) | ((i0 >> 29) & 0x3));
    if (imm & (1LL << 20)) {
        imm |= ~((1LL << 21) - 1);
    }
    uint64_t greg_g = (getrr & ~0xfffULL) + ((uint64_t)imm << 12)
                    + (uint64_t)((i1 >> 10) & 0xfff) * 8;
    uint64_t data_delta = greg_g - (KW_GREG_UNSLID + base_slide);
    uint64_t root  = kw_canon(kw_r64(d, greg_g));
    uint64_t plane = kw_canon(kw_r64(d, KW_GSVC_UNSLID + base_slide + data_delta));
    uint64_t ckey  = kw_canon(kw_r64(d, plane + 0x20));
    uint64_t gpuvt = kw_canon(KW_GPUVT_UNSLID + base_slide + data_delta);

    pg_log(d, "KWALK pc=0x%llx VBAR=0x%llx baseSlide=0x%llx getRRinsn=%08x/%08x "
              "gregG=0x%llx dataDelta=0x%llx root=0x%llx plane=0x%llx ckey=0x%llx "
              "gpuVT=0x%llx",
           (unsigned long long)pc, (unsigned long long)vbar,
           (unsigned long long)base_slide, i0, i1, (unsigned long long)greg_g,
           (unsigned long long)data_delta, (unsigned long long)root,
           (unsigned long long)plane, (unsigned long long)ckey,
           (unsigned long long)gpuvt);
    if (!root || !plane || !ckey) {
        return 0;
    }
    q[qt++] = root;
    while (qh < qt && qt < 4090) {
        uint64_t e = q[qh++], rt, arr, cset, carr;
        uint32_t cnt, ccnt, i;
        int dup = 0;
        for (int j = 0; j < ns; j++) {
            if (seen[j] == e) { dup = 1; break; }
        }
        if (dup) {
            continue;
        }
        if (ns < 4096) {
            seen[ns++] = e;
        }
        if (kw_canon(kw_r64(d, e)) == gpuvt) {
            pg_log(d, "KWALK FOUND AppleParavirtGPU=0x%llx (visited=%d)",
                   (unsigned long long)e, ns);
            return e;
        }
        rt = kw_canon(kw_r64(d, e + 0x18));          /* fRegistryTable */
        if (!rt) {
            continue;
        }
        cnt = kw_r32(d, rt + 0x14);
        arr = kw_canon(kw_r64(d, rt + 0x20));
        if (!arr || cnt > 4096) {
            continue;
        }
        cset = 0;
        for (i = 0; i < cnt; i++) {
            if (kw_canon(kw_r64(d, arr + i * 16)) == ckey) {
                cset = kw_canon(kw_r64(d, arr + i * 16 + 8));
                break;
            }
        }
        if (!cset) {
            continue;
        }
        ccnt = kw_r32(d, cset + 0x14);
        carr = kw_canon(kw_r64(d, cset + 0x20));
        if (!carr || ccnt > 4096) {
            continue;
        }
        for (i = 0; i < ccnt && qt < 4090; i++) {
            uint64_t c = kw_canon(kw_r64(d, carr + i * 8));
            if (c) {
                q[qt++] = c;
            }
        }
    }
    pg_log(d, "KWALK GPU NOT FOUND (visited=%d)", ns);
    return 0;
}

/* Heap-pointer heuristic: canonical kernel VA (0xfffffe..) above the static
 * kernelcache region (>256MB into the window) = a kalloc heap object. */
static inline int kw_is_heap(uint64_t x)
{
    return ((x >> 40) == 0xfffffeULL) && ((x & 0xffffffffffULL) > 0x10000000ULL);
}
/* A real C++ vtable is a run of clustered canonical code pointers. Reject
 * strings/counts (which pass the weak heap test). Reads the first 4 entries
 * (PAC-signed -> canon strips) and requires them all canonical + within 8 MB. */
static int kw_is_vtable(PGNativeDevice *d, uint64_t vt)
{
    uint64_t e[4], lo, hi;
    if (!kw_is_heap(vt)) {
        return 0;
    }
    for (int i = 0; i < 4; i++) {
        e[i] = kw_canon(kw_r64(d, vt + i * 8));
        if (!kw_is_heap(e[i])) {
            return 0;
        }
    }
    lo = hi = e[0];
    for (int i = 1; i < 4; i++) {
        if (e[i] < lo) lo = e[i];
        if (e[i] > hi) hi = e[i];
    }
    return (hi - lo) < 0x800000ULL;
}
/* SLIDE-FREE GPU-instance finder: at the op 0x43 trap the CPU registers hold
 * real (correctly-rebased) channel/queue pointers. Sweep them + their near
 * fields for the AppleParavirtGPU structural signature: getObjectQueue=[GPU+0x468]
 * is a heap object whose first word (vtable) is also canonical. Log candidates +
 * the +0x460..+0x480 region so the GPU can be identified and the task table found.
 * No slide arithmetic (dead under the chained-fixup KC). */
static void kw_sweep_gpu(PGNativeDevice *d)
{
    int found = 0;
    for (int r = 0; r <= 30 && found < 8; r++) {
        uint64_t p = kw_canon(d->ops->cpu_reg(d->ops->opaque, r));
        if (!kw_is_heap(p)) {
            continue;
        }
        for (int o = -8; o < 0x220 && found < 8; o += 8) {
            /* o==-8 sentinel means "test p itself as the GPU" */
            uint64_t g = (o < 0) ? p : kw_canon(kw_r64(d, p + (uint32_t)o));
            uint64_t oq;
            if (!kw_is_heap(g) || !kw_is_vtable(d, kw_canon(kw_r64(d, g)))) {
                continue;                       /* g must be a real C++ object */
            }
            oq = kw_canon(kw_r64(d, g + 0x468));
            if (!kw_is_heap(oq) || !kw_is_vtable(d, kw_canon(kw_r64(d, oq)))) {
                continue;                       /* objectQueue must be a real C++ object */
            }
            pg_log(d, "GPUCAND X%d%s%x gpu=0x%llx gpuVt=0x%llx objQ(+0x468)=0x%llx "
                      "objQvt=0x%llx [+0x460..+0x480]: %llx %llx %llx %llx %llx",
                   r, (o < 0) ? "(direct) " : "+0x", (o < 0) ? 0 : (unsigned)o,
                   (unsigned long long)g, (unsigned long long)kw_canon(kw_r64(d, g)),
                   (unsigned long long)oq,
                   (unsigned long long)kw_canon(kw_r64(d, oq)),
                   (unsigned long long)kw_r64(d, g + 0x460),
                   (unsigned long long)kw_r64(d, g + 0x468),
                   (unsigned long long)kw_r64(d, g + 0x470),
                   (unsigned long long)kw_r64(d, g + 0x478),
                   (unsigned long long)kw_r64(d, g + 0x480));
            found++;
        }
    }
    if (!found) {
        pg_log(d, "GPUCAND none found via register sweep / 2-hop");
    }
}

/* ExecIndirect2/3 (0x37/0x43): the Metal draw stream. Accel-only; stub+ack. */
static void op_exec_indirect(PGNativeDevice *d, const PGCmdHeader *hdr,
                             const uint8_t *payload, uint32_t payload_len)
{
    uint32_t task_id = (payload_len >= 4) ? rd_u32(payload + 0) : 0;
    d->stats.exec_indirect++;
    /*
     * PHASE-1 SPIKE (env APPLE_GFX_TRANS) — the decisive go/no-go probe.
     * Present a host-owned BGRA backbuffer FROM the real op 0x43 seam:
     *   LEFT half  = a constant gradient  -> proves the exec->present->VNC wire
     *                works on the production seam (not just the vblank test-pattern).
     *   RIGHT half = a heatmap of the 44B payload + 256B@heap_gpa -> if it (and the
     *                logged heap checksum) is STATIC across screendumps, the bytes we
     *                can address are NOT the live stream (heap VA 0x104000 = code),
     *                which redirects all effort to the runtime GPU-instance anchor.
     * base_gpa is intentionally NOT needed: we present OUR buffer, not the guest's.
     * Verify ONLY by a VNC frame-grab against -display vnc.
     */
    {
        static int tr = -1;
        if (tr < 0) {
            tr = getenv("APPLE_GFX_TRANS") ? 1 : 0;
        }
        if (tr && d->ops && d->ops->present_bgra8) {
            static uint8_t trbuf[1920 * 1080 * 4];
            uint32_t W = 1280, H = 800, S = 1280 * 4;
            for (uint32_t i = 0; i < PVG_MAX_SURFACES; i++) {
                if (d->surfaces[i].valid && d->surfaces[i].width >= 640 &&
                    d->surfaces[i].height >= 480 && d->surfaces[i].stride) {
                    W = d->surfaces[i].width;
                    H = d->surfaces[i].height;
                    S = d->surfaces[i].stride;
                    break;
                }
            }
            if ((uint64_t)H * S <= sizeof(trbuf) && W && H) {
                uint8_t hb[256];
                int have_hb = 0;
                uint32_t hcsum = 0;
                PGTask *t = pg_native_find_task(d, task_id);
                if (t && t->heap_bound) {
                    have_hb = pg_read_guest(d, t->heap_gpa, sizeof(hb), hb);
                }
                if (have_hb) {
                    for (int i = 0; i < 256; i++) {
                        hcsum = (hcsum * 31u) + hb[i];
                    }
                }
                for (uint32_t y = 0; y < H; y++) {
                    uint8_t *rowp = trbuf + (uint64_t)y * S;
                    for (uint32_t x = 0; x < W; x++) {
                        uint8_t *px = rowp + (uint64_t)x * 4;
                        if (x < W / 2) {                  /* LEFT: wire proof */
                            px[0] = (uint8_t)(x * 255 / (W / 2));   /* B */
                            px[1] = (uint8_t)(y * 255 / H);         /* G */
                            px[2] = 0x40;                           /* R */
                        } else {                          /* RIGHT: byte heatmap */
                            uint8_t pb = payload[(x >> 3) % payload_len];
                            uint8_t hv = have_hb ? hb[(y >> 1) & 0xff] : 0;
                            px[0] = hv;                             /* B = heap byte */
                            px[1] = pb;                             /* G = payload byte */
                            px[2] = have_hb ? 0x00 : 0x80;          /* R: flag no-heap */
                        }
                        px[3] = 0xff;                               /* X */
                    }
                }
                d->ops->present_bgra8(d->ops->opaque, W, H, S, trbuf);
                pg_log(d, "TRANS present #%u task=%u %ux%u stride=%u heap_ok=%d "
                          "heapCsum=0x%x pl0-3=%02x%02x%02x%02x "
                          "(LEFT gradient=wire proof, RIGHT=payload/heap heatmap)",
                       d->stats.exec_indirect, task_id, W, H, S, have_hb, hcsum,
                       payload_len > 0 ? payload[0] : 0,
                       payload_len > 1 ? payload[1] : 0,
                       payload_len > 2 ? payload[2] : 0,
                       payload_len > 3 ? payload[3] : 0);
            }
        }
    }
    /* INSTRUMENTATION: dump the full payload + the referenced task heap
     * head so we can decode the composite command stream (is it CPU-executable
     * 2D blits, or opaque Metal-3D?). Toggle via env APPLE_GFX_DUMP_EXEC. */
    {
        static int dump = -1;
        if (dump < 0) dump = getenv("APPLE_GFX_DUMP_EXEC") ? 1 : 0;
        if (dump && d->stats.exec_indirect <= 40) {
            char hex[520]; int n = 0;
            uint32_t dl = payload_len < 256 ? payload_len : 256;
            for (uint32_t i = 0; i < dl && n < (int)sizeof(hex) - 3; i++)
                n += snprintf(hex + n, sizeof(hex) - n, "%02x", payload[i]);
            pg_log(d, "EXECIND-PAYLOAD op=0x%x taskID=%u len=%u: %s",
                   hdr->cmdID, task_id, payload_len, hex);
            PGTask *t = pg_native_find_task(d, task_id);
            if (t && t->heap_bound) {
                uint8_t buf[256];
                if (pg_read_guest(d, t->heap_gpa, sizeof(buf), buf)) {
                    char hh[600]; int m = 0;
                    for (int i = 0; i < 256 && m < (int)sizeof(hh) - 3; i++)
                        m += snprintf(hh + m, sizeof(hh) - m, "%02x", buf[i]);
                    pg_log(d, "EXECIND-HEAP taskID=%u heapGPA=0x%llx[0:256]: %s",
                           task_id, (unsigned long long)t->heap_gpa, hh);
                }
            }
        }
    }
    /* capture the REAL inner Metal _stream. The ExecIndirect
     * header's TRAILING u64s are {innerStreamGPA, innerStreamLen}; the prior dumpers
     * read the hardcoded heap code-page (0x104000) instead. Read the real stream via
     * pg_read_guest and log it chunked as EXECIND-STREAM (256B/line) so it can be
     * lifted straight into a stream-walk reference script. env APPLE_GFX_STREAM. Read-only. */
    {
        static int strm = -1;
        if (strm < 0) strm = getenv("APPLE_GFX_STREAM") ? 1 : 0;
        if (strm && d->stats.exec_indirect <= 16 && payload_len >= 0x10) {
            uint64_t inner_va = rd_u64(payload + payload_len - 0x10);
            uint64_t inner_len = rd_u64(payload + payload_len - 0x08);
            uint64_t cap = inner_len > 4096 ? 4096 : inner_len;
            PGTask *t = pg_native_find_task(d, task_id);
            /* Full diagnostic: header + task ranges + every candidate location's
             * first 64 bytes (with non-zero count) + full dump of the richest one.
             * The trailing-u64 inner_va resolved readable-but-zero via ram+va, so we
             * need to see which address actually holds the Metal _stream. */
            { char hx[600]; int m = 0; uint32_t dl = payload_len < 256 ? payload_len : 256;
              for (uint32_t i = 0; i < dl; i++) m += snprintf(hx + m, sizeof(hx) - m, "%02x", payload[i]);
              pg_log(d, "EXECIND-HDR op=0x%x task=%u plen=%u inner_va=0x%llx inner_len=%llu: %s",
                     hdr->cmdID, task_id, payload_len,
                     (unsigned long long)inner_va, (unsigned long long)inner_len, hx); }
            if (t) {
              pg_log(d, "EXECIND-TASK task=%u root=0x%llx ranges=%u heap=0x%llx",
                     task_id, (unsigned long long)t->task_root, t->range_count,
                     (unsigned long long)t->heap_gpa);
              for (uint32_t i = 0; i < t->range_count && i < 8; i++)
                pg_log(d, "  range[%u] va=0x%llx phys=0x%llx len=0x%llx", i,
                       (unsigned long long)t->ranges[i].virtual_offset,
                       (unsigned long long)t->ranges[i].phys_addr,
                       (unsigned long long)t->ranges[i].length);
            }
            uint64_t cg[6]; const char *cnm[6]; int nc = 0; uint64_t rg;
            if (t && pg_native_task_resolve(t, inner_va, &rg)) { cg[nc] = rg; cnm[nc++] = "task"; }
            cg[nc] = 0x70000000ull + inner_va; cnm[nc++] = "ram+va";
            if (t) { cg[nc] = t->task_root + inner_va; cnm[nc++] = "root+va"; }
            if (t) { cg[nc] = t->task_root; cnm[nc++] = "taskroot"; }
            if (t && t->range_count) { cg[nc] = t->ranges[0].phys_addr; cnm[nc++] = "range0"; }
            int best = -1, bestnz = 0; uint8_t cb[64];
            for (int c = 0; c < nc; c++) {
                if (cg[c] && pg_read_guest(d, cg[c], 64, cb)) {
                    char hx[140]; int m = 0, nz = 0;
                    for (int i = 0; i < 64; i++) { m += snprintf(hx + m, sizeof(hx) - m, "%02x", cb[i]); if (cb[i]) nz++; }
                    pg_log(d, "EXECIND-CAND %s gpa=0x%llx nz=%d: %s", cnm[c], (unsigned long long)cg[c], nz, hx);
                    if (nz > bestnz) { bestnz = nz; best = c; }
                } else {
                    pg_log(d, "EXECIND-CAND %s gpa=0x%llx UNREADABLE", cnm[c], (unsigned long long)cg[c]);
                }
            }
            if (best >= 0 && cap) {
                uint8_t sb[4096];
                if (pg_read_guest(d, cg[best], cap, sb)) {
                    for (uint64_t off = 0; off < cap; off += 256) {
                        uint64_t n = (cap - off < 256) ? cap - off : 256;
                        char hx[520]; int m = 0;
                        for (uint64_t i = 0; i < n; i++)
                            m += snprintf(hx + m, sizeof(hx) - m, "%02x", sb[off + i]);
                        pg_log(d, "EXECIND-STREAM %s +0x%llx: %s", cnm[best], (unsigned long long)off, hx);
                    }
                }
            }
        }
    }
    /*
     *  KERNEL-WALK probe (env APPLE_GFX_KWALK): dump the triggering CPU's
     * registers (the anchor for the op 0x43 kernel-object walk + the KASLR slide
     * from PC/LR vs a known kext callsite) and verify read_kva (robust VA->GPA
     * via QEMU's MMU) works on kernel text. This bootstraps reading the real op
     * 0x43 command buffer in-process without the unreliable gdb-stub. Bounded.
     */
    {
        static int kw = -1;
        if (kw < 0) {
            kw = getenv("APPLE_GFX_KWALK") ? 1 : 0;
        }
        if (kw && d->stats.exec_indirect <= 3 && d->ops && d->ops->cpu_reg) {
            uint64_t pc = d->ops->cpu_reg(d->ops->opaque, 31);
            uint64_t lr = d->ops->cpu_reg(d->ops->opaque, 30);
            uint64_t sp = d->ops->cpu_reg(d->ops->opaque, 32);
            pg_log(d, "KWALK exec#%u taskID=%u PC=0x%llx LR=0x%llx SP=0x%llx",
                   d->stats.exec_indirect, task_id,
                   (unsigned long long)pc, (unsigned long long)lr,
                   (unsigned long long)sp);
            for (int i = 0; i < 31; i += 2) {
                if (i + 1 < 31) {
                    pg_log(d, "KWALK X%02d=0x%llx X%02d=0x%llx", i,
                           (unsigned long long)d->ops->cpu_reg(d->ops->opaque, i),
                           i + 1,
                           (unsigned long long)d->ops->cpu_reg(d->ops->opaque,
                                                              i + 1));
                } else {
                    pg_log(d, "KWALK X%02d=0x%llx", i,
                           (unsigned long long)d->ops->cpu_reg(d->ops->opaque, i));
                }
            }
            if (d->ops->read_kva) {
                /* PRIMARY (slide-free): sweep the live trap registers for the
                 * AppleParavirtGPU structural signature. The chained-fixup KC
                 * defeats slide arithmetic, so the BFS below (kw_find_apple_gpu)
                 * is kept only as a fallback diagnostic. */
                kw_sweep_gpu(d);
                uint64_t gpu = kw_find_apple_gpu(d, pc);
                if (gpu) {
                    uint64_t oq = kw_canon(kw_r64(d, gpu + 0x460));
                    pg_log(d, "KWALK GPU+0x460(objQ)=0x%llx +0x470=0x%llx "
                              "+0x478=0x%llx",
                           (unsigned long long)oq,
                           (unsigned long long)kw_canon(kw_r64(d, gpu + 0x470)),
                           (unsigned long long)kw_canon(kw_r64(d, gpu + 0x478)));
                    for (uint64_t off = 0x400; off < 0x500; off += 0x20) {
                        pg_log(d, "KWALK GPU+0x%llx: %016llx %016llx %016llx "
                                  "%016llx", (unsigned long long)off,
                               (unsigned long long)kw_r64(d, gpu + off),
                               (unsigned long long)kw_r64(d, gpu + off + 8),
                               (unsigned long long)kw_r64(d, gpu + off + 0x10),
                               (unsigned long long)kw_r64(d, gpu + off + 0x18));
                    }
                    if (oq) {
                        for (uint64_t off = 0; off < 0x80; off += 0x20) {
                            pg_log(d, "KWALK objQ+0x%llx: %016llx %016llx "
                                      "%016llx %016llx", (unsigned long long)off,
                                   (unsigned long long)kw_r64(d, oq + off),
                                   (unsigned long long)kw_r64(d, oq + off + 8),
                                   (unsigned long long)kw_r64(d, oq + off + 0x10),
                                   (unsigned long long)kw_r64(d, oq + off + 0x18));
                        }
                    }
                }
            }
        }
    }
    /*
     * VA->GPA PROBE (2026-06-03, env APPLE_GFX_VAPROBE): the heap "GPA" recorded
     * from CmdSetObjAndPlace (0x44) is actually a GPU-task VIRTUAL address (e.g.
     * 0x104000, BELOW guest RAM base 0x70000000) — reading it as a raw GPA yields
     * low-mem ARM64 code, NOT the Metal stream. The framework path maps the whole
     * GPU address space as ONE range based at RAM_BASE with hostVA prefilled
     * (apple-gfx-mmio.m), i.e. GPU addresses are RAM-base-relative. Test that:
     * for the referenced task's heap VA, dump 128B at each candidate GPA
     * interpretation + a code/stream classifier, so we can locate the real stream
     * and fix mapSurface/exec resolution. Bounded to first few execs.
     */
    {
        static int vp = -1;
        if (vp < 0) vp = getenv("APPLE_GFX_VAPROBE") ? 1 : 0;
        if (vp && d->stats.exec_indirect <= 6) {
            PGTask *t = pg_native_find_task(d, task_id);
            uint64_t heap_va = t ? t->heap_gpa : 0;       /* misnamed: it's a VA */
            uint64_t cands[3] = {
                heap_va,                                  /* raw VA-as-GPA (= code) */
                0x70000000ull + heap_va,                  /* RAM-base-relative */
                t ? (t->task_root + heap_va) : 0,         /* task-root-relative */
            };
            const char *names[3] = {"raw", "ram+va", "root+va"};
            for (int c = 0; c < 3; c++) {
                if (!cands[c]) continue;
                uint8_t b[128];
                if (!pg_read_guest(d, cands[c], sizeof(b), b)) {
                    pg_log(d, "VAPROBE task=%u %s gpa=0x%llx UNREADABLE",
                           task_id, names[c], (unsigned long long)cands[c]);
                    continue;
                }
                /* classify: ARM64 code has ret(c0035fd6)/pacibsp(7f2303d5) and few
                 * zero runs; a command stream is mostly small structured words. */
                int zeros = 0, code_sig = 0, smallw = 0;
                for (int i = 0; i < 128; i++) if (b[i] == 0) zeros++;
                for (int i = 0; i + 4 <= 128; i += 4) {
                    if (b[i]==0xc0&&b[i+1]==0x03&&b[i+2]==0x5f&&b[i+3]==0xd6) code_sig++;
                    if (b[i]==0x7f&&b[i+1]==0x23&&b[i+2]==0x03&&b[i+3]==0xd5) code_sig++;
                    uint32_t w = rd_u32(b + i);
                    if (w != 0 && w < 0x1000) smallw++;   /* opcode/len-like */
                }
                char hh[2 * 128 + 4]; int m = 0;
                for (int i = 0; i < 128 && m < (int)sizeof(hh) - 3; i++)
                    m += snprintf(hh + m, sizeof(hh) - m, "%02x", b[i]);
                pg_log(d, "VAPROBE task=%u %s gpa=0x%llx zeros=%d codeSig=%d "
                          "smallW=%d verdict=%s [0:128]: %s",
                       task_id, names[c], (unsigned long long)cands[c], zeros,
                       code_sig, smallw, code_sig ? "CODE" :
                       (smallw >= 8 ? "STREAM?" : "?"), hh);
            }
        }
    }
    /*
     * VK C-BRIDGE (env APPLE_GFX_VK_BRIDGE) — drive the LIVE guest's op-0x37
     * Metal stream through the software-Vulkan (lavapipe) translator instead of
     * the immediate-stamp stub. This is the live counterpart of the offline
     * apple-gfx-vk-replay harness: the translator (apple-gfx-vk.c) renders the
     * recognized textured-quad composite and presents it via the host
     * present_bgra8 sink, then signals the completion stamp from its own timeline
     * thread (REAL software-GPU completion driving the guest fence wait).
     *
     * The native memory accessor is ALREADY bridged to the translator: at
     * pg_native_init() the device passes d->ops (its PGNativeHostOps with the real
     * QEMU host_ptr/read_mem) into pg_vk_ops()->init(), which the translator stores
     * as c->host. pg_vk_resolve_inner() then resolves the op-0x37 residency
     * header's innerStreamGPA via c->host->host_ptr()/read_mem() — i.e. the live
     * guest RAM — with no extra wiring here. We just hand it the op-0x37 payload
     * (task_id + header→{innerStreamGPA,innerStreamLen} + dst objectID, all parsed
     * translator-side from `payload`) and the completion fence (hdr->stampValue).
     *
     * Contract (apple-gfx-vk.h): exec_indirect returns the timeline value the
     * submission will signal at (nonzero) when it took the work — completion drives
     * the stamp, so we RETURN here and skip the stub stamp + POL/FBSCAN fallback.
     * It returns 0 when it did NOT submit (Vulkan absent, translator gated off via
     * APPLE_GFX_VK_EXEC, inner stream unresolved, or not a recognizable composite);
     * we then fall through to the existing immediate-stamp stub (default behavior
     * unchanged unless APPLE_GFX_VK_BRIDGE is set AND a composite was recognized).
     */
    {
        static int vkb = -1;
        if (vkb < 0) {
            vkb = getenv("APPLE_GFX_VK_BRIDGE") ? 1 : 0;
        }
        if (vkb && pg_vk_ops() && d->vk_ctx) {
            uint64_t tv = pg_vk_ops()->exec_indirect(d->vk_ctx, task_id, payload,
                                                     payload_len, hdr->stampValue);
            if (tv) {
                pg_log(d, "VK-BRIDGE exec#%u op=0x%x taskID=%u payload=%u -> "
                          "translator submitted (timeline=%llu); completion drives "
                          "stamp 0x%x", d->stats.exec_indirect, hdr->cmdID, task_id,
                       payload_len, (unsigned long long)tv, hdr->stampValue);
                return;   /* software-GPU completion will pg_signal_stamp + present */
            }
            pg_log(d, "VK-BRIDGE exec#%u op=0x%x taskID=%u payload=%u -> translator "
                      "fell back (timeline=0); using immediate-stamp stub",
                   d->stats.exec_indirect, hdr->cmdID, task_id, payload_len);
        }
    }

    pg_log(d, "Cmd%s op=0x%x taskID=%u payload=%u bytes -- STUB "
              "(Metal stream = accel-only TODO, OPAQUE #2)",
           hdr->cmdID == PVG_OP_CmdExecIndirect3 ? "ExecIndirect3"
                                                 : "ExecIndirect2",
           hdr->cmdID, task_id, payload_len);
    /* Ack the stamp so the guest's fence wait completes. */
    pg_signal_stamp(d, hdr->stampValue);

    /*
     * PROOF-OF-LIFE (env APPLE_GFX_POL): the Metal exec stream above is stubbed,
     * so WindowServer never emits op 0x07 (present). Synthesize one: find the
     * scanout surface, locate its framebuffer GPA by scanning the task root page
     * for page-aligned GPA entries holding non-zero BGRA content, and present the
     * best candidate. This answers whether the scanout is CPU-populated (-> first
     * pixels) or GPU-only (-> blank). Bounded to the first execs (cost/log spam).
     */
    {
        static int pol = -1;
        if (pol < 0) {
            pol = getenv("APPLE_GFX_POL") ? 1 : 0;
        }
        if (pol && d->stats.exec_indirect <= 8 && d->ops && d->ops->host_ptr &&
            d->ops->present_bgra8 && d->root_page_gpa) {
            PGSurface *sc = NULL;
            for (uint32_t i = 0; i < PVG_MAX_SURFACES; i++) {
                if (d->surfaces[i].valid && d->surfaces[i].width >= 640 &&
                    d->surfaces[i].height >= 480 && d->surfaces[i].stride) {
                    sc = &d->surfaces[i];
                    break;
                }
            }
            static int pol_done;
            uint8_t rp[0x400];
            if (sc && !pol_done && (d->stats.exec_indirect % 2 == 0)) {
                uint64_t fb_len = (uint64_t)sc->stride * sc->height;
                /* dump the task root page so the range table can be RE'd offline */
                if (pg_read_guest(d, d->root_page_gpa, sizeof(rp), rp)) {
                    char h[0x400 * 2 + 8]; int n = 0;
                    for (int i = 0; i < 0x400 && n < (int)sizeof(h) - 3; i++) {
                        n += snprintf(h + n, sizeof(h) - n, "%02x", rp[i]);
                    }
                    pg_log(d, "POL rootpage@0x%llx[0:0x400]: %s",
                           (unsigned long long)d->root_page_gpa, h);
                }
                /*
                 * Broad scan of guest RAM (1 MiB steps) for an image-coherent BGRA
                 * region: a real framebuffer has a near-constant alpha/X byte
                 * (offset 3 of each pixel) AND color variation; noise has neither,
                 * a blank region has the constant byte but no variation.
                 */
                uint64_t best_gpa = 0;
                double best_score = 0.0, best_alpha = 0, best_nz = 0;
                int best_colors = 0;
                for (uint64_t g = 0x70000000ull; g + fb_len <= 0x270000000ull;
                     g += 0x100000ull) {
                    const uint8_t *b = (const uint8_t *)d->ops->host_ptr(
                        d->ops->opaque, g, fb_len, true);
                    if (!b) {
                        continue;
                    }
                    uint64_t aff = 0, a00 = 0, nz = 0, tot = 0;
                    uint32_t rbits = 0;
                    for (uint32_t y = 0; y < sc->height; y += 16) {
                        const uint8_t *row = b + (uint64_t)y * sc->stride;
                        for (uint32_t x = 0; x + 4 <= sc->stride; x += 64) {
                            uint8_t al = row[x + 3];
                            if (al == 0xff) {
                                aff++;
                            } else if (al == 0x00) {
                                a00++;
                            }
                            if (row[x] | row[x + 1] | row[x + 2]) {
                                nz++;
                            }
                            tot++;
                            rbits |= (1u << (row[x + 2] >> 4));
                        }
                    }
                    if (!tot) {
                        continue;
                    }
                    double alpha = (double)(aff > a00 ? aff : a00) / (double)tot;
                    double nzf = (double)nz / (double)tot;
                    int colors = __builtin_popcount(rbits);
                    /* real light-background UI: alpha-consistent, mostly non-black,
                     * and color-varied. score rewards all three; reject near-black
                     * (nzf < 0.4) and monochrome (colors < 5). */
                    if (alpha < 0.9 || nzf < 0.4 || colors < 5) {
                        continue;
                    }
                    double score = alpha * nzf * (double)colors;
                    pg_log(d, "POL cand fb_gpa=0x%llx alpha=%.3f nonzero=%.1f%% "
                              "colors=%d", (unsigned long long)g, alpha,
                           100.0 * nzf, colors);
                    if (score > best_score) {
                        best_score = score;
                        best_gpa = g;
                        best_alpha = alpha;
                        best_nz = nzf;
                        best_colors = colors;
                    }
                }
                if (best_gpa) {
                    const void *src = d->ops->host_ptr(d->ops->opaque, best_gpa,
                                                       fb_len, true);
                    d->ops->present_bgra8(d->ops->opaque, sc->width, sc->height,
                                          sc->stride, src);
                    pg_log(d, "POL PRESENT scanout=%u %ux%u stride=%u fb_gpa=0x%llx "
                              "alpha=%.3f nonzero=%.1f%% colors=%d", sc->surface_id,
                           sc->width, sc->height, sc->stride,
                           (unsigned long long)best_gpa, best_alpha,
                           100.0 * best_nz, best_colors);
                    pol_done = 1;
                } else {
                    static int misses;
                    pg_log(d, "POL no UI-like BGRA region in RAM at exec #%u "
                              "(try %d)", d->stats.exec_indirect, ++misses);
                    if (misses >= 12) {
                        pol_done = 1;
                        pg_log(d, "POL giving up after %d scans -> scanout is "
                                  "GPU-only (no CPU framebuffer)", misses);
                    }
                }
            }
        }
    }
}

/* CmdGetDeviceInfo (0x3a): write the device-info TLV response, then ack.
 *
 * The guest's AppleParavirtGPU::setupDeviceInfo submits this command with a
 * 12-byte body {version=0x2d; length_qwords; buffer_pfn} and then synchronously
 * calls parseDeviceInfo() on the buffer the device is expected to fill. If the
 * buffer is left all-zero (the old "unhandled" path), parseDeviceInfo yields an
 * empty APVDeviceInfoStruct (no present flags, all capability fields 0); the GPU
 * treats that as a broken/incapable device and tears itself down + restarts in a
 * loop (observed live: 34x ring re-creation, no display ever created, no present
 * ever submitted). Filling a full, valid TLV table here makes the device report a
 * capable GPU so bring-up proceeds to AppleParavirtDisplay and the present path.
 *
 * Response wire format (parseDeviceInfo, KDK 26.5): a flat array of 8-byte TLV
 * entries {u32 type; u32 value}. For type T in 1..0x2c the parser stores `value`
 * at APVDeviceInfoStruct+(T-1)*4 and sets the present-flag byte at 0xac+(T-1). We
 * emit every field 1..0x2c with value 1 (present + non-zero) except the Metal
 * shader-version field (type 0x12) which parseDeviceInfo range-checks against
 * 0x20007 -- we report 0x20007 so it is accepted as-is rather than defaulted.
 */
static void op_get_device_info(PGNativeDevice *d, const PGCmdHeader *hdr,
                               const uint8_t *payload, uint32_t payload_len)
{
    if (payload_len < 0xc) {
        pg_log(d, "GetDeviceInfo: short payload (%u < 0xc)", payload_len);
        d->stats.parse_errors++;
        pg_signal_stamp(d, hdr->stampValue);
        return;
    }
    uint32_t version = rd_u32(payload + 0x0);
    uint32_t len_qw  = rd_u32(payload + 0x4);  /* response size in 8-byte units */
    uint32_t buf_pfn = rd_u32(payload + 0x8);

    /* Latch the device-info version (0x2d=26.5, 0xe=Monterey) for ver-gated
     * protocol quirks downstream (GetDeviceInfo runs before the display op 0x9). */
    d->devinfo_version = version;
    uint64_t buf_gpa = (uint64_t)buf_pfn << PVG_GUEST_PAGE_SHIFT;

    /* How many TLV entries fit in the buffer the guest provided. We only need to
     * publish fields 1..0x2c, but never exceed the buffer (length_qwords). */
    uint32_t max_entries = len_qw ? len_qw : PVG_DEVINFO_FIELD_COUNT;
    uint32_t n_entries = PVG_DEVINFO_FIELD_COUNT;
    if (n_entries > max_entries) {
        n_entries = max_entries;
    }
    uint64_t resp_bytes = (uint64_t)n_entries * PVG_DEVINFO_TLV_STRIDE;

    uint8_t *resp = NULL;
    if (buf_gpa && d->ops && d->ops->host_ptr) {
        resp = (uint8_t *)d->ops->host_ptr(d->ops->opaque, buf_gpa, resp_bytes,
                                           false /* writable */);
    }
    if (!resp) {
        pg_log(d, "GetDeviceInfo: response buf GPA=0x%llx (pfn=0x%x) not "
                  "host-writable; device info NOT published (IRQ only)",
               (unsigned long long)buf_gpa, buf_pfn);
        pg_signal_stamp(d, hdr->stampValue);
        return;
    }

    /* Emit one {type, value} per field 1..0x2c. parseDeviceInfo dispatches by
     * type, so any ordering works; we use ascending type == buffer index.
     *
     *  (Phase 1, Metal->Vulkan milestone): the old code emitted value=1
     * for EVERY field. That is sufficient for the kernel display path (proven), but
     * the userspace Metal plugin (AppleParavirtGPUMetal) reads these same fields via
     * AppleParavirtDeviceUserClient::s_getDeviceInfo into its `_deviceInfo` struct and
     * REJECTS the device when the capability values are nonsensical (e.g.
     * MaxThreadsPerThreadgroup=1, GpuCoreCount=1) -> MTLCopyAllDevices returns empty
     * -> WindowServer aborts (the GUI wall). type N maps to the Nth field of the
     * APVDeviceInfoStruct (confirmed: parseDeviceInfo stores value at off (N-1)*4,
     * present byte at 0xac+(N-1); type 0x12 == MaxMetalShaderVersion at off 0x44).
     *
     * Values below: pass-through limits are GROUND TRUTH from this host's real Metal
     * device (Apple M2, apple8 family) queried via MTLDevice; PVG-internal fields
     * (DeserializerVersion, SupportFlags20xx, HostGPUFamily*) are informed estimates
     * to be refined empirically. Field-name comments are from the AppleParavirtGPUMetal
     * `_deviceInfo` objc struct layout. */
    static const uint32_t devinfo_values[PVG_DEVINFO_FIELD_COUNT + 1] = {
        [0]  = 0,            /* unused (types are 1-based)                          */
        [1]  = 4,            /* MSAASamples (MAX supported sample count; M2={1,2,4}) */
        [2]  = 1,            /* D24S8Supported                                      */
        [3]  = 1024,         /* MaxThreadsPerThreadgroupW (M2: 1024)                */
        [4]  = 1024,         /* MaxThreadsPerThreadgroupH (M2: 1024)                */
        [5]  = 1024,         /* MaxThreadsPerThreadgroupD (M2: 1024)                */
        [6]  = 32768,        /* MaxThreadgroupMemoryLength (M2: 32 KiB)             */
        [7]  = 1,            /* IsFramebufferReadSupported                          */
        [8]  = 1,            /* IsRGB10A2GammaSupported                             */
        [9]  = 1,            /* SupportsNativeHardwareFP16                          */
        [10] = 8,            /* DeserializerVersion (PGDeserializerVersion, GT)      */
        [11] = 0x1ff,        /* PrimtiveTypeSupport (GT)                             */
        [12] = 1,            /* SupportsMultiplaneTextures                          */
        [13] = 16,           /* LinearTextureAlignment (GT)                         */
        [14] = 1,            /* HeapBuffers                                         */
        [15] = 16,           /* HeapBufferAlignment                                 */
        [16] = 1,            /* HeapTextures                                        */
        [17] = 1,            /* BufferFromIOSurface                                 */
        [18] = 0x20007,      /* MaxMetalShaderVersion (fixupMetalShaderVersion ok)  */
        [19] = 1,            /* SupportsSharedTextures                              */
        [20] = 2,            /* MaxVertexAmplificationCount                         */
        [21] = 1,            /* SupportsProgrammableSamplePositions                 */
        [22] = 2,            /* RasterizationRateLayerCount (GT)                    */
        [23] = 1,            /* TileShaders                                         */
        [24] = 1,            /* ImageBlocks                                         */
        [25] = 1,            /* RasterOrderGroups                                   */
        [26] = 1,            /* MemoryOrderAtomics                                  */
        [27] = 1,            /* LargeMRT                                            */
        [28] = 0x7,          /* SupportFlags2023 (Apple5|DynAttribStride|2DMSArray) */
        [29] = 1024,         /* MaxTotalComputeThreadsPerThreadgroup               */
        [30] = 32768,        /* MaxComputeLocalMemorySizes                          */
        [31] = 32768,        /* MaxComputeThreadgroupMemory                         */
        [32] = 16,           /* MaxComputeThreadgroupMemoryAlignmentBytes           */
        [33] = 0xdff,        /* SupportFlags2024 (GT; bit9 cond-load-store off)     */
        [34] = 10,           /* GpuCoreCount (this host: 10-core M2, GT)            */
        [35] = 2048,         /* MaxTextureLayers                                    */
        [36] = 1,            /* MaxPredicatedNestingDepth                           */
        [37] = 1008,         /* HostGPUFamilyClamped (raw MTLGPUFamily apple8, GT)  */
        [38] = 2,            /* ArgumentBuffersTier (M2: tier 2)                    */
        [39] = 1024,         /* ArgumentBuffersMaxSamplerCount                      */
        [40] = 16,           /* MinimumLinearTextureAlignment (GT)                  */
        [41] = 0x7,          /* SupportedTextureWriteRoundingModes (GT)             */
        [42] = 0x3,          /* SupportFlags2025 (GT; both bits = argbuffers)        */
        [43] = 0xf,          /* HostGPUFamilies (supportsFamily 5,6,7,8)            */
        [44] = 1,            /* (trailing field; benign default)                    */
    };
    /* Force-SW test: APPLE_GFX_NO_METAL=1 emits value=1 for every
     * field, which the userspace Metal plugin rejects (MTLCopyAllDevices empty) so
     * WindowServer takes activate_compositor_sw (CompositorSW, CPU composite) instead
     * of Metal compositing. Resolves whether the 26.5 guest SW-composites (-> present
     * -> GUI) or actually requires functional Metal. */
    static int no_metal = -1;
    if (no_metal < 0) {
        no_metal = getenv("APPLE_GFX_NO_METAL") ? 1 : 0;
    }
    /* Monterey (PVG ver=0xe) uses a DIFFERENT device-info format than 26.5
     * (ver=0x2d). RE of Monterey's parseDeviceInfo (AppleParavirtGPUIOGPUFamily,
     * the Monterey kernelcache @0x...306b90): it accepts types 0..0xd (14 fields,
     * 0-BASED; cmp #0xd / b.hi skips >0xd), storing value at struct+type*4 and the
     * present byte at struct+0x34+type. The 26.5 path below emits types 1..0x2c,
     * which Monterey mis-parses: type 0 is NEVER set (its mandatory field 0 stays
     * absent) -> GPU-init wedge after createDisplayAttributes (the gfx-IRQ storm).
     * For ver<0x20 emit the 14-field 0-based format. EXPERIMENT: value=1 (present +
     * non-zero) for all — tests whether fixing the FORMAT (esp. type 0 presence)
     * unwedges; exact field magnitudes are RE-TBD if presence alone is insufficient. */
    if (version < 0x20) {
        uint32_t nm = 14;
        if (nm > max_entries) {
            nm = max_entries;
        }
        /* EXPERIMENT (2026-06-04): value=1 made the GPU treat itself as broken
         * (teardown+retry loop). Try REAL capability values instead — Monterey
         * type i (0-based, struct off i*4) maps to 26.5 type i+1 (1-based, off
         * (i+1-1)*4 == i*4), so reuse devinfo_values[i+1]. If this stops the
         * retry, observe whether stable-GPU Monterey SW-composites (present) or
         * engages Metal (no present = the GPU wall). Env APPLE_GFX_MONT1 forces
         * back to value=1 for comparison. */
        static int mont1 = -1;
        if (mont1 < 0) {
            mont1 = getenv("APPLE_GFX_MONT1") ? 1 : 0;
        }
        for (uint32_t i = 0; i < nm; i++) {
            uint32_t v = (!mont1 && (i + 1) <= PVG_DEVINFO_FIELD_COUNT)
                             ? devinfo_values[i + 1] : 1u;
            wr_u32(resp + i * PVG_DEVINFO_TLV_STRIDE + 0, i);    /* type 0..0xd */
            wr_u32(resp + i * PVG_DEVINFO_TLV_STRIDE + 4, v);
        }
        n_entries = nm;
        pg_log(d, "GetDeviceInfo: ver=0x%x (Monterey PVG) -> emitted %u 0-based "
                  "fields (types 0..0xd, %s)", version, nm,
               mont1 ? "value=1" : "26.5-mapped caps");
    } else
    for (uint32_t i = 0; i < n_entries; i++) {
        uint32_t type = i + 1;                 /* field types are 1-based */
        uint32_t value = no_metal ? 1u
                             : (type <= PVG_DEVINFO_FIELD_COUNT)
                             ? devinfo_values[type]
                             : 1;              /* default present + non-zero */
        wr_u32(resp + i * PVG_DEVINFO_TLV_STRIDE + 0, type);
        wr_u32(resp + i * PVG_DEVINFO_TLV_STRIDE + 4, value);
    }

    d->stats.device_info_responses++;
    d->stats.last_device_info_pfn = buf_pfn;
    d->stats.last_device_info_count = n_entries;
    pg_log(d, "CmdGetDeviceInfo ver=0x%x bufPFN=0x%x len_qw=0x%x -> wrote %u TLV "
              "fields (0x%llx bytes) @GPA=0x%llx",
           version, buf_pfn, len_qw, n_entries,
           (unsigned long long)resp_bytes, (unsigned long long)buf_gpa);
    /* Ack this command's own root-channel fence (low byte selects the slot). */
    pg_signal_stamp(d, hdr->stampValue);
    /* setupDeviceInfo's post-parse AppleParavirtChannel::addCommand
     * arms a fence on the device-info channel's NON-zero stamp slot and then
     * blocks in waitForStamp (no further FIFO doorbell is emitted). Advance the
     * per-channel fence slots now so that wait completes and GPU::start can
     * proceed past setupDeviceInfo into its per-display loop. */
    pg_advance_channel_fences(d, hdr->stampValue);
}

/* CmdDefineChildFIFO (0x30): record the child FIFO's virtual-channel id, then ack.
 *
 * AppleParavirtVirtualChannel::init (kext 0x11814) emits this for each virtual
 * channel created in setupChannels (the 3 fixed channels ids 1..3 @0x13108, the
 * per-display channel @0x13218, and the conditional id-0xc channel @0x1338c) —
 * BEFORE the display's setupSharedState submits op 0x01. The body is a single u32
 * = the channel id ([channel+0x68]). All channels (root + children) submit through
 * the SAME shared ring + 0x1008 doorbell (writeFifo, kext 0x14528 -> [gpu+0x380]),
 * so there is no separate child ring to drain; the structural requirement is only
 * that the device acknowledge the child FIFO definition (bind the channel id) so
 * the display's channel is a known FIFO when its op 0x01 arrives on the shared
 * ring. Previously this opcode fell through the "(unhandled)" default. */
static void op_define_child_fifo(PGNativeDevice *d, const PGCmdHeader *hdr,
                                 const uint8_t *payload, uint32_t payload_len)
{
    uint32_t channel_id = 0;
    if (payload_len >= 4) {
        channel_id = rd_u32(payload + 0x0);
    } else {
        /* objectCount-encoded variant: channel id may ride in objectCount. */
        channel_id = hdr->objectCount;
    }

    PGChildFIFO *cf = pg_child_fifo_slot(d, channel_id);
    if (cf) {
        cf->valid      = true;
        cf->channel_id = channel_id;
    }
    d->stats.child_fifos_defined++;
    d->stats.last_child_fifo_id = channel_id;
    pg_log(d, "CmdDefineChildFIFO channel=%u len=0x%x -> child FIFO bound (slot %s, "
              "tracked=%u)", channel_id, hdr->length, cf ? "ok" : "FULL",
           d->child_fifo_count);
    pg_signal_stamp(d, hdr->stampValue);
}

/* CmdDeleteChildFIFO (0x31): drop the tracked child FIFO, then ack. */
static void op_delete_child_fifo(PGNativeDevice *d, const PGCmdHeader *hdr,
                                 const uint8_t *payload, uint32_t payload_len)
{
    uint32_t channel_id = (payload_len >= 4) ? rd_u32(payload + 0x0)
                                             : hdr->objectCount;
    PGChildFIFO *cf = pg_native_find_child_fifo(d, channel_id);
    if (cf) {
        cf->valid = false;
        if (d->child_fifo_count) {
            d->child_fifo_count--;
        }
    }
    d->stats.child_fifos_deleted++;
    pg_log(d, "CmdDeleteChildFIFO channel=%u -> %s",
           channel_id, cf ? "released" : "unknown");
    pg_signal_stamp(d, hdr->stampValue);
}

/* CmdSetObjectList (0x33): record the task's object-list binding, then ack.
 *
 * AppleParavirtTask::setResourceHeap (kext 0x1cff0) emits this once per cycle to
 * bind the task's resource-heap / object table (APVCmdSetObjectList body, 0xc
 * bytes) before the per-display loop runs. The object<->resource binding beyond
 * the leading words is not load-bearing for display bring-up (docs/metal-vulkan-translator.md
 * open-questions), but the command MUST be acknowledged (it was "(unhandled)"),
 * and recording the binding lets the harness assert it ran. */
static void op_set_object_list(PGNativeDevice *d, const PGCmdHeader *hdr,
                               const uint8_t *payload, uint32_t payload_len)
{
    d->object_list_bound = true;
    d->last_object_list.word0 = payload_len >= 4  ? rd_u32(payload + 0x0) : 0;
    d->last_object_list.word1 = payload_len >= 8  ? rd_u32(payload + 0x4) : 0;
    d->last_object_list.word2 = payload_len >= 12 ? rd_u32(payload + 0x8) : 0;
    d->stats.object_lists_set++;
    pg_log(d, "CmdSetObjectList objCount=%u len=0x%x body={0x%x,0x%x,0x%x} -> bound",
           hdr->objectCount, hdr->length, d->last_object_list.word0,
           d->last_object_list.word1, d->last_object_list.word2);
    pg_signal_stamp(d, hdr->stampValue);
}

/* CmdSetObjAndPlace (0x44): bind a task's GPU resource heap (an IOGPUMemoryMap).
 *
 * Builder: AppleParavirtTask::setResourceHeap (IOGPUFamily kext 0x1cf00), which
 * emits this 28-byte command immediately followed by CmdSetObjectList (0x33). It
 * tells the device the memory map backing the task's GPU objects, so objectIDs in
 * the ExecIndirect stream can resolve. Body layout (capture + disasm):
 *   [0x00] u32 heap/object id (== task id in capture)
 *   [0x04] u32 [task+0x1d0]
 *   [0x08] u32 [memMap+0x28]
 *   [0x0c] u64 masked heap GPA (x0 & 0xffffc00000003fff) base + low flags
 *   [0x14] u64 heap size
 * Recording it (vs the old ack-only default) gives the Metal task a defined
 * resource heap; without it the device-init path tears down + restarts. */
static void op_set_obj_and_place(PGNativeDevice *d, const PGCmdHeader *hdr,
                                 const uint8_t *payload, uint32_t payload_len)
{
    if (payload_len < 0x1c) {
        pg_log(d, "SetObjAndPlace: short payload (%u < 0x1c)", payload_len);
        d->stats.parse_errors++;
        pg_signal_stamp(d, hdr->stampValue);
        return;
    }
    uint32_t heap_id  = rd_u32(payload + 0x00);
    uint64_t heap_gpa = rd_u64(payload + 0x0c);
    uint64_t heap_sz  = rd_u32(payload + 0x14); /* [0x14] size; [0x18]=alt size */

    PGTask *t = pg_native_find_task(d, heap_id);
    if (!t) {
        t = pg_task_alloc(d, heap_id, PVG_CREATETASK_VMSIZE, d->root_page_gpa);
    }
    if (t) {
        t->heap_bound = true;
        t->heap_id    = heap_id;
        t->heap_gpa   = heap_gpa;
        t->heap_size  = heap_sz;
    }
    pg_log(d, "CmdSetObjAndPlace heapID=%u heapGPA=0x%llx size=0x%llx -> resource "
              "heap bound to task %u%s",
           heap_id, (unsigned long long)heap_gpa, (unsigned long long)heap_sz,
           heap_id, t ? "" : " (TASK ALLOC FAILED)");
    pg_signal_stamp(d, hdr->stampValue);
}

/* DisplaySleepState (op 0x09): the guest's IOMFB/display power-state machine
 * submits this fire-and-forget on a power transition (Monterey
 * do_power_state_change_gated, kc 0xfffffe00083004bc -> mov w3,#9 -> addCommand
 * -> retab; NO inline waitForStamp; identical shape in 26.5). Acking only its
 * stamp (the old default: path) retires the submit fence but leaves Monterey's
 * display IOWorkLoop parked waiting for a host->guest DISPLAY-channel interrupt
 * carrying mailbox cause bit 2 (createDisplayAttributes / "online-ready"). The
 * backend never raised it after op 9 -> the guest livelocks at 95% CPU,
 * DefineTask2 stuck at taskID=0, display never up, mapSurface/present=0, black.
 *
 * Fix (RE wf_4991e191, disasm of handleHostInterrupt @0xfffffe00082fcd64 +
 * the GPU IES MMIO+0x14/+0x18 demux @0xfffffe00083060b8): on the power-ON
 * transition re-drive the display-attributes IRQ via the EXISTING primitive
 * pg_signal_display_attr() (sets mbox[0x100] |= (1<<2) honoring the +0x104 enable
 * mask, sets this port's bit in disp_cause @0x1014, pulses PVG_IRQ_GFX) so
 * handleHostInterrupt re-enters -> tbnz w8,#2 -> createDisplayAttributes ->
 * emits op-0x02 DisplayAck -> IOMFB power-state machine advances. The stamp is
 * ALWAYS acked first (submit fence). body[0]=u32 pstate, body[4]=u32 on-flag.
 *
 * VER-GATED to Monterey (devinfo_version < 0x20): 26.5 (ver 0x2d) already reaches
 * WindowServer past this point without the re-drive, so we must NOT churn its
 * attribute path. The vblank pulse deliberately avoids cause bit 2 for the same
 * reason; this is the one place we intentionally re-assert it, only for Monterey.
 */
static void op_display_sleep_state(PGNativeDevice *d, const PGCmdHeader *hdr,
                                   const uint8_t *payload, uint32_t payload_len)
{
    uint32_t pstate  = (payload_len >= 4) ? rd_u32(payload + 0x0) : 0;
    uint32_t on_flag = (payload_len >= 8) ? rd_u32(payload + 0x4) : 0;
    bool turning_on  = (pstate != 0) || (on_flag != 0);
    bool monterey    = d->devinfo_version != 0 && d->devinfo_version < 0x20;

    /* (1) ALWAYS ack the op's own fence first (MMIO+0x18 stamp leg). */
    pg_signal_stamp(d, hdr->stampValue);

    pg_log(d, "DisplaySleepState (op 0x09) pstate=%u on=%u ver=0x%x len=0x%x "
              "stamp=0x%x -> %s", pstate, on_flag, d->devinfo_version,
           hdr->length, hdr->stampValue,
           (turning_on && monterey) ? "re-drive displayAttr (cause bit2 + IRQ)"
                                     : "ack only");

    /* (2) Monterey power-ON: re-drive the DISPLAY-channel attributes IRQ for
     *     every live display. This is the host->guest signal the guest is parked
     *     on; without it the display never reports online and bring-up wedges. */
    if (turning_on && monterey) {
        unsigned redriven = 0;
        for (unsigned i = 0; i < PVG_MAX_DISPLAYS; i++) {
            PGDisplay *disp = &d->displays[i];
            if (disp->valid && disp->mbox_gpa) {
                pg_signal_display_attr(d, disp);
                redriven++;
            }
        }
        pg_log(d, "DisplaySleepState: re-drove displayAttr on %u display(s)",
               redriven);
    }
}

/* ----------------------------------------------------- single-command parse */

uint32_t pg_native_dispatch_one(PGNativeDevice *d, const uint8_t *buf,
                                uint32_t len)
{
    if (len < PVG_CMD_HDR_SIZE) {
        d->stats.parse_errors++;
        return 0;
    }
    PGCmdHeader hdr;
    hdr.cmdID       = rd_u16(buf + 0);
    hdr.objectCount = rd_u16(buf + 2);
    hdr.length      = rd_u32(buf + 4);
    hdr.stampValue  = rd_u32(buf + 8);

    if (hdr.length < PVG_CMD_HDR_SIZE || hdr.length > len) {
        pg_log(d, "bad header: cmdID=0x%x length=0x%x (avail=%u)",
               hdr.cmdID, hdr.length, len);
        d->stats.parse_errors++;
        return 0;
    }

    const uint8_t *payload = buf + PVG_CMD_HDR_SIZE;
    uint32_t payload_len = hdr.length - PVG_CMD_HDR_SIZE;
    d->stats.commands_parsed++;

    switch (hdr.cmdID) {
    case PVG_OP_CmdDefineTask2:
        op_define_task2(d, &hdr, payload, payload_len);
        break;
    case PVG_OP_CmdMapMemory2:
        op_map_memory2(d, &hdr, payload, payload_len);
        break;
    case PVG_OP_DisplaySetSharedState:
        op_display_set_shared_state(d, &hdr, payload, payload_len);
        break;
    case PVG_OP_DisplayTransaction3:
        op_display_transaction3(d, &hdr, payload, payload_len);
        break;
    case PVG_OP_CmdExecIndirect2:
    case PVG_OP_CmdExecIndirect3:
        op_exec_indirect(d, &hdr, payload, payload_len);
        break;
    case PVG_OP_CmdGetDeviceInfo:
        op_get_device_info(d, &hdr, payload, payload_len);
        break;
    case PVG_OP_CmdDefineChildFIFO:
        op_define_child_fifo(d, &hdr, payload, payload_len);
        break;
    case PVG_OP_CmdDeleteChildFIFO:
        op_delete_child_fifo(d, &hdr, payload, payload_len);
        break;
    case PVG_OP_CmdSetObjectList:
        op_set_object_list(d, &hdr, payload, payload_len);
        break;
    case PVG_OP_CmdSetObjAndPlace:
        op_set_obj_and_place(d, &hdr, payload, payload_len);
        break;
    case PVG_OP_DisplayAck:
        /*
         * the guest acks a display-attributes / mode-change IRQ on the
         * display channel (seen live as op 0x02 on channel 4 right after
         * createDisplayAttributes). It carries no host-owned payload; we just ack
         * its fence so the guest's submit completes. (Previously "(unhandled)".)
         */
        d->stats.display_acks++;
        pg_log(d, "DisplayAck (op 0x02) on display channel len=0x%x stamp=0x%x",
               hdr.length, hdr.stampValue);
        pg_signal_stamp(d, hdr.stampValue);
        break;
    case PVG_OP_DisplaySleepState:
        op_display_sleep_state(d, &hdr, payload, payload_len);
        break;
    case PVG_OP_NOP:
    case PVG_OP_CmdDebug:
        pg_log(d, "op 0x%x (NOP/Debug) len=0x%x", hdr.cmdID, hdr.length);
        pg_signal_stamp(d, hdr.stampValue);
        break;
    default: {
        /* Known-but-unhandled opcode: log + ack so the fence still completes. */
        pg_log(d, "op 0x%x (unhandled) objCount=%u len=0x%x stamp=0x%x",
               hdr.cmdID, hdr.objectCount, hdr.length, hdr.stampValue);
        /*  capture: hexdump the body so we can decode the Metal-init
         * commands (CmdSetObjAndPlace 0x44, etc.). Temporary instrumentation. */
        char hexb[3 * 64 + 1];
        uint32_t hn = payload_len < 64 ? payload_len : 64;
        for (uint32_t i = 0; i < hn; i++) {
            static const char hx[] = "0123456789abcdef";
            hexb[i * 3 + 0] = hx[(payload[i] >> 4) & 0xf];
            hexb[i * 3 + 1] = hx[payload[i] & 0xf];
            hexb[i * 3 + 2] = ' ';
        }
        hexb[hn ? hn * 3 - 1 : 0] = '\0';
        pg_log(d, "  op 0x%x body[%u]: %s", hdr.cmdID, payload_len, hexb);
        pg_signal_stamp(d, hdr.stampValue);
        break;
    }
    }

    /* Commands are dword-granular; round the consumed length up to 4. */
    uint32_t consumed = (hdr.length + 3u) & ~3u;
    return consumed;
}

/* --------------------------------------------------------------- ring drain
 *
 * The command data region begins at ring_gpa + fifo_start (the setFifoStart /
 * 0x1010 value), NOT ring_gpa + 0x1000. Confirmed live (session 47): with
 * fifo_start=0x4000 the first command header sits exactly at ring_gpa+0x4000
 * (cmdID=0x38 DefineTask2, length=0x1c == first doorbell tail), while
 * ring_gpa+0 and ring_gpa+0x1000 read all-zero. The doorbell tail and our head
 * cursor are byte offsets RELATIVE to this data base, wrapping at capacity =
 * ring_len - fifo_start. (Older code assumed a fixed 0x1000 header; that only
 * happened to match the offline replay fixture, which used fifo_start=0x1000.)
 * Supports both doorbell transports (§0.5): pg_native_fifo_drain is called from
 * the 0x1008 doorbell write AND can be polled.
 */
uint32_t pg_native_fifo_drain(PGNativeDevice *d)
{
    /* Data region starts at the guest-published setFifoStart offset; fall back
     * to the legacy 0x1000 header size if the guest never set it (replay). */
    uint64_t hdr_off = d->fifo_start ? d->fifo_start : PVG_FIFO_HDR_SIZE;
    if (!d->fifo_enabled || d->ring_len <= hdr_off) {
        return 0;
    }
    uint64_t data_gpa = d->ring_gpa + hdr_off;
    uint64_t capacity = d->ring_len - hdr_off;

    uint8_t *ring = NULL;
    if (d->ops && d->ops->host_ptr) {
        ring = (uint8_t *)d->ops->host_ptr(d->ops->opaque, data_gpa,
                                           capacity, true);
    }

    uint32_t dispatched = 0;
    uint32_t head = d->fifo_head % capacity;
    uint32_t tail = d->fifo_tail % capacity;

    while (head != tail) {
        /* Bytes available contiguously before the wrap point. */
        uint32_t avail = (tail > head) ? (tail - head)
                                       : (uint32_t)(capacity - head);

        /* Read the 12-byte header (handle split across the wrap). */
        uint8_t hdrbuf[PVG_CMD_HDR_SIZE];
        if (!ring) {
            if (!d->ops || !d->ops->read_mem) {
                break;
            }
            /* Bulk-read path: read header (may wrap). */
            uint32_t first = avail < PVG_CMD_HDR_SIZE ? avail : PVG_CMD_HDR_SIZE;
            d->ops->read_mem(d->ops->opaque, data_gpa + head, first, hdrbuf);
            if (first < PVG_CMD_HDR_SIZE) {
                d->ops->read_mem(d->ops->opaque, data_gpa,
                                 PVG_CMD_HDR_SIZE - first, hdrbuf + first);
            }
        } else {
            if (avail >= PVG_CMD_HDR_SIZE) {
                memcpy(hdrbuf, ring + head, PVG_CMD_HDR_SIZE);
            } else {
                memcpy(hdrbuf, ring + head, avail);
                memcpy(hdrbuf + avail, ring, PVG_CMD_HDR_SIZE - avail);
            }
        }

        uint32_t cmd_len = rd_u32(hdrbuf + 4);
        if (cmd_len < PVG_CMD_HDR_SIZE || cmd_len > capacity) {
            pg_log(d, "drain: bad cmd_len=0x%x at head=0x%x", cmd_len, head);
            d->stats.parse_errors++;
            break;
        }
        uint32_t cmd_pad = (cmd_len + 3u) & ~3u;

        /* Assemble the full command into a contiguous buffer (handle wrap). */
        static uint8_t cmdbuf[0x10000];
        uint32_t copy_len = cmd_len < sizeof(cmdbuf) ? cmd_len : sizeof(cmdbuf);
        if (!ring) {
            uint32_t first = avail < copy_len ? avail : copy_len;
            d->ops->read_mem(d->ops->opaque, data_gpa + head, first, cmdbuf);
            if (first < copy_len) {
                d->ops->read_mem(d->ops->opaque, data_gpa,
                                 copy_len - first, cmdbuf + first);
            }
        } else {
            if (avail >= copy_len) {
                memcpy(cmdbuf, ring + head, copy_len);
            } else {
                memcpy(cmdbuf, ring + head, avail);
                memcpy(cmdbuf + avail, ring, copy_len - avail);
            }
        }

        if (pg_native_dispatch_one(d, cmdbuf, copy_len) == 0) {
            break; /* parse error already counted */
        }
        dispatched++;

        head = (uint32_t)((head + cmd_pad) % capacity);
        d->fifo_head = head;
    }

    return dispatched;
}

/* ----------------------------------------------------- child-FIFO ring drain
 *
 * The virtual channels (exec=1, object=2, memory=3, per-display=4+,
 * info=0xc) each own a SEPARATE ring buffer + a SEPARATE doorbell (wakeFifo,
 * 0x1020), unlike the root channel (root ring + 0x1008). AppleParavirtDisplay::
 * setupSharedState submits op 0x01 (DisplaySetSharedStatePage) on CHANNEL 4's own
 * ring and rings wakeFifo(4); previously the backend only logged that doorbell, so
 * op 0x01 sat unserviced and the trace went quiet after wakeFifo child=4.
 *
 * The per-channel ring geometry comes from the ROOT-PAGE index-record array
 * (setRootPage 0x101c page): record(id) = root_page_gpa + 0x400 + (id-1)*0x14,
 * with producer/WRITE index @+0x0 (byte offset), consumer/READ index @+0x4, and
 * ring base PFN @+0x10 (child_ring_gpa = PFN<<14). Ring DATA starts at
 * child_ring_gpa + 0x4000 (the first 0x4000 is header/guard, exactly like the
 * root ring's setFifoStart). We drain head->producer, dispatch each command, then
 * publish our new consumer index back into record+0x4 so the guest sees space.
 */
uint32_t pg_native_child_fifo_drain(PGNativeDevice *d, uint32_t channel_id)
{
    if (channel_id == 0 || !d->root_page_gpa || !d->ops) {
        return 0;
    }
    if (!d->ops->host_ptr && !d->ops->read_mem) {
        return 0;
    }

    /* (1) Read the per-channel index record from the root page. */
    uint64_t rec_gpa = d->root_page_gpa + PVG_CHILD_REC_BASE_OFF +
                       (uint64_t)(channel_id - 1) * PVG_CHILD_REC_STRIDE;
    uint8_t rec[PVG_CHILD_REC_STRIDE];
    if (!pg_read_guest(d, rec_gpa, sizeof(rec), rec)) {
        pg_log(d, "child drain ch=%u: cannot read index record @0x%llx",
               channel_id, (unsigned long long)rec_gpa);
        return 0;
    }
    uint32_t producer = rd_u32(rec + PVG_CHILD_REC_PROD_OFF);
    uint32_t ring_pfn = rd_u32(rec + PVG_CHILD_REC_PFN_OFF);
    if (ring_pfn == 0) {
        pg_log(d, "child drain ch=%u: ring PFN not published yet (rec@0x%llx, "
                  "prod=0x%x)", channel_id, (unsigned long long)rec_gpa, producer);
        return 0;
    }
    uint64_t ring_buf_gpa = (uint64_t)ring_pfn << PVG_GUEST_PAGE_SHIFT;
    uint64_t data_gpa = ring_buf_gpa + PVG_CHILD_RING_DATA_OFF;

    /* (2) Track this channel + its ring capacity. The capacity is the channel's
     * allocated ring size ([chan+0x50]); for the display channel that is 0x4000.
     * We default to PVG_CHILD_RING_DEFAULT_CAP and remember our consumer cursor
     * across doorbells in the tracked PGChildFIFO. */
    PGChildFIFO *cf = pg_child_fifo_slot(d, channel_id);
    if (cf && !cf->valid) {
        cf->valid = true;
        cf->channel_id = channel_id;
        d->stats.child_fifos_defined++;
    }
    uint32_t capacity = (cf && cf->capacity) ? cf->capacity
                                             : PVG_CHILD_RING_DEFAULT_CAP;
    if (cf && !cf->capacity) {
        cf->capacity = capacity;
    }

    /* (1b) INDIRECT child-ring layout .
     *
     * Two layouts are seen for the per-display channel-4 ring. In the contiguous
     * layout the command bytes live directly at ring_buf_gpa +
     * 0x4000. In the INDIRECT layout the ring-base page is a one-entry scatter
     * list: word[0] holds a PFN that points at the actual command-data page, and
     * the +0x4000 slot is zero. If the contiguous +0x4000 slot has no valid
     * command header at our head but ring_base[0] is a plausible PFN whose page
     * carries a valid command (op>0, sane cmd_len), follow the indirection.
     * This keeps the /65 contiguous path intact (its +0x4000 slot is valid)
     * and recovers op 0x01 on the indirect-layout boots where it was dropped. */
    {
        uint32_t cur_head_off = (cf ? cf->head : 0) % capacity;
        uint8_t probe[PVG_CMD_HDR_SIZE];
        uint32_t contig_len = 0;
        if (pg_read_guest(d, data_gpa + cur_head_off, sizeof(probe), probe)) {
            contig_len = rd_u32(probe + 4);
        }
        bool contig_ok = (contig_len >= PVG_CMD_HDR_SIZE &&
                          contig_len <= capacity);
        if (!contig_ok) {
            uint8_t hdr[4];
            if (pg_read_guest(d, ring_buf_gpa, sizeof(hdr), hdr)) {
                uint32_t ind_pfn = rd_u32(hdr);
                if (ind_pfn != 0 && ind_pfn != ring_pfn) {
                    uint64_t ind_gpa = (uint64_t)ind_pfn << PVG_GUEST_PAGE_SHIFT;
                    uint8_t iprobe[PVG_CMD_HDR_SIZE];
                    if (pg_read_guest(d, ind_gpa + cur_head_off,
                                      sizeof(iprobe), iprobe)) {
                        uint32_t iop  = rd_u32(iprobe);
                        uint32_t ilen = rd_u32(iprobe + 4);
                        if (iop != 0 && ilen >= PVG_CMD_HDR_SIZE &&
                            ilen <= capacity) {
                            pg_log(d, "child drain ch=%u: INDIRECT ring layout "
                                      "(base[0] PFN=0x%x -> dataGPA=0x%llx; "
                                      "op=0x%x len=0x%x)",
                                   channel_id, ind_pfn,
                                   (unsigned long long)ind_gpa, iop, ilen);
                            data_gpa = ind_gpa;
                            d->stats.child_ring_indirect++;
                        }
                    }
                }
            }
        }
    }

    uint8_t *ring = NULL;
    if (d->ops->host_ptr) {
        ring = (uint8_t *)d->ops->host_ptr(d->ops->opaque, data_gpa,
                                           capacity, true);
    }

    uint32_t head = (cf ? cf->head : 0) % capacity;
    uint32_t tail = producer % capacity;
    uint32_t dispatched = 0;
    uint32_t guard = capacity; /* never iterate past one ring's worth */

    while (head != tail && guard) {
        uint32_t avail = (tail > head) ? (tail - head)
                                       : (uint32_t)(capacity - head);

        /* Assemble the (possibly wrapped) command into a contiguous buffer. */
        static uint8_t cmdbuf[0x10000];
        uint8_t hdrbuf[PVG_CMD_HDR_SIZE];
        if (ring) {
            if (avail >= PVG_CMD_HDR_SIZE) {
                memcpy(hdrbuf, ring + head, PVG_CMD_HDR_SIZE);
            } else {
                memcpy(hdrbuf, ring + head, avail);
                memcpy(hdrbuf + avail, ring, PVG_CMD_HDR_SIZE - avail);
            }
        } else {
            uint32_t first = avail < PVG_CMD_HDR_SIZE ? avail : PVG_CMD_HDR_SIZE;
            d->ops->read_mem(d->ops->opaque, data_gpa + head, first, hdrbuf);
            if (first < PVG_CMD_HDR_SIZE) {
                d->ops->read_mem(d->ops->opaque, data_gpa,
                                 PVG_CMD_HDR_SIZE - first, hdrbuf + first);
            }
        }

        uint32_t cmd_len = rd_u32(hdrbuf + 4);
        if (cmd_len < PVG_CMD_HDR_SIZE || cmd_len > capacity) {
            pg_log(d, "child drain ch=%u: bad cmd_len=0x%x at head=0x%x",
                   channel_id, cmd_len, head);
            d->stats.parse_errors++;
            break;
        }
        uint32_t cmd_pad = (cmd_len + 3u) & ~3u;
        uint32_t copy_len = cmd_len < sizeof(cmdbuf) ? cmd_len : sizeof(cmdbuf);

        if (ring) {
            if (avail >= copy_len) {
                memcpy(cmdbuf, ring + head, copy_len);
            } else {
                memcpy(cmdbuf, ring + head, avail);
                memcpy(cmdbuf + avail, ring, copy_len - avail);
            }
        } else {
            uint32_t first = avail < copy_len ? avail : copy_len;
            d->ops->read_mem(d->ops->opaque, data_gpa + head, first, cmdbuf);
            if (first < copy_len) {
                d->ops->read_mem(d->ops->opaque, data_gpa,
                                 copy_len - first, cmdbuf + first);
            }
        }

        if (pg_native_dispatch_one(d, cmdbuf, copy_len) == 0) {
            break;
        }
        dispatched++;
        head = (uint32_t)((head + cmd_pad) % capacity);
        if (cf) {
            cf->head = head;
        }
        guard--;
    }

    /* (3) Publish our consumer index back into the record so the guest's
     * free-space check (submitBufferLocked reads rec+0x4) sees the ring drained. */
    if (dispatched) {
        if (d->ops->host_ptr) {
            uint8_t *recp = (uint8_t *)d->ops->host_ptr(
                d->ops->opaque, rec_gpa + PVG_CHILD_REC_CONS_OFF, 4, false);
            if (recp) {
                wr_u32(recp, head);
            }
        }
        d->stats.child_ring_drains++;
        d->stats.child_cmds_dispatched += dispatched;
        pg_log(d, "child drain ch=%u: dispatched %u cmd(s), head->0x%x "
                  "(ringGPA=0x%llx cap=0x%x)", channel_id, dispatched, head,
               (unsigned long long)ring_buf_gpa, capacity);
    }
    return dispatched;
}

/* ---------------------------------------------------------- gfx-BAR MMIO I/O */

uint32_t pg_native_mmio_read(PGNativeDevice *d, uint64_t offset, unsigned size)
{
    uint32_t val = 0;
    (void)size;
    d->stats.mmio_reads++;
    switch (offset) {
    case PVG_REG_VERSION:        /* 0x122c -> version / EFI-mode = 1 */
        val = PVG_VERSION_VALUE;
        break;
    case PVG_REG_FIFO_READ:      /* 0x100c -> consumer read index (head) */
        val = d->fifo_head ? (d->fifo_start + d->fifo_head) : d->fifo_start;
        /*
         * The live trace reports 0x100c as the head index in the SAME units as
         * the doorbell tail (0x1c, 0x44...). We mirror the tail-after-drain.
         */
        val = d->fifo_head;
        break;
    case PVG_REG_IRQ_CAUSE:      /* 0x1018 -> STAMP/EXEC cause, read+clear */
        val = d->irq_cause;
        d->irq_cause = 0;        /* read-and-CLEAR */
        break;
    case PVG_REG_IRQ_CAUSE_1014: /* 0x1014 -> DISPLAY/SWAP cause, read+clear */
        /*
         * Distinct register from 0x18 (the completion contract §2/§6.1): the
         * guest's IRQ block reads 0x14 as the per-display-port swap cause bitmask
         * (iterates set bits -> signalInterrupt per display), separate from the
         * 0x18 stamp/exec cause. Both are read-and-clear.
         */
        val = d->disp_cause;
        d->disp_cause = 0;       /* read-and-CLEAR */
        break;
    case PVG_REG_FAULT:          /* 0x102c -> interrupt fault (none) */
    case PVG_REG_FIFO_FAULT_OFF: /* 0x103c */
        val = 0;
        break;
    case PVG_REG_BINARY_VERSION: /* 0x1034 readback */
        val = d->binary_version;
        break;
    case PVG_REG_ROOT_PAGE:      /* 0x101c readback */
        val = d->root_page_pfn;
        break;
    case PVG_REG_FIFO_CONTROL:   /* 0x1000 -> FIFO-enabled flag */
        val = d->fifo_enabled ? 1 : 0;
        break;
    default:
        val = 0;                 /* whole window reads 0 at rest (§2.4) */
        break;
    }
    return val;
}

void pg_native_mmio_write(PGNativeDevice *d, uint64_t offset,
                          uint32_t value, unsigned size)
{
    (void)size;
    d->stats.mmio_writes++;
    switch (offset) {
    case PVG_REG_BINARY_VERSION: /* 0x1034 setBinaryVersion */
        d->binary_version = value;
        break;

    case PVG_REG_ROOT_PAGE:      /* 0x101c setRootPage -> createTask(16MB) */
        d->root_page_pfn = value;
        d->root_page_gpa = (uint64_t)value << PVG_GUEST_PAGE_SHIFT;
        /* Fires the createTask cb: reserve a 16 MB VA window (task 0). */
        pg_task_alloc(d, 0 /*implicit root task*/, PVG_CREATETASK_VMSIZE,
                      d->root_page_gpa);
        d->stats.createtask_cb++;
        pg_log(d, "setRootPage pfn=0x%x -> rootGPA=0x%llx, createTask(16MB)",
               value, (unsigned long long)d->root_page_gpa);
        break;

    case PVG_REG_FIFO_BASE_PAGE: /* 0x1030 setFifoBasePage (ring PFN) */
        d->fifo_base_pfn = value;
        d->ring_gpa = (uint64_t)value << PVG_GUEST_PAGE_SHIFT;
        pg_log(d, "setFifoBasePage pfn=0x%x -> ringGPA=0x%llx",
               value, (unsigned long long)d->ring_gpa);
        break;

    case PVG_REG_FIFO_LENGTH:    /* 0x1004 setFifoLength */
        d->fifo_length = value;
        d->ring_len = value;
        pg_log(d, "setFifoLength 0x%x", value);
        break;

    case PVG_REG_FIFO_START:     /* 0x1010 setFifoStart */
        d->fifo_start = value;
        pg_log(d, "setFifoStart 0x%x", value);
        break;

    case PVG_REG_FIFO_CONTROL:   /* 0x1000 enableFifo / teardown */
        if (value == 0) {
            d->fifo_enabled = false;
            /*
             * teardownFifos: the guest is destroying the ring. Reset our drain
             * cursor so a subsequent re-arm (enableFifo) starts from a clean
             * head==0, matching the device's fresh consumer index.
             */
            d->fifo_head = 0;
            d->fifo_tail = 0;
            pg_log(d, "teardownFifos");
        } else {
            /*
             * enableFifo (re)arms the ring. The guest's AppleParavirtGPU can
             * tear down and re-create the GPU (AppleParavirtGPU::restart) many
             * times during early bring-up (e.g. while it negotiates device
             * info), republishing the whole geometry (setRootPage / FifoBasePage
             * / FifoLength / FifoStart) before each enable. The device's consumer
             * read index is reset to the start of the fresh ring on each arm, so
             * we MUST reset our drain head to 0 here. Without this the stale head
             * from the previous ring instance lands on a zero ring word and the
             * drain bails ("bad cmd_len=0x0"), so every restart after the first
             * processes ZERO commands. Reset both cursors to track the new ring.
             */
            d->fifo_enabled = true;
            d->fifo_head = 0;
            d->fifo_tail = 0;
            pg_log(d, "enableFifo -> ring live (gpa=0x%llx len=0x%llx) head:=0",
                   (unsigned long long)d->ring_gpa,
                   (unsigned long long)d->ring_len);
        }
        break;

    case PVG_REG_FIFO_WRITTEN:   /* 0x1008 DOORBELL: publish tail + drain */
        d->fifo_tail = value;
        d->stats.doorbells++;
        pg_log(d, "DOORBELL setFifoWritten tail=0x%x (head=0x%x)",
               value, d->fifo_head);
        pg_native_fifo_drain(d);
        break;

    case PVG_REG_WAKE_FIFO:      /* 0x1020 wakeFifo:(child #val) = child doorbell */
        d->stats.wake_fifos++;
        d->stats.last_wake_channel = value;
        pg_log(d, "wakeFifo child=%u -> drain child ring", value);
        /*
         * the virtual-channel doorbell. Drain channel `value`'s own
         * ring so op 0x01 (DisplaySetSharedStatePage on channel 4) and op 0x07
         * (DisplayTransaction3 present) are serviced. Without this the trace went
         * quiet right after "wakeFifo child=4".
         */
        pg_native_child_fifo_drain(d, value);
        break;

    case PVG_REG_DISPLAY_DOORBELL: /* 0x1220 ringDisplayDoorbellAtPort: */
        pg_log(d, "ringDisplayDoorbell port=%u", value);
        break;

    case PVG_REG_EFI_DISPLAY:    /* 0x1200 setEFIDisplay */
        d->efi_display = value;
        pg_log(d, "setEFIDisplay %u", value);
        break;

    case PVG_REG_RESUME_FIFO:    /* 0x1024 */
    case PVG_REG_RESUME_CHILD:   /* 0x1028 */
    case PVG_REG_NEW_INDIRECT_CB:/* 0x1038 */
    case PVG_REG_EFI_MODE_SELECT:
    case PVG_REG_EFI_FB_START:
    case PVG_REG_EFI_FB_LENGTH:
    case PVG_REG_EFI_FB_DEPTH:
    case PVG_REG_EFI_FB_MODE:
    case PVG_REG_EFI_FB_STRIDE:
        pg_log(d, "reg 0x%llx <- 0x%x (recorded)",
               (unsigned long long)offset, value);
        break;

    default:
        pg_log(d, "unknown gfx write 0x%llx <- 0x%x",
               (unsigned long long)offset, value);
        break;
    }
}

/* ------------------------------------------------------ iosfc-BAR MMIO I/O
 *
 * §0.7.2: the second device (base 0x30210000, 64 KiB). Index decode is
 * idx=(offset-0x1000)/8. We implement the four setters + the doorbell that the
 * live trace exercises (0x1000 ringBase, 0x1008 ringSize(entries), 0x1010
 * descriptorBase, 0x1018 doorbell, 0x1020 enable, 0x1030 desc2). Reads return
 * the cached ivars (idx5 status, a constant 1 for the enable ack at idx4).
 */
uint32_t pg_native_iosfc_mmio_read(PGNativeDevice *d, uint64_t offset,
                                   unsigned size)
{
    uint32_t val = 0;
    (void)size;
    d->stats.mmio_reads++;
    switch (offset) {
    case PVG_IOSFC_REG_RING_BASE:
        val = (uint32_t)d->iosfc_ring_gpa;
        break;
    case PVG_IOSFC_REG_RING_SIZE:
        val = d->iosfc_ring_size;
        break;
    case PVG_IOSFC_REG_DESC_BASE:
        val = (uint32_t)d->iosfc_desc_gpa;
        break;
    case PVG_IOSFC_REG_DOORBELL:
        val = d->iosfc_ring_tail;
        break;
    case PVG_IOSFC_REG_DESC_ENABLE:
        /* 0x1020 READ = the consumer/completion head: the index up to which the
         * device has retired ring entries. The guest's device_interrupt_gated
         * polls this to retire its map commands; returning a constant made the
         * guest's completion never advance, so it re-mapped forever and never
         * presented. Expose the real drained head. */
        val = d->iosfc_ring_head;
        break;
    case PVG_IOSFC_REG_STATUS:
        /* idx5: nonzero once the ring+descriptor regions are mapped+enabled. */
        val = (d->iosfc_enabled && d->iosfc_ring_gpa && d->iosfc_desc_gpa)
              ? 1 : d->iosfc_status;
        break;
    case PVG_IOSFC_REG_DESC2_BASE:
        val = (uint32_t)d->iosfc_desc2_gpa;
        break;
    default:
        val = 0;
        break;
    }
    /*  swap-fix diagnostic: log the full iosfc register read sequence so we can
     * see which geometry/slot-count registers the mapper device reads at init
     * (an unhandled offset returning 0 would give the guest a degenerate
     * descriptor-table size -> add_mapping bounds failure for the scanout). */
    if (getenv("APPLE_GFX_IOSFC_TRACE")) {
        bool known = (offset==PVG_IOSFC_REG_RING_BASE||offset==PVG_IOSFC_REG_RING_SIZE||
                      offset==PVG_IOSFC_REG_DESC_BASE||offset==PVG_IOSFC_REG_DOORBELL||
                      offset==PVG_IOSFC_REG_DESC_ENABLE||offset==PVG_IOSFC_REG_STATUS||
                      offset==PVG_IOSFC_REG_DESC2_BASE);
        pg_log(d, "iosfc_read off=0x%llx -> 0x%x%s", (unsigned long long)offset,
               val, known ? "" : "  <-- UNHANDLED(0)");
    }
    return val;
}

void pg_native_iosfc_mmio_write(PGNativeDevice *d, uint64_t offset,
                                uint32_t value, unsigned size)
{
    (void)size;
    d->stats.mmio_writes++;
    d->stats.iosfc_mmio_writes++;
    switch (offset) {
    case PVG_IOSFC_REG_RING_BASE:   /* 0x1000 setRingBase */
        d->iosfc_ring_gpa = value;
        pg_log(d, "iosfc setRingBase 0x%x", value);
        break;

    case PVG_IOSFC_REG_RING_SIZE:   /* 0x1008 setRingSize (entries) */
        d->iosfc_ring_size = value;
        pg_log(d, "iosfc setRingSize %u entries (%u bytes)", value,
               value * PVG_IOSFC_RING_ENTRY_SIZE);
        break;

    case PVG_IOSFC_REG_DESC_BASE:   /* 0x1010 setDescriptorBase */
        d->iosfc_desc_gpa = value;
        pg_log(d, "iosfc setDescriptorBase 0x%x", value);
        break;

    case PVG_IOSFC_REG_DOORBELL:    /* 0x1018 DOORBELL: publish tail + drain */
        d->iosfc_ring_tail = value;
        d->stats.iosfc_doorbells++;
        pg_log(d, "iosfc DOORBELL tail=%u (head=%u)", value, d->iosfc_ring_head);
        pg_native_iosfc_ring_drain(d);
        break;

    case PVG_IOSFC_REG_DESC_ENABLE: /* 0x1020 setDescriptorEnable */
        d->iosfc_enabled = (value & 1) != 0;
        pg_log(d, "iosfc setDescriptorEnable %u", d->iosfc_enabled);
        break;

    case PVG_IOSFC_REG_DESC2_BASE:  /* 0x1030 secondary descriptor base */
        d->iosfc_desc2_gpa = value;
        pg_log(d, "iosfc setDescriptor2Base 0x%x", value);
        break;

    default:
        pg_log(d, "unknown iosfc write 0x%llx <- 0x%x",
               (unsigned long long)offset, value);
        break;
    }
}

/* ----------------------------------------------------- iosfc ring drain
 *
 * §0.7.2: each entry is 16 bytes {u32 op; u32 surfaceID; u32 result; u32 pad};
 * op 1=map, 2=unmap, 3=test. The ring lives at iosfc_ring_gpa with capacity
 * iosfc_ring_size entries; head/tail are entry indices. We drain from head up
 * to the doorbell tail, dispatch each entry, and write the result back.
 */
uint32_t pg_native_iosfc_ring_drain(PGNativeDevice *d)
{
    if (!d->iosfc_ring_gpa || d->iosfc_ring_size == 0) {
        return 0;
    }
    uint32_t cap = d->iosfc_ring_size;
    uint32_t head = d->iosfc_ring_head % cap;
    uint32_t tail = d->iosfc_ring_tail % cap;
    uint32_t processed = 0;
    /* Guard against a runaway loop: never iterate more than capacity entries. */
    uint32_t guard = cap;

    while (head != tail && guard--) {
        uint64_t ent_gpa = d->iosfc_ring_gpa +
                           (uint64_t)head * PVG_IOSFC_RING_ENTRY_SIZE;
        uint8_t ent[PVG_IOSFC_RING_ENTRY_SIZE];
        if (!pg_read_guest(d, ent_gpa, sizeof(ent), ent)) {
            pg_log(d, "iosfc drain: cannot read entry @0x%llx",
                   (unsigned long long)ent_gpa);
            break;
        }
        uint32_t op         = rd_u32(ent + PVG_IOSFC_ENT_OP);
        uint32_t surface_id = rd_u32(ent + PVG_IOSFC_ENT_SURFACE_ID);
        uint32_t result = 0;     /* 0 = ok */

        switch (op) {
        case PVG_IOSFC_OP_MAP_SURFACE:
            result = pg_native_map_surface(d, surface_id) ? 0u : 1u;
            break;
        case PVG_IOSFC_OP_UNMAP_SURFACE:
            pg_unmap_surface(d, surface_id);
            break;
        case PVG_IOSFC_OP_TEST_SURFACE:
            result = pg_native_find_surface(d, surface_id) ? 0u : 1u;
            pg_log(d, "iosfc testSurface surfaceID=%u -> %u", surface_id,
                   result);
            break;
        default:
            pg_log(d, "iosfc drain: unknown op=%u surfaceID=%u", op,
                   surface_id);
            result = 1;
            break;
        }

        /* Write the result word back to the entry (host completion). */
        if (d->ops && d->ops->host_ptr) {
            uint8_t *p = (uint8_t *)d->ops->host_ptr(
                d->ops->opaque, ent_gpa + PVG_IOSFC_ENT_RESULT, 4, false);
            if (p) {
                p[0] = (uint8_t)result;
                p[1] = (uint8_t)(result >> 8);
                p[2] = (uint8_t)(result >> 16);
                p[3] = (uint8_t)(result >> 24);
            }
        }

        d->stats.iosfc_ring_entries++;
        processed++;
        head = (head + 1) % cap;
        d->iosfc_ring_head = head;
    }

    /* Surface ops complete on the iosfc IRQ line, not the gfx stamp channel. */
    if (processed && d->ops && d->ops->raise_irq) {
        d->ops->raise_irq(d->ops->opaque, PVG_IRQ_IOSFC);
        d->stats.irqs_raised++;
    }
    return processed;
}
