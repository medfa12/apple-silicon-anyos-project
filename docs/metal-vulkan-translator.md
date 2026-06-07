# Metal-command-stream → Vulkan translator (software / lavapipe)

This documents the modern-machine graphics path: a **from-scratch, framework-free
reimplementation of the Apple paravirtual-graphics (PVG) device** plus a
**translator that turns the guest's Metal-derived GPU command stream into Vulkan**,
executed on the CPU through Mesa **lavapipe** (llvmpipe software Vulkan).

The result is a real macOS composite frame rendered on a **non-Apple ARM host with
no GPU at all** — proven on a non-Apple aarch64 Linux host running Mesa lavapipe
(software Vulkan, no GPU). No Apple `ParavirtualizedGraphics.framework`, no
Metal, no GPU.

> **Reproducible, with no material from us.** Everything below is original analysis
> and original GPLv2 code. To render a real frame you must supply **your own** macOS
> restore image (one you are licensed to use) and your own guest; we ship no Apple
> binaries, kernelcache, memory dumps, device-tree, or images. macOS, Metal, and
> Apple Silicon are trademarks of Apple Inc.; this is interoperability research.

The complete, self-contained source set is in
**[`../gpu-translator/`](../gpu-translator/)**: the translator
(`apple-gfx-vk.c`), the framework-free PVG backend it plugs into
(`apple-gfx-native.c` / `.h`), the interface seam (`apple-gfx-vk.h`), and the
embedded present shaders (`pvg_vk_shaders.h`) — all original GPLv2.

---

## Why a translator is needed

On the modern `vmapple` machine the guest's display stack is GPU-accelerated. A
software framebuffer paints **only the wallpaper**; the actual UI — the
Setup-Assistant dialog, text, icons, controls — is composited by the guest's
WindowServer through Metal and submitted to the PVG device as a GPU command stream.
The present/swap path (the "display transaction") and the surface→pixels mapping can
be made to work on their own, but they have nothing to show until the **render**
command stream is executed. So a non-Apple host must actually run that Metal work.
Doing it on a GPU-less host means translating it to software Vulkan (lavapipe).

The host PVG device submits accelerated GPU work via one opcode we call **op-0x37**
(`CmdExecIndirect2`). The present/swap is a separate opcode (the display
transaction). This document is about op-0x37 — the render stream — and how the
translator replays it into Vulkan.

---

## 0. The two-layer model — what op-0x37 actually is

op-0x37 is **two nested layers**:

```
op-0x37 SUBMISSION DESCRIPTOR  (ring layer — names a task + residency set + a pointer)
  └─ innerStreamGPA / innerStreamLen  ─────────►  INNER METAL STREAM  (the real encoder commands)
                                                  └─ command-segment header (encoderType = Render)
                                                     └─ length-framed records: SetRenderPipelineState,
                                                        SetVertex/FragmentBuffers, SetFragmentTextures,
                                                        SetFragmentSamplerStates, SetScissorRect,
                                                        DrawIndexedPrimitives16, ...
```

One op-0x37 submit means: *"run the Metal command buffer at `innerStreamGPA` for
this task, with this residency set resident, writing into render-target
`dstObjectID`."* WindowServer issues **one submit per composite surface per frame**.

### 0.1 op-0x37 submission descriptor (the ring layer)

Variant-0 / taskID = 1 layout (offsets are within the residency header):

```
+0x00  u64  innerStreamGPA      // GPA of the inner Metal command buffer (rotates per frame)
+0x08  u64  innerStreamLen      // byte length of the inner stream (≈0x290..0x4e4 observed)
+0x10  u32  op                  // 0x00010037 = (variant<<16)|0x0037
+0x14  u32  payloadLen          // residency-set payload bytes
+0x18  u64  stampValue          // rolling fence stamp; ascends ~0x100 per frame-surface
+0x20  u32  count2              // sub-list / residency count token
+0x24  u32  taskID              // 1 for this guest
+0x28  u32  dstObjectID         // RENDER-TARGET objectID (the surface this submit writes)
+0x2c  u32  usage               // 0x1 for all present/composite submits
        ENTRY[] residency set   // wide 24B entries {u32 objectID; u32 usage; u64; u64}
                                //   first entry usage 0x100/0x101 = dst; rest = read sources
```

Higher variants (taskID = 3, op `0x00020037`) insert an extra residency sub-list
before `taskID`, shifting the `+0x24/+0x28` columns. For those, re-anchor on the
`{innerGPA, innerLen, 0x..0037}` triplet rather than the fixed offsets. The
translator's header parser reads the trailing `{innerStreamGPA, innerStreamLen}`
u64s (the only fields it needs to find + resolve the inner stream) and best-effort
reads `dstObjectID` at `+0x28` with a fallback to the first residency entry.

### 0.2 Inner-stream framing

- **Outer:** a `u64 protectionOptions`, then a command-segment header
  `{u32 byteLength; u8 encoderType; u8 hasCommands; u8 isFinal}`. For these
  composite frames the whole stream is **one Render segment** (encoderType = Render).
  A single resolved stream may carry several concatenated Render segments; each is
  prefixed by its own segment header, and the translator resyncs across them.
- **Per record:** `{u32 opcode; u32 length}` where `length` is the TOTAL bytes
  *including* the 8-byte header (`operandLen = length - 8`). Records are
  self-delimiting; walk `next = cur + length`.
- **Every resource operand is a `u32` global reference** into a resource table
  (an objectID), **not** a pointer or GPA.

---

## 1. The render subset — 9 opcodes are the entire requirement

A full composite frame decodes to a **fixed vocabulary of 9 distinct opcodes**. No
blit, no compute, no draw type other than indexed triangles. The frame is, in
substance, *bind pipeline → bind a quad's vertex/uv/transform buffers → bind a
source texture + sampler → set scissor → draw 6 indices (one textured quad)*,
repeated ~24× with different pipeline / texture / scissor. That repetition is the
composited UI.

| op   | command                  | record len | operand layout                                                             |
|------|--------------------------|------------|-----------------------------------------------------------------------------|
| 0x74 | SetRenderPipelineState   | 12 (0x0c)  | `{u32 pipelineRef}`                                                          |
| 0x75 | SetScissorRect           | 40 (0x28)  | `{u64 x; u64 y; u64 width; u64 height}`                                      |
| 0x6e | SetFragmentBuffers       | 28 (0x1c)  | `{u32 start; u32 count}` + count×`{u32 bufRef; u64 offset}`                  |
| 0x6f | SetFragmentBufferOffset  | 20 (0x14)  | `{u32 index; u64 offset}`                                                    |
| 0x70 | SetFragmentSamplerStates | 20 (0x14)  | `{u32 start; u32 count}` + count×`{u32 samplerRef}`                          |
| 0x72 | SetFragmentTextures      | 20 (0x14)  | `{u32 start; u32 count}` + count×`{u32 texRef}`                              |
| 0x7d | SetVertexBuffers         | 28 (0x1c)  | `{u32 start; u32 count}` + count×`{u32 bufRef; u64 offset}`                  |
| 0x7e | SetVertexBufferOffset    | 20 (0x14)  | `{u32 index; u64 offset}`                                                    |
| 0x07 | DrawIndexedPrimitives16  | 20 (0x14)  | `{u32 primType; u32 indexBufRef; u16 indexCount; u16; u32 idxBufOffset}`     |

`DrawIndexedPrimitives16` carries `primType = 3` (triangle list), an index-buffer
ref, and a `u16 indexCount`. The textured-quad draws use `indexCount = 6` (two
triangles = one quad); mesh draws use larger counts (0x30 / 0x36 / 0x12 / 0x0c).
Indices are `uint16`.

### Frame shape (one representative composite frame)

A representative full composite frame is one Render segment of ~157 records. Its
opcode histogram:

```
SetVertexBufferOffset×45, SetFragmentTextures×29, DrawIndexedPrimitives16×24,
SetScissorRect×21, SetFragmentBuffers×10, SetRenderPipelineState×9,
SetFragmentBufferOffset×8, SetVertexBuffers×6, SetFragmentSamplerStates×5
```

Structurally it is a small number of blocks: a full-screen background quad, a large
icon/atlas blend, a run of ~17 glyph/control quads (the text + buttons, scissor
stepping the layout), and a few final overlay passes. Each block is the same
bind→draw-6 pattern under a different pipeline + source texture.

---

## 2. What the stream does and does NOT contain

op-0x37 carries resource **references** (objectIDs) and bind/draw **structure**. It
does **not** carry resource *contents*:

- **Texture pixels, buffer bytes, pipeline-state-object contents** (shaders, blend,
  vertex descriptor, color format) are **not** in the stream. They are defined by the
  guest's **object-creation ring** records (task-define / map-memory / texture &
  pipeline creates) and live at GPAs the create records and the residency set name.
- **The render-pass attachment descriptor** is parsed out-of-band at encoder-begin,
  not in the dispatch loop. For a single-color-attachment composite the translator
  synthesizes it (1 color attachment = the dst surface, clear/load → store, no
  depth/stencil).

This is the one bounded external dependency. The recommended first milestone (and
what the shipped translator does by default) is to **replay the draw structure with
stub textures** — solid/procedural sources keyed by the recognized objectIDs —
into a BGRA8 target. That validates the entire pipeline/draw/present path before
wiring real contents.

For **real pixels**, you supply the resource contents yourself, from *your own*
guest: the destination/scanout surfaces are stored **linear** in guest RAM (BGRA8),
so they can be carved out without any GPU-side decompression. The translator can
load such a raw BGRA8 file as the sampled source via the `APPLE_GFX_VK_REAL_TEX`
env var (`path` or `path:WxH`). Source *sub*-resources (icon atlas, glyph textures)
are GPU-side render-target-compressed and are not extractable offline; the linear
destination surfaces are. (This is why the proven real frame samples the linear
composited destination surface — the ground-truth output the whole stream produces.)

---

## 3. How the translator maps each opcode to Vulkan

The render subset maps **1:1** onto core Vulkan. The translator
(`apple-gfx-vk.c`) brings up a headless lavapipe context (instance → CPU/llvmpipe
physical device → device + single universal queue → command pool → timeline
semaphore → host-visible readback buffer → completion thread), then walks the inner
stream and replays it:

| op-0x37 command          | Vulkan                                                             |
|--------------------------|-------------------------------------------------------------------|
| 0x74 SetRenderPipelineState | `vkCmdBindPipeline` (graphics)                                  |
| 0x7d / 0x6e SetVertex/FragmentBuffers | descriptor-set / vertex-buffer binds                  |
| 0x7e / 0x6f buffer offsets | bind offsets / dynamic offsets                                  |
| 0x72 SetFragmentTextures | sampled-image descriptors                                          |
| 0x70 SetFragmentSamplerStates | sampler descriptors (combined image-sampler)                 |
| 0x75 SetScissorRect      | `vkCmdSetScissor`                                                  |
| 0x07 DrawIndexedPrimitives16 | `vkCmdBindIndexBuffer(VK_INDEX_TYPE_UINT16)` + `vkCmdDrawIndexed` |

Rendering uses **dynamic rendering** (`VK_KHR_dynamic_rendering`) into a cached
BGRA8 color image — no render-pass/framebuffer objects. Completion is a
**timeline semaphore** (`VK_KHR_timeline_semaphore`): each submit signals a
monotonic value; a completion thread blocks on it, then drives the present + the
guest's fence stamp, so stamps signal in guest order.

### The recognizer + present path (the shipped first frame)

The current translator implements the "recognize-the-quad-signature" milestone end
to end:

1. **Resolve** the inner stream (`innerStreamGPA/Len`) to a host-readable buffer
   (zero-copy host pointer if available, else a bounded `read_mem` bounce).
2. **Walk** the records defensively (bounded, self-delimiting, segment-resync), and
   **recognize** the fullscreen textured-quad composite signature — `bind pipeline
   → bind a source texture at fragment slot 3 → DrawIndexedPrimitives16 count=6`,
   repeated.
3. **Replay** the recognized quad draw(s) through an embedded SPIR-V
   vertex+fragment pipeline (a fullscreen textured quad) sampling either the stub
   source or, when `APPLE_GFX_VK_REAL_TEX` is set, your supplied real BGRA8 source.
4. **Render** into the cached BGRA8 image via dynamic rendering, copy to the
   host-visible readback buffer, and hand the pixels to the device's present sink.
5. **Submit** with the timeline signal and enqueue a completion record so the
   completion thread drives the stamp.

The translator path is **opt-in** via `APPLE_GFX_VK_EXEC` (so the proven
immediate-stamp behavior remains the default until the create-side resource decode
lands), and falls back to the immediate stamp whenever a submit is not a
recognizable composite or the inner stream can't be resolved.

### Per-record decode confidence

Byte-confirmed against captured inner streams: the two-layer model, the descriptor
layout, the inner-stream framing (8-byte header, length-includes-header,
self-delimiting), the 9-opcode vocabulary + operand layouts, the exact command
order/counts of the representative frame, the bound resource refs and their slots,
and the render-target objectIDs. Inferred (consistent with the bytes + the
public Metal-on-the-wire structure, but not byte-proven): the *role* of each
pipeline (bg vs atlas vs glyph vs overlay), source-texture formats/dims (taken as
BGRA8 / R8), PSO/shader contents (refs only), the exact split of the
`DrawIndexedPrimitives16` trailing 4 bytes (offset vs base-vertex), the
`SetScissorRect` field widths (read as 4×u64; could be 2×u64 + 4×u32 — values
decode sanely either way), and the render-pass attachment descriptor layout
(synthesized for one color target).

---

## 4. Build & run (reproducible)

You need: the `anyos-qemu` tree (this project), the complete graphics source set
shipped in [`../gpu-translator/`](../gpu-translator/), a host with **Mesa lavapipe**
(software Vulkan) and the Vulkan loader/headers, and **your own** macOS restore
image.

The graphics path is five self-contained, original GPLv2 files, all under
`gpu-translator/`:

| File | Role |
|---|---|
| `apple-gfx-native.c` / `apple-gfx-native.h` | The framework-free Apple PVG (`apple-gfx`) device backend (the from-scratch protocol reimplementation). |
| `apple-gfx-vk.c` | The op-0x37 → Vulkan/lavapipe translator. |
| `apple-gfx-vk.h` | The `PGVkOps` seam between the two (no Vulkan dependency). |
| `pvg_vk_shaders.h` | The embedded SPIR-V present shaders (with GLSL source + regenerate command). |

### 4.1 Add the graphics path to the build

1. Place all five files from `gpu-translator/` into `hw/display/` and add the two
   `.c` files to that directory's meson build. `apple-gfx-native.c` is plain
   host-portable C with no special dependency; `apple-gfx-vk.c` needs the Vulkan
   loader/headers.
2. Gate the translator on Vulkan: have meson set `CONFIG_PVG_VULKAN` when
   `dependency('vulkan', required: false)` is found, and link the Vulkan loader for
   the `aarch64-softmmu` target. When the dependency is absent `apple-gfx-vk.c`
   compiles to a single `pg_vk_ops()` returning `NULL` and the native backend links
   unchanged (keeping its immediate-stamp fallback).
3. Wire `apple-gfx-native.c` into your machine's `apple-gfx` MMIO device: the glue
   provides the `PGNativeHostOps` callbacks (`host_ptr` / `read_mem` for guest RAM,
   `raise_irq`, and `present_bgra8` for the framebuffer) and forwards the gfx-BAR
   and iosfc-BAR MMIO to `pg_native_mmio_*` / `pg_native_iosfc_mmio_*`.
4. Configure for `aarch64-softmmu` and build as usual for this tree.

On a Linux host none of the Apple W^X / codesigning machinery applies, so this is a
normal QEMU build.

### 4.2 Run on a non-Apple ARM/Linux host with lavapipe

Install Mesa lavapipe and point the Vulkan loader at it, then launch the `vmapple`
machine with your own restore image and a display backend. Useful env vars exposed
by the translator:

| env var | effect |
|---|---|
| `VK_ICD_FILENAMES` | point the loader at the lavapipe ICD JSON (force software Vulkan) |
| `APPLE_GFX_VK_EXEC` | enable the op-0x37 → Vulkan translator path (else immediate-stamp) |
| `APPLE_GFX_VK_QUAD` | use the textured-quad present pipeline (vs a plain clear present) |
| `APPLE_GFX_VK_REAL_TEX` | `path` or `path:WxH` of a raw BGRA8 source you supply (else procedural stub) |
| `APPLE_GFX_VK_DEBUG` | enable Vulkan debug-utils (slow on a CPU ICD; off by default) |

The translator selects the lavapipe/CPU `VkPhysicalDevice` automatically
(`deviceType == CPU` or device name contains `llvmpipe`).

### 4.3 Validate offline without a full boot

A TCG boot of a modern macOS guest is slow (hours). To iterate on the translator
without one, build a tiny standalone driver that:

- links the compiled translator object and calls `pg_vk_ops()` to get the vtable,
- provides the handful of host callbacks the translator needs (`host_ptr` /
  `read_mem` to serve an inner stream at a fixed GPA, a `present_bgra8` sink that
  writes the delivered frame to a PPM, and a log sink), and
- provides thin pthread shims for the few QEMU thread primitives the object
  references (`qemu_mutex_*`, `qemu_cond_*`, `qemu_thread_*`).

Feed it a captured inner Metal stream (one you captured from *your own* guest), set
`APPLE_GFX_VK_EXEC=1`, and it will run the production `exec_indirect` end to end —
stream walk → quad recognize → lavapipe render → readback → present — and write the
frame as a PPM. With `APPLE_GFX_VK_REAL_TEX` pointed at a linear BGRA8 surface you
carved from your own guest's RAM, the output is a recognizable composited frame.

---

## 5. Status

- Framework-free PVG device + op-0x37 → Vulkan/lavapipe translator: **rendering a
  real composite frame on a non-Apple ARM host with no GPU (software Vulkan)** —
  proven end to end offline on a Linux/aarch64 box.
- The draw/present path is complete for the recognized composite signature; wiring
  the full create-side resource decode (so every objectID resolves to its real
  texture/pipeline contents automatically, rather than the linear-surface source +
  stub fallback) is the remaining work toward a fully live desktop.
