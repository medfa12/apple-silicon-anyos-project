# TCG patch — W^X JIT batching + serial visibility

`0001-wx-batching-and-serial-visibility.patch` is a **reference diff** of the two
original fixes that made the J273/XNU-7195 kernel boot to an interactive
`bash-3.2#` prompt on a modern Apple-Silicon **host** under QEMU 5.1.0 + TCG.
It is meant to be read and adapted to your own fork checkout — the comment text
has been generalised, so apply with `--reject`/by-hand rather than expecting a
byte-exact `git apply` against an arbitrary tree.

## Files touched

| File | Change |
|---|---|
| `tcg/aarch64/tcg-target.h` | Thread-local W^X nesting refcount API (`jit_write_protect_disable/enable`); `flush_icache_range` via `sys_icache_invalidate`. Non-Apple builds get empty inlines. |
| `tcg/aarch64/tcg-target.inc.c` | Single definition of the refcount; **balances** `tb_target_set_jmp_target` (the runtime chaining path previously left the thread writable → latent SIGBUS). |
| `accel/tcg/translate-all.c` | `tb_gen_code` opens the writable window **exactly once** across the `buffer_overflow` retry and closes it on every return path (incl. before the `cpu_loop_exit` longjmp); icache invalidation decoupled from the W↔X flip. |
| `tcg/tcg.c` | `tcg_prologue_init` routed through the balanced API. |
| `hw/arm/j273_macos11.c` | Repoint the two `kprintf` enable flags to the **correct** control words (verified by disassembling `_kprintf`). |
| `hw/char/exynos4210_uart.c` | Mirror every guest UART TX byte to stderr (a belt-and-suspenders serial tap). |

## Build (Apple-Silicon macOS host)

1. Obtain QEMU 5.1.0 and the upstream `xnu-qemu-arm64` macOS fork (Cylance/BlackBerry
   `macos-arm64-emulation`); apply their diff to a clean QEMU 5.1.0 tree.
2. Apply this patch's changes on top.
3. Configure for the `aarch64-softmmu` target only and build (a 2020 codebase: disable
   `-Werror`). On macOS you must code-sign the binary with JIT entitlements
   (`com.apple.security.cs.allow-jit`, `allow-unsigned-executable-memory`,
   `disable-library-validation`), and strip any resource fork (`xattr -c`) before
   signing.

## Run

Launch the `macos11-j273-a12z` machine with **your own**
`kernelcache` + `DeviceTree` + `ramdisk`, a serial console (`-serial mon:stdio`),
and `-m 4G`.

> **macOS-host caveat:** runtime TB chaining is broken under Apple's W^X
> enforcement (a pre-existing issue, independent of this patch), so on a macOS
> host you must pass **`-d nochain`**. The interactive prompt then appears a few
> minutes in. On a **Linux** host this whole W^X layer compiles out (it is guarded
> for Apple hosts only), chaining works normally, and boot-to-shell is fast.

## Why it matters

The W^X refcount removes the dominant per-translation-block kernel-trap cost
(~2.8× throughput) and the serial repoint makes the boot observable. Together
they turn a silent, very slow boot into a visible, reproducible XNU-to-userland
handoff (`launchd` PID 1 → `bash`).

## Provenance & license

`SPDX-License-Identifier: GPL-2.0-only`. This diff is a derivative work of
**QEMU 5.1.0** and of the **Cylance/BlackBerry `macos-arm64-emulation`** macOS-11
QEMU fork (itself built on Aleph Security's `xnu-qemu-arm64`), all GPLv2. It
modifies only those projects' files (`accel/tcg/translate-all.c`, `tcg/*`,
`hw/arm/j273_macos11.c`, `hw/char/exynos4210_uart.c`) and contains no
Apple-copyrighted material. Obtain the upstream base from those projects; this
patch is provided as a reference diff to apply on top.
