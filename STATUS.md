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

## Known limitations
- The reached prompt was captured non-interactively; driving the shell needs a
  real TTY on stdin.
- On a macOS host, runtime TB chaining is broken under Apple W^X (pre-existing),
  so runs require `-d nochain`. A Linux host avoids this entirely (the W^X layer
  is Apple-guarded and compiles out).
- This is the J273 / XNU-7195 baseline kernel. Porting forward to a current
  XNU is a separate, larger effort.

## Next
The clean foundation is the **Linux ARM port** (no W^X penalty, working
chaining, fast boot), followed by the tiered graphical path. Both — including an
honest analysis of what is and isn't reachable (a graphical kernel *console* is
feasible; a full Aqua/WindowServer desktop is not, on the cross-platform path) —
are laid out in [`docs/ROADMAP_crossplatform_gui.md`](docs/ROADMAP_crossplatform_gui.md).
