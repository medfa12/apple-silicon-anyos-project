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

## Known limitations
- On a macOS host, runtime TB chaining is broken under Apple W^X (pre-existing),
  so runs require `-d nochain`. A Linux host avoids this entirely (the W^X layer
  is Apple-guarded and compiles out).
- This is the J273 / XNU-7195 baseline kernel. Porting forward to a current
  XNU is a separate, larger effort.

## Next
The clean foundation is the **Linux ARM port** (no W^X penalty, working
chaining, fast boot). The graphical kernel console (Tier 1) is now reached; a
full Aqua/WindowServer desktop remains out of scope on the cross-platform path.
The honest display-tier analysis is in
[`docs/ROADMAP_crossplatform_gui.md`](docs/ROADMAP_crossplatform_gui.md).
