# apple-silicon-anyos-project

Original QEMU work toward running Apple's **XNU / macOS cross-platform** — on a
Linux or Windows ARM host, not only Apple hardware — through the
software-emulation (TCG) path, and, for the modern `vmapple` machine, under KVM.

> **No Apple software is distributed here.** This repository contains only
> original analysis and original modifications to open-source QEMU. To reproduce
> any result you must supply your own Apple kernelcache, device tree, and
> ramdisk (from a restore image you are licensed to use). macOS, XNU, and Apple
> Silicon are trademarks of Apple Inc.; this project is for interoperability
> research and education.

## Original contributions

The macOS-11 (J273 / A12Z, Big Sur 11.1, XNU 7195.60.75) XNU boot is a known
public result; what this project contributes is making it **run, build, and
render cross-platform**, plus a from-scratch graphics path for the modern
machine:

1. **Cross-platform boot-to-shell.** The same source tree builds cleanly on a
   Linux/ARM64 host and reaches an interactive `bash-3.2#` prompt in ~50 s — the
   Apple-only W^X machinery compiles out entirely on non-Apple hosts.

2. **Apple-Silicon host fixes** (`patches/tcg/`):
   - *Serial visibility* — the early `kprintf` enable flags were being written
     to the wrong control words; repointing them (verified by disassembling
     `_kprintf`) plus a UART TX tap is what first produced real serial output.
   - *W^X JIT batching, ~2.8× faster TB translation* — Apple Silicon enforces
     strict W^X on JIT pages, and the naïve retrofit toggled the per-thread
     write-protect bit (a kernel trap) **twice per translation block** because
     the `TranslationBlock` metadata lives in the JIT region. A thread-local
     nesting refcount collapses that to one open/close per codegen round, and
     fixes a latent SIGBUS on block-chaining. Strict superset of the prior
     trace — every previously reached PC still reached, zero regressions.

3. **Graphical kernel console** (`patches/graphics/`). Activated and fixed the
   `xnu-ramfb` framebuffer device (dead code upstream — never compiled): QEMU-5.1
   API drift plus a 16→32 bpp pixel-depth fix, so the kernel software-renders its
   verbose boot console — through to the `bash-3.2#` prompt — into a framebuffer
   viewable over VNC. A graphical kernel *console*, not an Aqua desktop.

4. **Framework-free Apple paravirtual-graphics + a Metal→Vulkan translator that
   replays a captured composite frame on a GPU-less non-Apple host** *(modern
   `vmapple` track)*. A from-scratch reimplementation of the Apple PVG device that
   runs **without** Apple's `ParavirtualizedGraphics.framework`, plus a translator
   that turns the guest's Metal-derived GPU command stream into **software Vulkan
   (Mesa lavapipe / llvmpipe)**. The guest's accelerated render submit (the
   `CmdExecIndirect2` "op-0x37" stream) decodes to a fixed, small vocabulary of
   **9 render opcodes** that map 1:1 onto core Vulkan; the translator walks that
   inner Metal stream, recognizes the textured-quad composite signature, and
   replays it through an embedded SPIR-V pipeline into a BGRA8 target — driven by
   a timeline-semaphore completion path — on a non-Apple aarch64 Linux host with
   Mesa lavapipe (software Vulkan, no GPU). **Honesty:** this proves the full
   op-0x37 → Vulkan *pipeline* end to end, but the replay's source texture is the
   *captured* composite, so it is **not yet** live execution of the guest's own
   per-layer GPU work — no live, guest-rendered desktop frame has been
   screendump-verified on a non-Apple host. Rendering the guest's live stream
   (resolving its real surface backings + a real GPU-completion contract) is the
   open frontier — see contribution 5 and `STATUS.md`.
   Reproducible: build `anyos-qemu` + the translator, supply **your own** macOS
   restore image, and run on a non-Apple ARM/Linux host with lavapipe. The method,
   the op-0x37 two-layer model, the 9-opcode subset, the opcode→`vkCmd` mapping,
   and the build/run steps are in
   [`docs/metal-vulkan-translator.md`](docs/metal-vulkan-translator.md); the
   original GPLv2 translator is [`gpu-translator/apple-gfx-vk.c`](gpu-translator/apple-gfx-vk.c).

5. **A modern set-up macOS boots under KVM to the GPU-execution stage on a
   non-Apple ARM host** *(modern `vmapple` track, `patches/vmapple/`)*. Distinct
   from the Big Sur / TCG boot above, an already-set-up macOS 26 reaches full
   userland — Bluetooth, networking, audio, a complete APFS root, and the
   paravirtual-GPU stack submitting real GPU command streams — under **KVM** on a
   non-Apple ARM host. The blocker was a host debug-feature gap
   (`ID_AA64DFR0_EL1.DoubleLock` is unspoofable under KVM, so XNU can never mask
   the debug exceptions it expects to lock out → a kernel-assertion + EL0-`SIGTRAP`
   cascade that panics the guest), cleared by KVM-gated, runtime-anchored
   guest-kernel debug-exception disarms (no Apple bytes shipped; anchors pinned
   from the live kernel). This puts the modern guest **at** the graphics frontier
   of contribution 4 — the GPU command stream now arrives; making it *complete*
   (live render + a real completion contract) is the remaining gap.

| Capability | Host | Status |
|---|---|---|
| Boot to userland `bash` (serial) | Linux / Windows / macOS ARM (x86 fallback) | **Reached** |
| Interactive shell over serial | Any | **Reached** |
| Graphical kernel console (framebuffer / VNC) | Any | **Reached** |
| Cross-platform build + boot | Linux / ARM | **Proven** (~50 s to bash) |
| Framework-free PVG + op-0x37 → Vulkan *pipeline* (lavapipe, no GPU) | non-Apple ARM / Linux | **Replay proven** (offline; captured-pixel source — see Honesty above) |
| Modern set-up macOS → GPU-execution stage under KVM | non-Apple ARM | **Reached** (boot); live render = open |
| Live, guest-rendered composite / desktop frame on a non-Apple host | non-Apple ARM / Linux | **Open frontier** |
| Full Aqua / WindowServer desktop | — | Out of scope here |

## Layout

```
STATUS.md                            What is reached / proven, with the landmark boot chain.
docs/ROADMAP_crossplatform_gui.md    Phased Linux / Windows-ARM port + display tiers.
docs/metal-vulkan-translator.md      The op-0x37 → Vulkan/lavapipe translator: model, 9-opcode subset, build/run.
patches/tcg/                         The serial + W^X fixes (reference diff + rebuild recipe).
patches/graphics/                    The xnu-ramfb kernel-console activation + VNC viewing.
patches/vmapple/                     KVM guest-kernel debug-exception boot fixes (modern macOS → GPU stage).
gpu-translator/                      The full modern-graphics source set (GPLv2): the framework-free
                                     PVG device backend (apple-gfx-native.c/.h), the op-0x37 → software-
                                     Vulkan translator (apple-gfx-vk.c), the interface seam (apple-gfx-vk.h),
                                     and the embedded SPIR-V present shaders (pvg_vk_shaders.h).
```

## Reproducing

See **`patches/tcg/README.md`**. In short: build QEMU 5.1.0 + the upstream
XNU-on-QEMU fork, apply the W^X / serial patch, supply your own J273
kernelcache + device tree + ramdisk, and launch the `macos11-j273-a12z` machine
with a serial console. On a Linux/ARM host the W^X machinery compiles out, so the
~2.8× penalty disappears and boot-to-shell lands in well under a minute.

## Built on / license

GPLv2 — see [`LICENSE`](LICENSE). This is a **derivative of QEMU**
(<https://www.qemu.org>) and is distributed under the same terms; no
Apple-copyrighted material is included. It builds, as a reference, on the prior
open-source XNU-on-QEMU lineage (Aleph Security `xnu-qemu-arm64`;
`zhuowei/qemu`; and the Cylance/BlackBerry J273 machine model + original
boot-to-bash demonstration on Linux/TCG).
