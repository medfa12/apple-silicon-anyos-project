# Status

## Reached
- **XNU → userland under TCG.** The J273 / A12Z (Big Sur 11.1, XNU 7195.60.75)
  kernel boots under QEMU 5.1.0 + TCG to an interactive `bash-3.2#` prompt:
  full landmark chain through `pmap_startup` → `vm_page_bootstrap` → IOKit class
  registration → prelinked-kext load → BSD root on `md0` → `load_init_program
  /sbin/launchd` → `launchd` (PID 1) → `bash`. The known non-fatal
  `No task-access server configured` warning appears, same as prior public work.
- **Made reproducible on a modern Apple-Silicon host:** serial output restored
  (kprintf enable-flag repoint + UART tap) and JIT throughput improved ~2.8×
  via a thread-local W^X nesting refcount. See `patches/tcg/`.
- **Interactive shell over serial.** With a real TTY on stdin the shell runs
  typed commands and returns output (`ls /` lists the XNU root; `uname`/`id` are
  absent — the SU recovery ramdisk is a stripped userland).
- **Graphical kernel console.** With `xnu-ramfb=on` the kernel software-renders
  its verbose boot console — through to the `bash-3.2#` prompt — into a
  framebuffer viewable over VNC. See `patches/graphics/`.
- **Cross-platform port PROVEN on Linux/ARM.** The same source tree builds
  cleanly on a Debian 12 ARM64 (Ampere) cloud instance — no W^X retrofit, no
  codesigning, no `-d nochain` — and boots to `bash-3.2#` in **~50 s** with TB
  chaining enabled (≈4× faster than the macOS host, matching the original
  Linux/TCG figure). Interactive shell confirmed there too. See
  `docs/ROADMAP_crossplatform_gui.md` Phase B and the build/run scripts.
- **Modern set-up macOS boots under KVM to the GPU-execution stage (non-Apple
  ARM).** Distinct from the Big Sur / TCG line above: an already-set-up modern
  macOS (the `vmapple` machine class, macOS 26) boots under **KVM** on a
  non-Apple ARM host through full userland — Bluetooth, networking, audio, a
  complete APFS root, and the paravirtual-GPU stack — to where its compositor
  submits real GPU command streams. The blocker was a host debug-feature gap
  (`ID_AA64DFR0_EL1.DoubleLock` is unspoofable under KVM, so XNU can never mask
  the debug exceptions it expects to lock out → a kernel-assertion + EL0-`SIGTRAP`
  cascade), cleared by KVM-gated, runtime-anchored guest-kernel debug-exception
  disarms. No Apple binaries shipped; anchors are pinned from the live kernel.
  See `patches/vmapple/`.

## Known limitations
- On a macOS host, runtime TB chaining is broken under Apple W^X (pre-existing),
  so runs require `-d nochain`. A Linux host avoids this entirely (the W^X layer
  is Apple-guarded and compiles out).
- This is the J273 / XNU-7195 baseline kernel. Porting forward to a current
  XNU is a separate, larger effort.
- **The Metal→Vulkan translator's rendered frame is, today, an offline *replay*.**
  It faithfully decodes a captured `op-0x37` command stream and replays it through
  real software-Vulkan (lavapipe) into a BGRA8 target — but the replay's source
  texture is the *captured* composite, so it demonstrates the pipeline mechanics,
  not live execution of the guest's own per-layer GPU work. Executing the live
  stream — resolving the guest's real surface backings and honouring the GPU
  command-completion contract — is the open frontier (now reached on the modern
  KVM track above), **not** a finished result. No live, guest-rendered desktop
  frame has been screendump-verified on a non-Apple host yet.

## Next
Two fronts:
1. **Big Sur / TCG line** — the clean foundation is the **Linux ARM port** (no
   W^X penalty, working chaining, fast boot); the graphical kernel console
   (Tier 1) is reached. Display-tier analysis:
   [`docs/ROADMAP_crossplatform_gui.md`](docs/ROADMAP_crossplatform_gui.md).
2. **Modern `vmapple` / KVM line** — the guest now reaches the paravirtual GPU,
   but its command stream does not yet *complete*: the translator must execute
   the real render (not just replay), resolve the guest's live surface backings,
   and signal a real completion so XNU's GPU scheduler sees the frame finish.
   That — a live, guest-rendered frame — is the honest remaining gap to a real
   desktop on a non-Apple host.
