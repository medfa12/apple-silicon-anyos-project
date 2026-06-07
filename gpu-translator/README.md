# gpu-translator — Apple paravirtual-graphics → Vulkan (software) translator

`apple-gfx-vk.c` is the host-side translator that backs the modern `vmapple`
machine's accelerated paravirtual-graphics (PVG) command stream on a **GPU-less,
non-Apple ARM/Linux host**. It executes the guest's Metal-derived GPU work on the
CPU through the Mesa **lavapipe** (llvmpipe) software Vulkan ICD — no Apple
`ParavirtualizedGraphics.framework`, no GPU, no Metal symbol anywhere in the link.

It implements the `PGVkOps` vtable consumed by the framework-free native PVG
backend: `init`, `exec_indirect` (the op-0x37 Metal-command-stream translator),
`present` / `present_guest`, and `shutdown`. When Vulkan is not compiled in
(`CONFIG_PVG_VULKAN` off — e.g. a macOS or minimal build) the whole file collapses
to a single `pg_vk_ops()` returning `NULL`, so the native backend links unchanged
and keeps its immediate-stamp stub.

The method, the op-0x37 two-layer model, the 9-opcode render subset, the
opcode→`vkCmd` mapping, and the build/run steps are documented in
**[`../docs/metal-vulkan-translator.md`](../docs/metal-vulkan-translator.md)**.

## What's here

This directory ships the complete, self-contained source set for the modern
graphics path — the translator, the framework-free PVG backend it plugs into, the
interface header, and the embedded present shaders. All of it is original GPLv2
work; no Apple-copyrighted material is included.

| File | What it is |
|---|---|
| `apple-gfx-vk.c` | The translator. Stream walker + quad-composite recognizer + lavapipe bring-up + textured-quad present + timeline-semaphore completion path. |
| `apple-gfx-native.c` | The framework-free Apple PVG (`apple-gfx`) device backend — a from-scratch reimplementation of the host half of the paravirtual-graphics protocol with no `PG*` / Metal / IOSurface symbols. Decodes the gfx-BAR MMIO + FIFO, the command grammar, the iosfc surface registration, and the present/display-transaction path. |
| `apple-gfx-native.h` | The backend's public interface: register map, command/opcode grammar, the `PGNativeHostOps` host-abstraction vtable, and the device struct. |
| `apple-gfx-vk.h` | The `PGVkOps` seam the native backend reaches the translator through, with no Vulkan header dependency. |
| `pvg_vk_shaders.h` | The two embedded SPIR-V present shaders (fullscreen textured quad), with the GLSL source and the exact `glslangValidator` invocation to regenerate them. |

The translator (`apple-gfx-vk.c`) is gated on `CONFIG_PVG_VULKAN`; when Vulkan is
not compiled in, the whole file collapses to a single `pg_vk_ops()` returning
`NULL` and the native backend links unchanged with its immediate-stamp fallback.
An offline replay driver that links the translator object and feeds it a captured
inner stream — so you can validate it without a multi-hour TCG boot — is described
in the doc (`../docs/metal-vulkan-translator.md` §4.3).

## License & attribution

`SPDX-License-Identifier: GPL-2.0-or-later`.

This is original work and a **derivative of QEMU** (<https://www.qemu.org>),
distributed under the same GPLv2 terms. It uses the QEMU portability and threading
layer (`qemu/osdep.h`, `qemu/thread.h`, `qemu/main-loop.h`) and is intended to be
compiled into the `aarch64-softmmu` target as a new `hw/display` translation unit.
No Apple-copyrighted material is included; all Apple-format references are original
reverse-engineering analysis (symbol/struct/offset notes), not Apple code or data.
To reproduce a rendered frame you must supply your own macOS restore image that you
are licensed to use.
