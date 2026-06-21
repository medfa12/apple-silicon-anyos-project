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
- **Modern set-up macOS boots to FULL USERLAND under KVM (non-Apple ARM).**
  Distinct from the Big Sur / TCG line above: an already-set-up modern macOS (the
  `vmapple` machine class, macOS 26 / Tahoe-class) boots under **KVM** on a
  non-Apple ARM host all the way to a complete userland — WindowServer plus on
  the order of **120 system daemons** (Bluetooth, networking, audio, a full APFS
  root). Two host gates cleared this:
  1. *Debug-feature gap.* `ID_AA64DFR0_EL1.DoubleLock` is unspoofable under KVM,
     so XNU can never mask the debug exceptions it expects to lock out → a
     kernel-assertion + EL0-`SIGTRAP` cascade. Cleared by KVM-gated,
     runtime-anchored guest-kernel debug-exception disarms (no Apple bytes
     shipped; anchors pinned from the live kernel). See `patches/vmapple/`.
  2. *AES-completion wedge.* The emulated AES engine never delivered the
     completion interrupt the keystore driver sleeps on during early boot, so the
     guest wedged before userland. Root-caused to a builtin-key length gap in the
     device model and fixed by validating every builtin key slot and draining the
     command FIFO on payload-complete. See `docs/breakthrough-boot-and-first-pixels.md`.
- **WindowServer composites continuously on the non-Apple host.** With a
  *set-up* (already-activated, autologin) macOS disk booted **without** the
  throwaway `-snapshot` overlay — so the first-boot finalize persists — the
  guest's compositor reaches **continuous compositing**: tens of thousands of
  `op-0x37` accelerated render submits over a run (a fresh, never-activated disk
  instead parks forever in Setup-Assistant activation and never submits a single
  one). See `docs/breakthrough-boot-and-first-pixels.md`.
- **The real macOS wallpaper renders on the non-Apple host (first real pixels).**
  The framework-free PVG device's surface-page resolver was extended to follow the
  geometry descriptor's word-0 page-list indirection; with that, the guest's
  scanout IOSurfaces resolve from `pages = 0` to their real page lists (e.g. 507
  pages for a 1920×1080 BGRA8 surface) and the VNC output shows the **real macOS
  desktop wallpaper** plus a band of composited UI — produced on a non-Apple ARM
  host with no GPU. See `docs/breakthrough-boot-and-first-pixels.md`.

## Known limitations
- On a macOS host, runtime TB chaining is broken under Apple W^X (pre-existing),
  so runs require `-d nochain`. A Linux host avoids this entirely (the W^X layer
  is Apple-guarded and compiles out).
- This is the J273 / XNU-7195 baseline kernel for the TCG line. Porting that line
  forward to a current XNU is a separate, larger effort.
- **The composited UI is not yet fully on screen.** The host translator now
  renders the guest's `op-0x37` composite into its own Vulkan image, and the
  scanout IOSurface's real page list resolves, so the wallpaper and a band of UI
  appear — but the translator does **not yet write its rendered composite back
  into the resolved scanout IOSurface**, so the full per-layer UI is not yet
  routed to the scanout. This is the single remaining gap on the modern line: a
  VK-composite → scanout write-back. The desktop is **partial**, not finished.
- One class of UI source surfaces (host-shared IOSurfaces, as opposed to the
  GPU-virtual-address class) is a known remaining sub-case for the per-layer
  resolver.

## Next
Two fronts:
1. **Big Sur / TCG line** — the clean foundation is the **Linux ARM port** (no
   W^X penalty, working chaining, fast boot); the graphical kernel console
   (Tier 1) is reached. Display-tier analysis:
   [`docs/ROADMAP_crossplatform_gui.md`](docs/ROADMAP_crossplatform_gui.md).
2. **Modern `vmapple` / KVM line** — the guest now boots to full userland,
   WindowServer composites continuously, and the **real macOS wallpaper renders
   on the non-Apple host** (`docs/breakthrough-boot-and-first-pixels.md`). The
   single remaining step to a full guest-rendered desktop is the
   **VK-composite → scanout write-back**: the translator already renders the
   `op-0x37` composite into a Vulkan image and the scanout IOSurface's page list
   already resolves, so the work is copying the rendered composite into the
   resolved scanout surface (and resolving the remaining host-shared-IOSurface
   source class). Until that lands, the on-screen result is honestly **partial** —
   wallpaper + a UI band, not the full composited desktop.
