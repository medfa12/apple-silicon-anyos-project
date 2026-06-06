# apple-silicon-anyos-project

Original QEMU work toward running Apple's **XNU / macOS cross-platform** — on a
Linux or Windows ARM host, not only Apple hardware — through the
software-emulation (TCG) path.

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

4. **Framework-free Apple paravirtual-graphics + Metal→Vulkan path** *(modern
   `vmapple` track)*. A from-scratch reimplementation of the Apple PVG device
   that runs **without** Apple's `ParavirtualizedGraphics.framework`, plus a
   Metal-command-stream → Vulkan (lavapipe) translation path — so the guest GPU
   stack can run on a GPU-less, non-Apple host. ~6k lines across 9 new files
   (the native PVG backend, the MMIO glue, the op-stream tap, the SPIR-V present
   pipeline, and an offline replay harness).

| Capability | Host | Status |
|---|---|---|
| Boot to userland `bash` (serial) | Linux / Windows / macOS ARM (x86 fallback) | **Reached** |
| Interactive shell over serial | Any | **Reached** |
| Graphical kernel console (framebuffer / VNC) | Any | **Reached** |
| Cross-platform build + boot | Linux / ARM | **Proven** (~50 s to bash) |
| Framework-free PVG / Metal→Vulkan | non-Apple | In progress (modern `vmapple` track) |
| Full Aqua / WindowServer desktop | — | Out of scope here |

## Layout

```
STATUS.md                            What is reached / proven, with the landmark boot chain.
docs/ROADMAP_crossplatform_gui.md    Phased Linux / Windows-ARM port + display tiers.
patches/tcg/                         The serial + W^X fixes (reference diff + rebuild recipe).
patches/graphics/                    The xnu-ramfb kernel-console activation + VNC viewing.
```

## Reproducing

See **`patches/tcg/README.md`**. In short: build QEMU 5.1.0 + the upstream
macOS-on-QEMU fork, apply the W^X / serial patch, supply your own J273
kernelcache + device tree + ramdisk, and launch the `macos11-j273-a12z` machine
with a serial console. On a Linux/ARM host the W^X machinery compiles out, so the
~2.8× penalty disappears and boot-to-shell lands in well under a minute.

## Built on / license

GPLv2 — see [`LICENSE`](LICENSE). This is a **derivative of QEMU**
(<https://www.qemu.org>) and is distributed under the same terms; no
Apple-copyrighted material is included. It builds, as a reference, on the prior
open-source macOS-on-QEMU lineage (Aleph Security `xnu-qemu-arm64`;
`zhuowei/qemu`; and the Cylance/BlackBerry J273 machine model + original
boot-to-bash demonstration on Linux/TCG).
