/*
 * apple-gfx-native.h — standalone, framework-FREE PVG (apple-gfx) device backend.
 *
 * This is the cross-platform deliverable described in docs/metal-vulkan-translator.md §0.7 / §4.2:
 * a reimplementation of the *consumer* (host) half of Apple's
 * ParavirtualizedGraphics.framework protocol that uses NO PGDevice / PGDisplay /
 * PG* / Metal / IOSurface symbols at all. It decodes the guest's gfx-BAR MMIO and
 * the 64 KiB index-ring FIFO, parses the 12-byte command header + the opcode
 * grammar, resolves the createTask / mapMemory guest-physical ranges, reaches the
 * DisplayTransaction3 (BGRA8 PRESENT) path, and drives the stamp/IRQ completion
 * channel.
 *
 * It is plain host-portable C (compiles on Linux/Windows-ARM later). All host
 * services it needs — guest-RAM access, IRQ injection, and the final framebuffer
 * present — are abstracted behind PGNativeHostOps so the same object file can be
 * wired into QEMU (apple-gfx-mmio.c) on a non-Apple host, OR driven by the offline
 * replay harness (the offline replay harness) for unit validation against the
 * captured trace a captured live trace.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QEMU_APPLE_GFX_NATIVE_H
#define QEMU_APPLE_GFX_NATIVE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ----------------------------------------------------------------------------
 * gfx-BAR register window (docs/metal-vulkan-translator.md §0.1). Registers live in the SECOND
 * page of the 0x4000 gfx BAR; the guest adds +0x1000 to every access. We expose
 * the absolute BAR offsets (0x1000.. ) so the decode matches the live trace 1:1.
 * --------------------------------------------------------------------------- */
#define PVG_GFX_BAR_SIZE          0x4000
#define PVG_IOSFC_BAR_SIZE        0x10000
#define PVG_REG_BASE              0x1000   /* register window start in the BAR  */

#define PVG_REG_FIFO_CONTROL      0x1000   /* W: 0=teardown else enableFifo     */
#define PVG_REG_FIFO_LENGTH       0x1004   /* W: setFifoLength (<=0x20000)       */
#define PVG_REG_FIFO_WRITTEN      0x1008   /* W: DOORBELL (publishes ring tail)  */
#define PVG_REG_FIFO_READ         0x100c   /* R: root-FIFO read index (head)     */
#define PVG_REG_FIFO_START        0x1010   /* W: setFifoStart                    */
#define PVG_REG_IRQ_CAUSE_1014    0x1014   /* R: atomic read+clear [+0x1c0]      */
#define PVG_REG_IRQ_CAUSE         0x1018   /* R: STAMP-PENDING / IRQ-CAUSE r+clr */
#define PVG_REG_ROOT_PAGE         0x101c   /* W: setRootPage -> createTask 16MB  */
#define PVG_REG_WAKE_FIFO         0x1020   /* W: wakeFifo:(child #val)           */
#define PVG_REG_RESUME_FIFO       0x1024   /* W: resumeFifo                      */
#define PVG_REG_RESUME_CHILD      0x1028   /* W: resumeChildFifo:                */
#define PVG_REG_FAULT             0x102c   /* R: readInterruptFault              */
#define PVG_REG_FIFO_BASE_PAGE    0x1030   /* W: setFifoBasePage (ring PFN)      */
#define PVG_REG_BINARY_VERSION    0x1034   /* W/R: setBinaryVersion              */
#define PVG_REG_NEW_INDIRECT_CB   0x1038   /* W: NewIndirectCommandBuffer        */
#define PVG_REG_FIFO_FAULT_OFF    0x103c   /* R: fifoFaultOffset                 */

/* EFI pre-GUI console block (0x1200..0x1234). */
#define PVG_REG_EFI_DISPLAY       0x1200
#define PVG_REG_EFI_MODE_SELECT   0x1208
#define PVG_REG_EFI_FB_START      0x1210
#define PVG_REG_EFI_FB_LENGTH     0x1214
#define PVG_REG_EFI_FB_DEPTH      0x1218
#define PVG_REG_EFI_FB_MODE       0x121c
#define PVG_REG_DISPLAY_DOORBELL  0x1220   /* W: ringDisplayDoorbellAtPort:      */
#define PVG_REG_EFI_FB_STRIDE     0x1228
#define PVG_REG_VERSION           0x122c   /* R: getEFIModeValue / version = 1    */

#define PVG_VERSION_VALUE         1        /* cross-validated 0x122c=1 fact      */

/* ----------------------------------------------------------------------------
 * iosfc BAR register window (docs/metal-vulkan-translator.md §0.7.2). Second device,
 * base 0x30210000, 64 KiB. Index decode is idx=(offset-0x1000)/8; registers
 * live in the second page of the BAR exactly like the gfx BAR. We expose the
 * absolute BAR offsets (0x1000.. ) so the decode matches the live trace 1:1.
 *
 * Confirmed numerically against a captured live trace:
 *   0x1000=0x79e98000 (ring base), 0x1010=0x79e94000 (descriptor base),
 *   0x1008=0x400 (ring size in entries), 0x1018/0x1020=0.
 * --------------------------------------------------------------------------- */
#define PVG_IOSFC_REG_RING_BASE     0x1000 /* idx0 W: setRingBase (cmd-ring GPA)  */
#define PVG_IOSFC_REG_RING_SIZE     0x1008 /* idx1 W: setRingSize (16-byte ents)  */
#define PVG_IOSFC_REG_DESC_BASE     0x1010 /* idx2 W: setDescriptorBase (GPA)     */
#define PVG_IOSFC_REG_DOORBELL      0x1018 /* idx3 W: tail index -> wakeDevice    */
#define PVG_IOSFC_REG_DESC_ENABLE   0x1020 /* idx4 W: setDescriptorEnable (& 1)   */
#define PVG_IOSFC_REG_STATUS        0x1028 /* idx5 R: status (dev+0x68 area)      */
#define PVG_IOSFC_REG_DESC2_BASE    0x1030 /* idx6 W: secondary descriptor/PFN    */

/* iosfc command-ring entry: 16 bytes (§0.7.2). */
#define PVG_IOSFC_RING_ENTRY_SIZE   0x10
#define PVG_IOSFC_ENT_OP            0x00   /* u32 op   1=map 2=unmap 3=test       */
#define PVG_IOSFC_ENT_SURFACE_ID    0x04   /* u32 surfaceID == mappingID          */
#define PVG_IOSFC_ENT_RESULT        0x08   /* u32 result (host writes back)        */
#define PVG_IOSFC_ENT_PAD           0x0c   /* u32 pad                              */

enum {
    PVG_IOSFC_OP_MAP_SURFACE   = 1,
    PVG_IOSFC_OP_UNMAP_SURFACE = 2,
    PVG_IOSFC_OP_TEST_SURFACE  = 3,
};

/*
 * iosfc surface descriptor — REAL 26.5 layout (, RE'd from guest kext
 * com.apple.iokit.AppleParavirtIOSurface IOSurfaceParavirtMapperDevice and LIVE-
 * CONFIRMED via QEMU xp). MMIO 0x1010 ("setDescriptorBase") is NOT a table of
 * 0x28-byte entries; it is a FLAT u32 PTE ARRAY indexed by descriptor SLOT =
 * surfaceID/descsPerPage. Each PTE = guestPFN<<2 | present(0x1), 16 KiB pages
 * (PVG_GUEST_PAGE_SHIFT=14). The geometry descriptor for a surface lives IN PLACE
 * at  desc_page_gpa + (surfaceID % descsPerPage) * 0x200 , where desc_page_gpa =
 * (pte[slot]>>2)<<14. surfaceID == ring mappingID. v1 (CLEAR) path: descStride
 * 0x200, descsPerPage = pagesize/0x200 = 32 @16 KiB (selected because MMIO 0x1028
 * STATUS reads 0). The descriptor is geometry-only (NO backing GPA, NO plane
 * sub-descriptor, planeCount==0); the framebuffer pixel pages are wired separately
 * via the GPU task page table (createTask rootGPA) and surface only via the
 * present/DisplayTransaction3 path.
 */
#define PVG_IOSFC_DESC_STRIDE_V1    0x200  /* per-surface descriptor slot stride  */
#define PVG_IOSFC_DESCS_PER_PAGE_V1 (PVG_GUEST_PAGE_SIZE / PVG_IOSFC_DESC_STRIDE_V1)
#define PVG_IOSFC_SURFACE_ID_MIN    1   /* 26.5 SW-compositor presents surfaceID 1; 0=none */
#define PVG_IOSFC_SURFACE_ID_MAX    0x2000

/* geometry header offsets (populate_descriptor, live-confirmed:
 * 'BGRA' 1920x1080 stride 0x1e00 alloc 0x7ec000). */
#define PVG_SDESC_PIXEL_FORMAT      0x04   /* u32 FourCC ('BGRA'=0x42475241)      */
#define PVG_SDESC_BASE_OFFSET       0x08   /* u32 byte offset within backing      */
#define PVG_SDESC_ALLOC_SIZE        0x10   /* u32 total backing size              */
#define PVG_SDESC_PACKED_WH         0x14   /* u64: elemW b0-7|width b8-31|elemH b32-39|height b40-63 */
#define PVG_SDESC_STRIDE            0x1c   /* u32 bytesPerRow                     */
#define PVG_SDESC_BPE               0x20   /* u16 bytesPerElement                 */
#define PVG_SDESC_PLANE_COUNT       0x24   /* u8                                  */

/* PTE word: (pte & 3)==1 means a present 4-byte PTE; GPA=(pte>>2)<<pageShift. */
#define PVG_IOSFC_PTE_PRESENT_MASK  0x3
#define PVG_IOSFC_PTE_PRESENT       0x1

/* Native scattered-page resolution . The WindowServer scanout IOSurface's
 * pixel pages are wired by the iosfc mapper-ring (wire_mapping@0x83167a8): per
 * page idx it packs PFN=GPA>>14 into a u32 PTE (bfi w9,w8,#2,#0x1e -> (PFN<<2)|
 * flags) into a per-surface PTE array. For 1080p (507 pages * 4 = 0x7EC bytes)
 * that array CANNOT be inline in the 0x200 descriptor slot, so it lives behind
 * an indirect GPA pointer stored in the descriptor (or the DESC2 overflow
 * table). The pointer offset is auto-discovered by scanning the descriptor for
 * a u64 whose target holds a coherent run of present PTEs; env overrides:
 *   APPLE_GFX_PTE_IND=<hex>  force indirect u64 ptr offset within the descriptor
 *   APPLE_GFX_PTE_OFF=<hex>  force inline u32 PTE-array offset within the descr
 *   APPLE_GFX_PTE_DESC2=1    take the PTE array from iosfc_desc2_gpa (0x1030) */
#define PVG_SDESC_PT_PTR_DEFAULT    0x18   /* the protocol notes §0.7.2 candidate B    */
#define PVG_SDESC_PTE_INLINE_DEFAULT 0x28  /* inline-trailer candidate A        */
#define PVG_NATIVE_PTE_VALIDATE_N   8      /* leading PTEs that must be coherent */

/*
 * FourCC pixel formats relevant to the present path. The framework stores the
 * IOSurfacePixelFormat as the 32-bit FourCC value 'BGRA' (per §0.7.2:
 * "BGRA8 -> 'BGRA' 0x42475241"); we compare against that value as read back by
 * rd_u32 from the descriptor's +0x04 field.
 */
#define PVG_FOURCC_BGRA             0x42475241u /* IOSurfacePixelFormat 'BGRA'     */

#define PVG_MAX_SURFACES            32 /* cached resolved surfaces in the table  */

/*
 * Apple Silicon guest uses 16 KiB pages, so the FIFO-base / root-page registers
 * carry a PFN that must be shifted left by 14 (NOT 12) to recover the GPA. This
 * is confirmed by the live trace: 0x101c=0x1e88c -> 0x7a230000 (root page,
 * createTask root); 0x1030=0x1e3e8 -> 0x78fa0000 (64 KiB ring). See §0.2.
 */
#define PVG_GUEST_PAGE_SHIFT      14
#define PVG_GUEST_PAGE_SIZE       (1u << PVG_GUEST_PAGE_SHIFT)

/* FIFO ring layout (§0.2): 64 KiB buffer, first 0x1000 is header/guard. */
#define PVG_FIFO_HDR_SIZE         0x1000
#define PVG_FIFO_HDR_ENABLE       0x00     /* =1 written LAST to arm the ring    */
#define PVG_FIFO_HDR_TAIL         0x08     /* producer WRITE index               */
#define PVG_FIFO_HDR_HEAD         0x0c     /* consumer READ index                */
#define PVG_FIFO_HDR_STRIDE       0x10     /* =0x1000 element stride / page size  */
#define PVG_FIFO_HDR_PFN          0x30     /* ring buffer PFN                    */

#define PVG_CREATETASK_VMSIZE     0x1000000 /* 16 MB createTask on RootPage      */

/*
 * Root-page stamp/fence completion table (docs/metal-vulkan-translator.md §0.5). The host
 * signalStamp:value:(idx,val) path writes `val` into stampTable[idx] at the
 * shared root-page VA [+0x1c8] and atomically sets bit idx in the pending
 * bitmask at [+0x1c0]; the guest's GPU IRQ handler reads/clears MMIO 0x1018
 * (= the [+0x1c0] bitmask) and then reads the per-stamp value directly from
 * this root-page RAM. The kernel's AppleParavirtDisplay::swap_begin_gated /
 * IOGPUCommandQueue::waitSharedEvent waiter (getStampBaseAddress = [gpu+0x370])
 * polls this RAM stamp value, so the backend MUST write it (not just pulse the
 * IRQ) or the high-VA swap_begin_gated spin never clears (the boot-wall analysis
 * §4/§5). Both offsets are relative to the root-page GPA (setRootPage 0x101c).
 */
#define PVG_ROOT_STAMP_BITMASK_OFF 0x1c0   /* u32 pending-stamp bitmask [+0x1c0] */
#define PVG_ROOT_STAMP_TABLE_OFF   0x1c8   /* u32 stampTable[] base     [+0x1c8] */
#define PVG_ROOT_STAMP_SLOT_STRIDE 0x4     /* u32 per-stamp value slot           */
#define PVG_ROOT_STAMP_MAX_SLOTS   0x20    /* stamp indices we mirror            */

/* ----------------------------------------------------------------------------
 * DISPLAY / SWAP completion — the dual-transport mailbox (the completion contract
 * §4 / §7). Distinct from the GPU exec/stamp channel above. The wedge is that
 * AppleParavirtDisplay::swap_begin_gated's clean_queue_gated stays busy until
 * notify_swap_done_gated clears [display+0x1790]; that runs ONLY off the DISPLAY
 * MAILBOX + the cause-0x14 display IRQ.
 *
 * The mailbox lives in guest RAM at [display+0x1d08] (the 2 KiB IOBuffer the guest
 * maps in setupSharedState). The guest publishes its GPA to the device via the
 * FIFO command DisplaySetSharedStatePage (op 0x01); we record it there. Layout
 * (offsets relative to the mailbox base GPA):
 *   +0x100  u32  cause word  — host sets the enabled notify bit(s); guest
 *                              ldclral-clears in handleHostInterrupt.
 *   +0x104  u32  enable mask — guest writes (start_hardware sets 0xc = bits 2|3).
 *   +0x200  u32  payload     — swap seq / vblank, read by the guest handler.
 *
 * On a swap-done the device writes the payload, sets the swap-done/notify bit in
 * the cause word (honoring the enable mask), sets this display's bit in the DISPLAY
 * cause register 0x14, and pulses the GPU IRQ. The guest reads+clears 0x14, runs
 * signalInterrupt -> handleHostInterrupt -> notify_swap_done_gated, which clears
 * [display+0x1790] so the next clean_queue_gated() returns "complete" and the
 * present queue drains.
 *
 * §8 ambiguity: the exact swap-done bit is not 100% disambiguated statically.
 * SAFE FIRST ATTEMPT (the contract's primary) = bit2 + cause 0x14. FALLBACK = the
 * display fence's stamp-slot leg (the existing 0x18 stamp path), which we ALSO
 * fire so the completion lands via whichever leg the guest is waiting on.
 */
#define PVG_DISP_MBOX_CAUSE_OFF    0x100  /* u32 cause word (host sets bits)     */
#define PVG_DISP_MBOX_ENABLE_OFF   0x104  /* u32 enable mask (guest writes)      */
#define PVG_DISP_MBOX_PAYLOAD_OFF  0x200  /* u32 payload (swap seq / vblank)     */
#define PVG_DISP_MBOX_SIZE         0x800  /* 2 KiB shared-state page             */

#define PVG_DISP_SWAP_DONE_BIT     2      /* primary swap-done/notify bit (bit2) */
#define PVG_DISP_DEFAULT_ENABLE    0xc    /* start_hardware enable (bits 2|3)    */

#define PVG_MAX_DISPLAYS           8      /* per-port display mailboxes          */

/* ----------------------------------------------------------------------------
 * CHILD FIFOs (CmdDefineChildFIFO op 0x30 / CmdDeleteChildFIFO op 0x31) and the
 * task OBJECT LISTs (CmdSetObjectList op 0x33). These are the channel-multiplexing
 * commands AppleParavirtGPU::setupChannels (kext 0x13108) + the per-display loop
 * (0x13218) + AppleParavirtTask::setResourceHeap (0x1cff0) emit on the ROOT ring
 * BEFORE the display's setupSharedState (0x9a5c) submits op 0x01.
 *
 * Wire (re-disassembled, KDK 26.5):
 *  - op 0x30 CmdDefineChildFIFO: 12-byte header + a 4-byte body = the channel id
 *    (AppleParavirtVirtualChannel::init 0x11814: getCommandBytesInt(len=4);
 *     ldr w8,[channel+0x68]; str w8,[body]). objectCount=0, length=0x10.
 *  - op 0x33 CmdSetObjectList: 12-byte header + a 12-byte APVCmdSetObjectList body
 *    (AppleParavirtTask::setResourceHeap 0x1cff0 -> commandDescriptor cmdID=0x33;
 *     getCommandBytes<APVCmdSetObjectList> 0xc bytes). objectCount=0, length=0x18.
 *
 * *** RE CORRECTION (KDK re-disassembly, was WRONG before) ***
 * The prior claim that "every channel flushes through the single shared ring +
 * 0x1008 doorbell" is FALSE. Only the AppleParavirtRootChannel uses the root ring
 * + 0x1008. The VIRTUAL channels (exec=1, object=2, memory=3, the per-display
 * channel = 4+displayIdx, info=0xc) each own a SEPARATE ring buffer and a SEPARATE
 * doorbell:
 *   - AppleParavirtVirtualChannel::init (kext 0x11424) allocs a per-channel ring
 *     IOBufferMemoryDescriptor (size = arg4, e.g. 0x4000 for the display channel),
 *     maps it, and stores the ring DATA VA at [chan+0x98] (= mappedVA + 0x4000;
 *     the first 0x4000 of the buffer is the header/guard, exactly like the root
 *     ring's setFifoStart=0x4000). Capacity (bytes) is at [chan+0x50].
 *   - The per-channel PRODUCER/CONSUMER indices + ring PFN live in a record array
 *     in the ROOT PAGE (setRootPage 0x101c page): record(id) =
 *     root_page_gpa + 0x400 + (id-1)*0x14, with:
 *         +0x00 u32  producer/WRITE index (tail, byte offset; guest stlr's it)
 *         +0x04 u32  consumer/READ index  (head, byte offset; DEVICE advances it)
 *         +0x10 u32  ring base PFN         (child_ring_gpa >> 14; guest publishes)
 *     (init: [chan+0x90] = root_va+0x400+(id-1)*0x14; str ringPFN,[rec+0x10];
 *      stlr wzr,[rec+0x4]; stlr wzr,[rec]. submitBufferLocked: stlr newTail,[rec];
 *      reads [rec+0x4] back as the consumer index for free-space.)
 *   - AppleParavirtVirtualChannel::submitBufferLocked (kext 0x11a3c) memcpy's the
 *     command into [chan+0x98] at the byte offset, publishes the new tail into
 *     rec+0x0, then writes the CHANNEL ID ([chan+0x68]) to regwin+0x20 =
 *     absolute 0x1020 = wakeFifo. So the doorbell for a virtual channel is
 *     wakeFifo(0x1020) = channel id, NOT the root 0x1008 doorbell.
 *
 * THEREFORE: AppleParavirtDisplay::setupSharedState submits op 0x01
 * (DisplaySetSharedStatePage) on CHANNEL 4's own ring and rings wakeFifo(4). The
 * backend MUST, on wakeFifo(child=N): read record(N) from the root page, resolve
 * channel N's ring (PFN<<14, data at +0x4000, capacity from the tracked size),
 * drain head->producer through pg_native_dispatch_one (so op 0x01 runs its echo +
 * mode-list + attr-IRQ path, and op 0x07 -> present_bgra8), then write the new
 * consumer index back to record(N)+0x4 so the guest sees the ring drained.
 *
 * We still record the op 0x30 CmdDefineChildFIFO channel id + the op 0x33
 * CmdSetObjectList binding (they flow on the root ring before setupSharedState),
 * and we ack their fences like every other command.
 * --------------------------------------------------------------------------- */
#define PVG_MAX_CHILD_FIFOS        16     /* tracked child FIFOs (channel ids)   */

/*
 * Child-FIFO ring layout (, re-disassembled). The per-channel index
 * record array lives in the root page (setRootPage 0x101c) at offset 0x400, with
 * a 20-byte (0x14) stride indexed by (channel_id - 1).
 */
#define PVG_CHILD_REC_BASE_OFF     0x400  /* index-record array off in root page  */
#define PVG_CHILD_REC_STRIDE       0x14   /* per-channel record stride (bytes)    */
#define PVG_CHILD_REC_PROD_OFF     0x00   /* u32 producer/WRITE index (tail)      */
#define PVG_CHILD_REC_CONS_OFF     0x04   /* u32 consumer/READ index (head)       */
#define PVG_CHILD_REC_PFN_OFF      0x10   /* u32 child ring base PFN (<<14 = GPA)  */
#define PVG_CHILD_RING_DATA_OFF    0x4000 /* ring data starts here in the buffer  */
#define PVG_CHILD_RING_DEFAULT_CAP 0x4000 /* display channel ring capacity bytes  */

/* ----------------------------------------------------------------------------
 * 12-byte command header + opcode grammar (§0.3).
 * --------------------------------------------------------------------------- */
typedef struct PGCmdHeader {
    uint16_t cmdID;        /* @0  opcode 0x00..0x45                      */
    uint16_t objectCount;  /* @2  trailing objectCount*8 object IDs      */
    uint32_t length;       /* @4  TOTAL length incl. 12-byte header      */
    uint32_t stampValue;   /* @8  completion fence value                 */
} PGCmdHeader;

#define PVG_CMD_HDR_SIZE          12

/* The load-bearing opcodes (full table in §0.3). */
enum {
    PVG_OP_CmdDebug              = 0x00,
    PVG_OP_DisplaySetSharedState= 0x01,
    PVG_OP_DisplayAck           = 0x02,
    PVG_OP_DisplayCursorGlyph   = 0x04,
    PVG_OP_DisplayCursorShow    = 0x05,
    PVG_OP_DisplayTransaction2  = 0x06, /* deprecated */
    PVG_OP_DisplayTransaction3  = 0x07, /* PER-FRAME PRESENT (BGRA8)   */
    PVG_OP_DisplaySwapMapping   = 0x08,
    PVG_OP_DisplaySleepState    = 0x09,
    PVG_OP_DisplaySetProperties = 0x0a,
    PVG_OP_NOP                  = 0x1e,
    PVG_OP_CmdDeleteTask        = 0x20,
    PVG_OP_CmdUnmapMemory       = 0x22,
    PVG_OP_CmdDeleteResource    = 0x25,
    PVG_OP_CmdDeleteObject      = 0x28,
    PVG_OP_CmdDefineChildFIFO   = 0x30,
    PVG_OP_CmdDeleteChildFIFO   = 0x31,
    PVG_OP_CmdSetObjectList     = 0x33,
    PVG_OP_CmdInvalidateRes     = 0x34,
    PVG_OP_CmdSyncResources     = 0x35,
    PVG_OP_CmdExecIndirect2     = 0x37, /* Metal draw stream (accel-only) */
    PVG_OP_CmdDefineTask2       = 0x38,
    PVG_OP_CmdMapMemory2        = 0x39,
    PVG_OP_CmdGetDeviceInfo     = 0x3a,
    PVG_OP_CmdGetComputeInfo    = 0x3b,
    PVG_OP_CmdReplacePhysical   = 0x3c,
    PVG_OP_CmdDelay             = 0x3d,
    PVG_OP_CmdExecIndirect3     = 0x43, /* Metal draw stream (accel-only) */
    PVG_OP_CmdSetObjAndPlace    = 0x44,
    PVG_OP_CmdInfoIndirect      = 0x45,
    PVG_OP_MAX                  = 0x46,
};

/*
 * DisplaySetSharedStatePage (0x01) payload, >=8 bytes. The AUTHORITATIVE wire
 * side is the live ARM guest PRODUCER, AppleParavirtDisplay::setupSharedState
 * (KDK 26.5, kext 0x9c88-0x9c90): `w8=fPort; w9=mailboxGPA>>14; stp w8,w9,[body]`
 * — i.e. { u32 port @0 ; u32 mailboxPFN @4 }, mailbox GPA = PFN<<14 (16 KiB page).
 *
 * NOTE: the x86 host-framework CONSUMER (-[PGFIFO CmdDisplaySetSharedStatePage:],
 * `ldp w3,w2,[payload]`, setSharedDisplayStatePage:(w3) forPort:(w2)) reads the
 * fields in the OPPOSITE order (page@0, port@4). The vmapple ARM guest is what
 * actually boots here, so the backend trusts the ARM producer layout below.
 */
typedef struct PGDisplaySetSharedState {
    uint32_t port;   /* @0  display port index (== fPort)                  */
    uint32_t page;   /* @4  shared-state page PFN (PFN<<14 = mailbox GPA)   */
} PGDisplaySetSharedState;

/*
 * Display shared-state mailbox layout consumed by AppleParavirtDisplay
 * (the display bring-up notes §1.4 / §3.2). All offsets relative to the
 * mailbox base GPA ([display+0x1d08] = PFN<<14):
 *   +0x00  u32  display number          (createDisplayAttributes appends as a prop)
 *   +0x12  u16  PORT ECHO               <- the device MUST write = port; this is
 *                                          the `fSharedState->port == fPort` gate
 *                                          that makes setupSharedState (hence
 *                                          AppleParavirtDisplay::start) return true.
 *   +0x208 u16  mode count              <- MUST be >= 1 (else empty-modeList fail)
 *   +0x210 u16  preferred width
 *   +0x212 u16  preferred height
 *   +0x218 ...  mode array, stride 0x10, element { u16 w@0; u16 h@2; u32 rate@4; }
 */
#define PVG_DISP_MBOX_NUM_OFF       0x0    /* u32 display number              */
#define PVG_DISP_MBOX_PORT_ECHO_OFF 0x12   /* u16 fSharedState->port echo     */
#define PVG_DISP_MBOX_MODE_COUNT_OFF 0x208 /* u16 mode count (>=1)            */
#define PVG_DISP_MBOX_PREF_W_OFF    0x210   /* u16 preferred width            */
#define PVG_DISP_MBOX_PREF_H_OFF    0x212   /* u16 preferred height           */
#define PVG_DISP_MBOX_MODE_ARRAY_OFF 0x218 /* mode timing-element array       */
#define PVG_DISP_MBOX_MODE_STRIDE   0x10    /* per-mode element stride        */
#define PVG_DISP_MODE_W_OFF         0x0     /* u16 width within an element    */
#define PVG_DISP_MODE_H_OFF         0x2     /* u16 height within an element   */
#define PVG_DISP_MODE_RATE_OFF      0x4     /* u32 refresh rate within element */

/* Default mode published to clear the modeList gate. : WindowServer composites
 * the desktop at 1920x1080 (the live IOSurface scanout buffers are 1920x1080), but
 * the display mode was published as 1280x800 — that mismatch means WS can't present
 * its 1920x1080 frame to a display it's told is 1280x800, so op 0x07 (the swap) is
 * never submitted. Align the published mode to the composited scanout geometry.
 * Override via APPLE_GFX_MODE=WxH@R. */
#define PVG_DISP_DEFAULT_WIDTH      1920
#define PVG_DISP_DEFAULT_HEIGHT     1080
#define PVG_DISP_DEFAULT_REFRESH    60

/* Mailbox cause bit2 triggers createDisplayAttributes in handleHostInterrupt. */
#define PVG_DISP_ATTR_BIT           2

/* CmdDefineChildFIFO (0x30) payload, >=4 bytes: { u32 channelID @0 }. */
typedef struct PGDefineChildFIFO {
    uint32_t channel_id;   /* @0 [channel+0x68] virtual-channel id        */
} PGDefineChildFIFO;

/*
 * CmdSetObjectList (0x33) payload, >=0xc bytes (APVCmdSetObjectList). The exact
 * field semantics beyond the leading u32s are not load-bearing for bring-up
 * (docs/metal-vulkan-translator.md §open-questions: "objectID->resource binding semantics ...
 * only matters if child FIFOs are exercised"); we record the leading words so the
 * task's object-list/resource-heap binding is acknowledged.
 */
typedef struct PGSetObjectList {
    uint32_t word0;        /* @0  */
    uint32_t word1;        /* @4  */
    uint32_t word2;        /* @8  */
} PGSetObjectList;

/* A tracked child FIFO, recorded from CmdDefineChildFIFO (op 0x30). The ring
 * geometry is resolved lazily on the first wakeFifo(channel) doorbell from the
 * per-channel index record in the root page . */
typedef struct PGChildFIFO {
    bool     valid;
    uint32_t channel_id;   /* virtual-channel id carried by the op-0x30 body */
    uint32_t capacity;     /* ring data capacity in bytes ([chan+0x50])      */
    uint32_t head;         /* our consumer cursor (byte offset into ring)    */
} PGChildFIFO;

/* CmdDefineTask2 (0x38) payload, >=0x10 bytes. */
typedef struct PGDefineTask2 {
    uint32_t taskID_flag;  /* @0  taskID<<1 | flag        */
    uint64_t taskRoot;     /* @4  (unaligned in wire)     */
    uint32_t length;       /* @0xc                        */
} PGDefineTask2;

/* CmdMapMemory2 (0x39) payload, >=0x14 bytes. */
typedef struct PGMapMemory2 {
    uint32_t taskID;       /* @0                          */
    uint64_t virtualOffset;/* @4                          */
    uint64_t length;       /* @0xc                        */
} PGMapMemory2;

/* CmdDisplayTransaction3 (0x07) payload, >=0x24 bytes. */
typedef struct PGDisplayTransaction3 {
    uint32_t port;                    /* @0    */
    uint32_t rsvd;                    /* @4 =0 */
    uint32_t surfaceID;               /* @8    */
    uint64_t gammaTableVirtualOffset; /* @0xc  */
    uint64_t gammaTableMappedLength;  /* @0x14 */
    uint32_t gammaTableEntryCount;    /* @0x1c */
    uint32_t gammaTableSum;           /* @0x20 */
} PGDisplayTransaction3;

/*
 * CmdGetDeviceInfo (0x3a) — the GPU device-info query.
 *
 * Decoded from AppleParavirtGPU::setupDeviceInfo (kext 0x13668) +
 * parseDeviceInfo (kext 0x1602c), KDK 26.5. The guest builds a 12-byte command
 * BODY and submits it, then synchronously parses a TLV RESPONSE the DEVICE must
 * write into a guest buffer the body points at:
 *
 *   body[0] u32 = format/version tag (0x2d == 45, the max field index + 1)
 *   body[4] u32 = response buffer length, in 8-byte units (size>>3)
 *   body[8] u32 = response buffer PFN (GPA = PFN<<14, 16 KiB page)
 *
 * The response is a flat array of 8-byte TLV entries {u32 type; u32 value}.
 * parseDeviceInfo walks `count` entries; for each, `type` (1..0x2a, 0x2c) selects
 * an APVDeviceInfoStruct u32 field at offset (type-1)*4 (0x00..0xa8) and sets the
 * "present" flag byte at 0xac+(type-1). type 0/0x2b are skipped. We answer with a
 * full table (types 1..0x2c) of sane GPU/display capabilities so the driver gets a
 * valid, non-empty device — without it parseDeviceInfo sees all-zero fields and
 * AppleParavirtGPU tears down + restarts in a loop (never reaching the display).
 */
typedef struct PGGetDeviceInfoBody {
    uint32_t version;       /* @0  == 0x2d (45) */
    uint32_t length_qwords; /* @4  response buffer size >> 3 */
    uint32_t buffer_pfn;    /* @8  response buffer GPA >> 14 */
} PGGetDeviceInfoBody;

#define PVG_DEVINFO_VERSION       0x2d  /* body[0] / number of field types */
#define PVG_DEVINFO_FIELD_COUNT   0x2c  /* highest valid TLV type (fields 1..0x2c) */
#define PVG_DEVINFO_TLV_STRIDE    8     /* each TLV entry = {u32 type; u32 value} */

#define PVG_MAX_TASKS   64
#define PVG_MAX_RANGES  256  /* per-task mapped guest-phys ranges */

/* A guest-physical range bound into a task's virtual address space. */
typedef struct PGRange {
    uint64_t virtual_offset; /* offset within the task VA window        */
    uint64_t phys_addr;      /* guest-physical address                  */
    uint64_t length;         /* byte length                             */
    bool     read_only;
} PGRange;

typedef struct PGTask {
    bool      in_use;
    uint32_t  task_id;       /* guest-assigned task ID (DefineTask2)     */
    uint64_t  vm_size;       /* reserved VA window size                  */
    uint64_t  task_root;     /* taskRoot (root page GPA for this task)   */
    uint32_t  range_count;
    PGRange   ranges[PVG_MAX_RANGES];
    /* Resource heap bound by CmdSetObjAndPlace (0x44 = setResourceHeap). The
     * Metal device's GPU resource heap (an IOGPUMemoryMap); object IDs in the
     * ExecIndirect stream resolve against it. */
    bool      heap_bound;
    uint32_t  heap_id;       /* body[0x00] (== task id in capture)       */
    uint64_t  heap_gpa;      /* body[0x0c] masked heap base (page+flags) */
    uint64_t  heap_size;     /* body[0x14] heap size                     */
} PGTask;

/* ----------------------------------------------------------------------------
 * Host abstraction. Implemented by the QEMU glue (apple-gfx-mmio.c) on a real
 * VMM, or by the replay harness for offline validation. The backend NEVER calls
 * an Apple framework symbol; everything host-specific funnels through here.
 * --------------------------------------------------------------------------- */
typedef struct PGNativeHostOps {
    void *opaque;

    /*
     * Return a directly-dereferenceable host pointer for a guest-physical range,
     * or NULL if it can't be mapped directly (the backend then falls back to
     * read_mem). On QEMU this is apple_gfx_host_ptr_for_gpa_range(); in the
     * harness it indexes a flat model RAM buffer.
     */
    void *(*host_ptr)(void *opaque, uint64_t gpa, uint64_t len, bool read_only);

    /* Bulk DMA read of guest RAM into dst (fallback when host_ptr is NULL). */
    bool (*read_mem)(void *opaque, uint64_t gpa, uint64_t len, void *dst);

    /*
     * Read guest KERNEL-virtual memory: translate kva->gpa via the live CPU MMU
     * (QEMU's own page-table walker over TTBR1) then DMA-read. Reliable for the
     * kernel heap (unlike the gdb-stub). Used by the op 0x43 kernel-object walk
     * to resolve the GPU command-buffer GPA. NULL on the harness; returns false
     * if the VA is unmapped. (robust VA->GPA path.)
     */
    bool (*read_kva)(void *opaque, uint64_t kva, uint64_t len, void *dst);

    /*
     * Read a register of the CPU that triggered the current MMIO handler
     * (current_cpu): idx 0..30 = X0..X30, 31 = PC, 32 = SP. Anchors the op 0x43
     * kernel walk (a live GPU/queue `this` pointer + the KASLR slide from PC/LR
     * vs a known kext callsite) without the gdb-stub. NULL on harness; 0 if none.
     */
    uint64_t (*cpu_reg)(void *opaque, int idx);

    /* Pulse the device's edge IRQ line (raiseInterrupt(vector)). */
    void (*raise_irq)(void *opaque, uint32_t vector);

    /*
     * Present a BGRA8 surface. src points to width*height*4 BGRA8 bytes (host
     * memory, already resolved from guest RAM by the backend). On QEMU this
     * wraps a DisplaySurface + dpy_gfx_replace_surface/update; in the harness it
     * records the call for assertions.
     */
    void (*present_bgra8)(void *opaque, uint32_t width, uint32_t height,
                          uint32_t stride, const void *src);

    /* Optional: mode change notification (width/height/pixelFormat). */
    void (*mode_change)(void *opaque, uint32_t width, uint32_t height,
                        uint32_t pixel_format);

    /* Optional: structured log sink. If NULL, the backend logs to stderr. */
    void (*log)(void *opaque, const char *line);
} PGNativeHostOps;

/* ----------------------------------------------------------------------------
 * A resolved iosfc surface: what op_display_transaction3 needs to PRESENT.
 * Built by mapSurface (iosfc ring op==1): the surfaceID -> {plane-0 host VA or
 * GPA, width, height, stride, pixelFormat}. host_va is the directly-presentable
 * BGRA8 pointer (NULL if the plane pages weren't contiguously host-mappable, in
 * which case base_gpa + the page list let a backend gather them).
 */
typedef struct PGSurface {
    bool      valid;
    uint32_t  surface_id;
    uint32_t  pixel_format;   /* FourCC                                   */
    uint32_t  width;          /* plane[0] width  (px)                     */
    uint32_t  height;         /* plane[0] height (px)                     */
    uint32_t  stride;         /* plane[0] bytesPerRow                     */
    uint64_t  base_gpa;       /* plane[0] first-page GPA + plane offset    */
    void     *host_va;        /* plane[0] host pointer (NULL if ungathered)*/
    bool      read_only;
    uint64_t  desc_gpa;       /* iosfc descriptor GPA (proof-of-life)     */
    uint32_t  base_offset;    /* SDESC base_offset (plane[0] offset)      */
    /* Native scattered-page resolution : the WindowServer scanout IOSurface
     * is kIOMemoryTypeVirtual — physically scattered 16KiB pages. mapSurface
     * walks the iosfc PTE array to resolve each page's GPA; present gathers them
     * into a contiguous staging buffer. page_gpas==NULL => unresolved (FBSCAN). */
    uint64_t *page_gpas;      /* per-page GPA vector (length page_count)   */
    uint32_t  page_count;     /* ceil(stride*height / 0x4000)             */
    bool      contiguous;     /* page_gpas[i+1]==page_gpas[i]+0x4000 ∀i    */
} PGSurface;

/* ----------------------------------------------------------------------------
 * A per-port display: its shared-state mailbox GPA (recorded from
 * DisplaySetSharedStatePage op 0x01) + the enable mask the device honors when
 * delivering swap-done notifications. swap_seq is the monotonically-increasing
 * payload written into the mailbox per present.
 * --------------------------------------------------------------------------- */
typedef struct PGDisplay {
    bool     valid;
    uint32_t port;
    uint64_t mbox_gpa;     /* [display+0x1d08] shared-state mailbox GPA */
    uint32_t swap_seq;     /* incremented + published per swap-done     */
    uint32_t vblank_seq;   /* monotonic vblank/frame counter  */
} PGDisplay;

/* IRQ vectors (GIC SPI mapping is the VMM's job; these are logical). */
#define PVG_IRQ_GFX    0x0
#define PVG_IRQ_IOSFC  0x10

/* ----------------------------------------------------------------------------
 * Backend object.
 * --------------------------------------------------------------------------- */
/* Software-GPU (lavapipe) translator context, owned by the device. Opaque here;
 * the real definition lives in apple-gfx-vk.c. Forward-declared (not #include'd)
 * to avoid a circular include — apple-gfx-vk.h includes THIS header. The native
 * .c includes apple-gfx-vk.h to reach pg_vk_ops()/PGVkOps. */
typedef struct PGVkContext PGVkContext;

typedef struct PGNativeDevice {
    const PGNativeHostOps *ops;

    /* Software-Vulkan (lavapipe) translator context (apple-gfx-vk.c), or NULL
     * when CONFIG_PVG_VULKAN is off / the lavapipe context failed to come up. */
    PGVkContext *vk_ctx;

    /* gfx-BAR register shadow (only the few that read back). */
    uint32_t binary_version;        /* 0x1034 */
    uint32_t fifo_length;           /* 0x1004 */
    uint32_t fifo_start;            /* 0x1010 */
    uint32_t fifo_base_pfn;         /* 0x1030 */
    uint32_t root_page_pfn;         /* 0x101c */
    bool     fifo_enabled;          /* 0x1000 */
    uint32_t efi_display;           /* 0x1200 */

    /* Resolved FIFO geometry. */
    uint64_t ring_gpa;              /* fifo_base_pfn << 14                  */
    uint64_t ring_len;             /* fifo_length                          */
    uint64_t root_page_gpa;         /* root_page_pfn << 14                  */
    uint32_t fifo_head;             /* our read cursor (0x100c readback)    */
    uint32_t fifo_tail;             /* last doorbell value (0x1008)         */

    /* Stamp / IRQ completion channel (§0.5). */
    uint32_t irq_cause;             /* pending-stamp bitmask read+clear @0x1018 */

    /*
     * DISPLAY / SWAP completion channel (the completion contract §2/§4). Distinct
     * from the stamp cause above: 0x14 is the display cause (per-port bitmask),
     * read-and-clear, separate register from 0x18.
     */
    uint32_t disp_cause;            /* display cause bitmask read+clear @0x1014 */
    PGDisplay displays[PVG_MAX_DISPLAYS];
    uint32_t  display_count;

    /* GetDeviceInfo (op 0x3a) version tag latched from the request: 0x2d=26.5,
     * 0xe=Monterey 12.6. Used to gate ver-specific protocol quirks (e.g. the
     * op 0x9 DisplaySleepState attr-redrive Monterey needs but 26.5 doesn't). */
    uint32_t  devinfo_version;

    /*
     * Child FIFOs (CmdDefineChildFIFO op 0x30) + the most-recent object list
     * (CmdSetObjectList op 0x33). Recorded so the display's virtual channel is a
     * known/bound child FIFO before its op 0x01 (DisplaySetSharedStatePage) flows
     * through the shared ring; without this the device left them "(unhandled)".
     */
    PGChildFIFO child_fifos[PVG_MAX_CHILD_FIFOS];
    uint32_t    child_fifo_count;
    bool        object_list_bound;     /* a CmdSetObjectList has been recorded   */
    PGSetObjectList last_object_list;   /* most-recent op-0x33 body (leading u32s) */

    /* Implicit task #0: the createTask(16MB) reservation on RootPage. */
    PGTask   tasks[PVG_MAX_TASKS];
    uint32_t task_count;

    /* ---- iosfc BAR state (§0.7.2): IOSurface registration sub-device. ---- */
    uint64_t iosfc_ring_gpa;    /* 0x1000 setRingBase (cmd ring GPA)         */
    uint32_t iosfc_ring_size;   /* 0x1008 setRingSize (entries, *0x10 bytes) */
    uint64_t iosfc_desc_gpa;    /* 0x1010 setDescriptorBase (surface table)  */
    uint64_t iosfc_desc2_gpa;   /* 0x1030 secondary descriptor base          */
    bool     iosfc_enabled;     /* 0x1020 setDescriptorEnable & 1            */
    uint32_t iosfc_ring_head;   /* our consumer cursor (entries)             */
    uint32_t iosfc_ring_tail;   /* last 0x1018 doorbell value (entries)      */
    uint32_t iosfc_status;      /* 0x1028 readback                           */

    /* Cached resolved surfaces, keyed by surfaceID (built by mapSurface). */
    PGSurface surfaces[PVG_MAX_SURFACES];
    uint32_t  surface_count;

    /* Validation / introspection counters (used by the replay harness). */
    struct {
        uint64_t mmio_reads, mmio_writes;
        uint64_t doorbells;
        uint64_t commands_parsed;
        uint64_t define_task, map_memory, exec_indirect, present;
        uint64_t stamps_signaled, irqs_raised;
        uint64_t createtask_cb, mapmemory_cb;
        uint64_t parse_errors;
        uint32_t last_present_surface_id;
        uint32_t last_present_stamp;
        /* iosfc surface-resolution counters. */
        uint64_t iosfc_mmio_writes;
        uint64_t iosfc_doorbells;
        uint64_t iosfc_ring_entries;     /* entries drained from the iosfc ring  */
        uint64_t surfaces_mapped;        /* op==1 mapSurface successes           */
        uint64_t surfaces_unmapped;
        uint64_t surface_map_errors;
        uint64_t present_real_pixels;    /* presents that resolved a real surface */
        uint32_t last_present_width;
        uint32_t last_present_height;
        uint32_t last_present_stride;
        uint32_t last_present_format;
        /* display-swap completion counters (the swap_begin_gated unwedge). */
        uint64_t set_shared_state;       /* DisplaySetSharedStatePage (op 0x01)  */
        uint64_t port_echoes;            /* mbox[0x12] port echoes (clears start) */
        uint64_t mode_lists_published;   /* mode lists written into the mailbox  */
        uint64_t display_attr_irqs;      /* display-attr IRQs (createDisplayAttrs) */
        uint64_t swap_done_mailbox;      /* mailbox writes on swap completion    */
        uint64_t disp_cause_raised;      /* 0x14 display-cause bits set          */
        uint32_t last_swap_port;
        uint32_t last_swap_seq;
        uint32_t last_mbox_cause;        /* cause word value we wrote            */
        /* CmdGetDeviceInfo (op 0x3a): the device-info TLV response. */
        uint64_t device_info_responses;  /* 0x3a responses written into guest RAM */
        uint32_t last_device_info_pfn;   /* response buffer PFN (payload[8])     */
        uint32_t last_device_info_count; /* TLV entry count written              */
        /* Channel multiplexing: CmdDefineChildFIFO (0x30) / CmdSetObjectList (0x33). */
        uint64_t child_fifos_defined;    /* op 0x30 CmdDefineChildFIFO handled    */
        uint64_t child_fifos_deleted;    /* op 0x31 CmdDeleteChildFIFO handled    */
        uint64_t object_lists_set;       /* op 0x33 CmdSetObjectList handled      */
        uint32_t last_child_fifo_id;     /* channel id from the latest op 0x30    */
        /* Child-FIFO ring drain : wakeFifo(child=N) drains channel
         * N's own ring (op 0x01 DisplaySetSharedStatePage / op 0x07 present). */
        uint64_t wake_fifos;             /* wakeFifo (0x1020) doorbells seen      */
        uint64_t child_ring_drains;      /* child rings actually drained          */
        uint64_t child_ring_indirect;    /* indirect child-ring layout followed  */
        uint64_t child_cmds_dispatched;  /* commands dispatched off child rings   */
        uint32_t last_wake_channel;      /* the latest wakeFifo channel id        */
        uint64_t display_acks;           /* op 0x02 DisplayAck handled            */
        /* Per-channel completion-stamp fences . setupDeviceInfo's
         * AppleParavirtChannel::addCommand arms a fence on a NON-zero stamp slot
         * (the device-info channel's [chan+0x18]) and IOGPUEventMachine::finishEvent
         * -> waitForStamp blocks until stampBaseArray[idx] >= target. Our backend
         * historically only advanced root slot 0, so that wait spun forever. We now
         * advance the per-channel fence slots too. */
        uint64_t channel_fences_advanced; /* per-channel fence-slot stamp writes   */
        uint32_t last_fence_slot;         /* highest channel-fence slot we advanced */
        uint32_t last_fence_value;        /* value written to channel-fence slots   */
        /* Periodic display vblank / frame-ready notification . The
         * device pulses this on a ~60Hz host timer once a display mailbox is live
         * (post-createDisplayAttributes) to drive the guest's refresh/swap loop so
         * it submits the first DisplayTransaction3 present. */
        uint64_t vblank_ticks;            /* vblank pulses delivered to a display   */
        uint32_t last_vblank_seq;         /* latest vblank seq written to +0x200    */
    } stats;
    /* Monotonic running stamp value per root-page slot. The kernel's testEvent /
     * waitForStamp compares the top 24 bits (value & 0xffffff00); we keep each slot
     * monotonically non-decreasing so a later, smaller-target completion can never
     * regress a slot below an already-satisfied fence. */
    uint32_t stamp_slot_value[PVG_ROOT_STAMP_MAX_SLOTS];

    /* Gather-present staging buffer : scattered scanout pages are copied
     * here (one max-size width*height*4 alloc, reused per frame) before
     * present_bgra8 hands a contiguous BGRA buffer to the display console. */
    uint8_t  *present_scratch;
    size_t    present_scratch_len;
} PGNativeDevice;

/* Lifecycle. */
void     pg_native_init(PGNativeDevice *d, const PGNativeHostOps *ops);
void     pg_native_reset(PGNativeDevice *d);

/* gfx-BAR MMIO decode (offset is the absolute BAR offset, e.g. 0x101c). */
uint32_t pg_native_mmio_read(PGNativeDevice *d, uint64_t offset, unsigned size);
void     pg_native_mmio_write(PGNativeDevice *d, uint64_t offset,
                              uint32_t value, unsigned size);

/*
 * Drain the FIFO ring up to the current tail. Normally invoked by the 0x1008
 * doorbell, but also exposed so a polling backend (§0.5 dual transport) can call
 * it. Returns the number of commands dispatched.
 */
uint32_t pg_native_fifo_drain(PGNativeDevice *d);

/*
 * periodic display vblank / frame-ready notification.
 *
 * After createDisplayAttributes/DisplayAck the guest's GPU + display trace goes
 * quiet: it has published its mode list + acked its attributes and is now waiting
 * for the per-frame present to be driven. A real display device delivers a
 * periodic vblank/refresh interrupt; the guest's refresh loop (WindowServer's
 * CADisplayLink / IOMFB swap loop) consumes it to schedule + submit the first
 * DisplayTransaction3 (op 0x07) present.
 *
 * Our backend never emitted this, so the refresh never ticked. pg_native_vblank()
 * pulses a vblank on every display that has a live mailbox: it writes a monotonic
 * vblank seq into the display mailbox payload (+0x200), sets the enabled notify
 * bit in the mailbox cause word (+0x100), sets this display's bit in the DISPLAY
 * cause register 0x14, and raises the GPU IRQ — reusing the same dual-transport
 * machinery as a swap-done completion. It is a safe no-op until at least one
 * display mailbox is published (i.e. before op 0x01 DisplaySetSharedStatePage).
 *
 * Returns the number of displays pulsed (0 = nothing live yet). A QEMU-side
 * ~60Hz timer (apple-gfx-mmio.m) calls this so the refresh loop is driven.
 */
uint32_t pg_native_vblank(PGNativeDevice *d);

/*
 * Parse a single 12-byte command header + dispatch one command from a contiguous
 * host buffer (buf has at least len bytes). Returns the consumed length (the
 * command's total length field, padded), or 0 on a parse error. Exposed for the
 * unit harness to feed synthetic / captured command blobs directly.
 */
uint32_t pg_native_dispatch_one(PGNativeDevice *d, const uint8_t *buf,
                                uint32_t len);

/* Look up a task by guest task ID; NULL if not present. */
PGTask  *pg_native_find_task(PGNativeDevice *d, uint32_t task_id);

/*
 * Look up a per-port display (with its recorded shared-state mailbox GPA) by
 * port index; NULL if the guest has not yet sent DisplaySetSharedStatePage for
 * that port. Exposed for the QEMU glue and the replay harness.
 */
PGDisplay *pg_native_find_display(PGNativeDevice *d, uint32_t port);

/*
 * Look up a tracked child FIFO by channel id (recorded from CmdDefineChildFIFO
 * op 0x30); NULL if no such child FIFO has been defined. Exposed for the harness.
 */
PGChildFIFO *pg_native_find_child_fifo(PGNativeDevice *d, uint32_t channel_id);

/*
 * Drain a virtual-channel (child) FIFO ring. Invoked on a wakeFifo(0x1020)=N
 * doorbell : resolves channel N's ring from the per-channel index
 * record in the root page, dispatches head->producer through
 * pg_native_dispatch_one, and writes the new consumer index back. Returns the
 * number of commands dispatched. Exposed for the harness.
 */
uint32_t pg_native_child_fifo_drain(PGNativeDevice *d, uint32_t channel_id);

/*
 * Resolve a (task-relative) virtual offset to a guest-physical address using the
 * task's mapped ranges. Returns true and fills *gpa on success.
 */
bool     pg_native_task_resolve(const PGTask *t, uint64_t virtual_offset,
                                uint64_t *gpa);

/* ----------------------------------------------------------------------------
 * iosfc BAR (second device, 0x30210000) — IOSurface registration (§0.7.2).
 * --------------------------------------------------------------------------- */

/* iosfc-BAR MMIO decode (offset is the absolute BAR offset, e.g. 0x1010). */
uint32_t pg_native_iosfc_mmio_read(PGNativeDevice *d, uint64_t offset,
                                   unsigned size);
void     pg_native_iosfc_mmio_write(PGNativeDevice *d, uint64_t offset,
                                    uint32_t value, unsigned size);

/*
 * Drain the iosfc command ring up to the current tail (entries). Normally
 * invoked by the 0x1018 doorbell; also exposed for a polling backend. Returns
 * the number of ring entries processed.
 */
uint32_t pg_native_iosfc_ring_drain(PGNativeDevice *d);

/*
 * Look up a resolved surface by surfaceID in the cached table; NULL if the
 * surface has not been mapped (op==1) or is invalid.
 */
PGSurface *pg_native_find_surface(PGNativeDevice *d, uint32_t surface_id);

/*
 * Resolve a surfaceID into the table by reading its descriptor + plane[0] page
 * table from guest RAM (the mapSurface path). Returns the cached PGSurface on
 * success, NULL on failure (invalid ID / unreadable descriptor). Exposed so the
 * QEMU glue and the harness can resolve a surface directly.
 */
PGSurface *pg_native_map_surface(PGNativeDevice *d, uint32_t surface_id);

#endif /* QEMU_APPLE_GFX_NATIVE_H */
