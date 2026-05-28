# apple-silicon-anyos-project

Research notes, analysis, and QEMU patches for booting Apple's **XNU / macOS
kernel to an interactive userland shell under QEMU**, with an emphasis on the
**cross-platform software-emulation (TCG) path** — so the work can eventually
run on a Linux or Windows ARM host, not only on Apple hardware.

> **No Apple software is distributed here.** This repository contains only
> original analysis and original modifications to open-source QEMU. To
> reproduce any result you must supply your own Apple kernelcache, device tree,
> and ramdisk (e.g. from a restore image you are licensed to use) and the
> upstream QEMU fork (see *Credits*). macOS, XNU, and Apple Silicon are
> trademarks of Apple Inc.; this project is for interoperability research and
> education.

## What's here

The boot-to-bash chain on the **J273 / A12Z (Big Sur 11.1, XNU 7195.60.75)**
kernel was first demonstrated publicly by Cylance/BlackBerry (on Linux/TCG); the
credit for that result is theirs. What this repository adds are two original
fixes that make the same boot run on a modern **Apple-Silicon host** — where
QEMU 5.1.0 did not originally work — taking it under **QEMU 5.1.0 + TCG**
through `launchd` (PID 1) to a `bash-3.2#` userland prompt (reached and captured
non-interactively).

The two original contributions:

1. **Serial visibility fix** — the early `kprintf` enable flags were being
   written to the wrong control words; repointing them (verified by
   disassembling `_kprintf`) plus a small UART TX tap is what first produced
   real serial output. Boot had been progressing deep into IOKit for a while;
   the problem was that none of it was *visible*.
2. **W^X JIT batching — ~2.8× faster TB translation** on macOS hosts. Apple
   Silicon enforces strict W^X on JIT pages, and the naïve retrofit toggled the
   per-thread write-protect bit (a kernel trap) **twice per translation block**,
   because the `TranslationBlock` metadata itself lives in the JIT region. A
   thread-local nesting refcount collapses those toggles to one open/close per
   codegen round, and incidentally fixes a latent SIGBUS on the runtime
   block-chaining path. Strict superset of the prior trace: every previously
   reached PC still reached, zero regressions.

| Path | Host | Status |
|---|---|---|
| TCG (software emulation) | Any (Linux / Windows / macOS ARM, x86 fallback) | **Reaches userland `bash` on the J273 baseline** |
| Cross-platform port | Linux / Windows ARM | Planned — see `docs/ROADMAP_crossplatform_gui.md` |

## Layout

```
docs/
  INSIGHTS_blackberry.md       Extracted, cross-checked insights from prior public
                               macOS-on-QEMU work (IMP-DEF MSRs, DT mods, the
                               virtual-timer/FIQ fix, the JOP/PAC userland patch).
  COMPARISON_zhuowei.md        Structural comparison of the HVF vs TCG approaches.
  ROADMAP_crossplatform_gui.md Phased plan: Linux/Win ARM port + display tiers,
                               with an honest infeasibility analysis for a full
                               graphical desktop.
patches/tcg/
  0001-wx-batching-and-serial-visibility.patch   The two fixes above (reference diff).
  README.md                                       How to rebuild and reproduce.
```

## Reproducing

See **`patches/tcg/README.md`** for the full recipe. In short: build QEMU 5.1.0
+ the upstream `xnu-qemu-arm64` macOS fork, apply the W^X / serial patch, supply
your own J273 kernelcache + device tree + ramdisk, and launch the
`macos11-j273-a12z` machine with a serial console.

The cross-platform takeaway: on a **Linux ARM** host the W^X machinery compiles
out entirely (it is guarded for Apple hosts only), so the ~2.8× penalty
disappears and boot-to-shell is expected in well under a minute.

## Credits

This work stands on prior open-source projects, to whom full credit is due:

- **QEMU** — the emulator (GPLv2). https://www.qemu.org
- **Aleph Security `xnu-qemu-arm64`** — the original XNU-on-QEMU foundation.
- **`zhuowei/qemu` (`a12z-macos`)** — A12Z / Mach-O loading and PAC/sysreg
  groundwork. https://github.com/zhuowei/qemu
- **Cylance / BlackBerry `macos-arm64-emulation`** — the J273 macOS-11 machine
  model and the original boot-to-bash demonstration on Linux/TCG.
  https://github.com/cylance/macos-arm64-emulation

## License

GPLv2 — see [`LICENSE`](LICENSE). The QEMU patch here is a derivative work of
QEMU and is therefore distributed under the same terms. No Apple-copyrighted
material is included.
