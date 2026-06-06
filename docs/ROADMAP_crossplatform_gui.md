# macOS-on-QEMU: Phased Roadmap to Cross-Platform Graphical macOS

> Cross-platform roadmap covering the Linux/Windows ARM port, graphical
> display, and input + rootfs.

## 1. Executive Summary

The cross-platform TCG path (the Linux/Windows ARM target) can realistically reach a
**fast, interactive bash-on-serial kernel demo** — reproducing the prior baseline's 50s /
~5086-line / 113-kext result — and, with a small launch-flag change plus two config
corrections, a **graphical kernel-console window over VNC/GTK/SDL** showing
boot/verbose-log/panic text drawn by XNU's software videoconsole into `boot_args.Video`.
What is genuinely hard but mechanical is enriching the rootfs and adding emulated USB-HID
input. What is **infeasible on this stack** is a real Aqua/WindowServer desktop:
WindowServer hard-depends on IOMobileFramebuffer + the proprietary AGX/Metal GPU kext
stack that does not exist for emulated hardware. The one path to a real macOS desktop
(Apple PVG via QEMU's `vmapple`/`apple-gfx`) is macOS-host + HVF only and supports only
macOS 12 guests — it abandons the cross-platform goal and is absent from this 5.1.0 fork.

| Outcome | Cross-platform (Linux/Win ARM, TCG)? | Verdict |
|---|---|---|
| Serial bash (keyboard works) | Yes | **Proven** (prior baseline + our TCG baseline) |
| Fast serial bash on Linux ARM | Yes | **Achievable** (~35-60s, no W^X penalty) |
| Graphical kernel-console FB over VNC (T1) | Yes | **Feasible** — realistic graphical ceiling |
| GUI keyboard/mouse input | Maybe | **Unproven / brittle** (USB-HID + GIC + DT work) |
| Aqua/WindowServer desktop (T2) | No | **Infeasible** here (no GPU driver, no GUI userland) |
| Metal/GPU-accelerated desktop (T3) | No | **Infeasible** (proprietary AGX, not emulated) |
| Real macOS desktop via PVG/vmapple | No (macOS host only) | Possible but **wrong host**, macOS 12 only |

---

## 2. Phase Plan

### Phase A — Confirm fast serial bash on macOS host *(in progress)*
- **Goal:** Reduce the macOS W^X JIT penalty (a thread-local refcount recovers ~2.8x of it; the residual vs a non-Apple host is what the Linux port removes) so the existing Apple-Silicon TCG build
 boots to interactive serial bash at a usable speed; lock in a known-good baseline
 (serial_init, IOKit, ~5086 lines, 113 kexts).
- **Steps:** finish the per-TB `pthread_jit_write_protect_np`
 retrofit; re-run with the J273 run.sh args; confirm interactive bash and capture
 line/kext counts as the reference baseline.
- **Effort:** S (already underway elsewhere).
- **Risks:** macOS-specific W^X throughput; load_machfile `IMGPF_NOJOP` NOP must remain for
 bash/zsh not to freeze on JOP/PAC (INSIGHTS sec 6).
- **Go/No-Go:** Interactive bash reproduced on macOS host → proceed to B. (Gating dependency.)

### Phase B — Port to Linux ARM host → fast serial bash  ✅ DONE
- **Result (proven):** On a Debian 12 ARM64 (Ampere) cloud instance the same source tree builds
  cleanly (Python 3.11, `--disable-werror`, bundled submodules) with **no** W^X retrofit, **no**
  codesigning, and **no** `-d nochain` — runtime TB chaining works because the Apple-guarded W^X
  layer compiles out. Boot to `bash-3.2#` in **~50 s** (≈4× faster than the macOS host), and the
  interactive shell works. This confirms the cross-platform thesis end to end. The build/run
  scripts in the kit automate it.
- **Goal:** Build and run the *same* fork on a Linux aarch64 host, reaching interactive serial
 bash with **no** W^X overhead (the retrofit sites are all
 `#if defined(__APPLE__) && defined(__aarch64__)` and compile out).
- **Steps (concrete):**
 1. Copy the patched tree + 20C69 assets (kernelcache, DeviceTree, *enriched* ramdisk) to the
 Linux host.
 2. **Wipe the Darwin build state:** `make distclean` (or `git clean -xfd` on a clean checkout)
 — do **not** reuse the mac `config-host.mak` (CONFIG_DARWIN/COCOA).
 3. `git submodule update --init dtc capstone ui/keycodemapdb tests/fp/...` — dtc + capstone are
 load-bearing for the arm softmmu target.
 4. **Pin Python 3.10/3.11** via `--python=/usr/bin/python3.11` — 5.1.0's
 configure/tracetool/qapi use distutils/imp removed in 3.12+ (the single most likely hard
 blocker).
 5. Configure softmmu-only, no Werror:
 ```
 ./configure --target-list=aarch64-softmmu --disable-werror \
 --python=/usr/bin/python3.11 --enable-gtk --enable-sdl --enable-vnc --disable-docs \
 --extra-cflags="-Wno-error -Wno-deprecated-declarations -Wno-stringop-truncation \
 -Wno-stringop-overflow -Wno-format-truncation -Wno-maybe-uninitialized"
 make -j"$(nproc)"
 ```
 Debian/Ubuntu deps: `build-essential ninja-build pkg-config libglib2.0-dev libpixman-1-dev
 libfdt-dev zlib1g-dev libgtk-3-dev libsdl2-dev flex bison python3.11`.
 The glibc-2.36 `file_clone_range` break is **linux-user only** and cannot occur in a
 softmmu-only build.
 6. Run with the run.sh args against the Linux binary; keep `xnu-ramfb=off -serial mon:stdio
 -nographic` for this phase.
- **Effort:** M (mostly build-toolchain friction; behavior functionally identical to mac).
- **Go/No-Go:** Serial output matches the Phase-A baseline. Boot ~35-60s on a fast ARM core;
 ~90-150s on x86-64 fallback. Match → proceed to C.
- **Host options:** Oracle OCI Ampere A1 (always-free), AWS Graviton3/4, Apple Silicon under
 Asahi (fastest single-thread), Snapdragon X, or any x86-64 box as a slower fallback.

### Phase C — Graphical output (tiered; console FB over VNC first)
- **Goal:** Get *any* pixels — validate that XNU software-renders into the ramfb linear
 framebuffer (Tier 1). This is the **realistic graphical end-state** for this artifact.
- **Steps (single decisive experiment first):**
 1. **Fix the depth/geometry bug BEFORE judging anything:** `boot_args` says `V_DEPTH=16`
 while the QEMU surface uses 24bpp `PIXMAN_LE_r8g8b8` with `V_LINESIZE=width*3` — left as-is
 the window shows striped garbage easily mistaken for "macOS doesn't draw." Make both sides
 consistent — simplest is 32bpp `x8r8g8b8`, linesize `width*4` — in both
 `xnu_get_video_bootargs` and `xnu_ramfb.c` + `xnu_fb_cfg.h`.
 2. Set `xnu-ramfb=on` (instantiates the device AND populates `boot_args.Video`; with `off` the
 PA stays 0 and nothing draws).
 3. Drop `-nographic` and add a backend — **headless cloud: `-vnc :0`** (viewer → host:5900);
 local ARM laptop: `-display gtk`/`sdl`. Dropping `-nographic` removes the implicit
 `-serial mon:stdio`, so **re-add serial explicitly** (e.g.
 `-serial telnet:127.0.0.1:4444,server,nowait`) to keep the debug console.
 4. Use a verbose-friendly cmdline (drop `-noprogress`, optionally `-v`).
 5. **Go/no-go signal:** screen clears to background color, white iso_font kernel-log text, or a
 panic dialog — any one proves the linear-FB console path works end-to-end. Cross-check by
 dumping the `AllocatedData.ramfb` PA from the QEMU monitor for non-zero pixel data.
- **Effort:** S-M (no display-backend wiring needed — `graphic_console_init`/`dpy_gfx_*` already
 wired in `xnu_ramfb.c`; QEMU 5.1 UI selection is stock; work is flags + the depth fix).
- **Risks:** No IOMFB/GPU at T1, but boot may need to progress *past* PE console init — possibly
 further than today's TCG wall — before any pixel appears; our kernel is XNU 11156 (heavier,
 possibly lazier console init) vs the prior baseline's 7195.
- **Go/No-Go:** Pixels → T1 proven. No pixels even at correct depth → FB needs deeper boot
 progress; do **not** invest in T2.

### Phase D — GUI input + richer userland *(only if C shows promise)*
- **Goal:** Deliver keyboard/mouse events into the guest, and understand the userland ceiling.
 **This phase does not unlock a desktop** (no GUI userland/driver) — only justified for
 interactive graphical-console input or driver research.
- **Steps:**
 - *Input:* QEMU already ships `virtio-input`, `hcd-xhci`, `dev-hid` (usb-kbd/usb-tablet). But
 the j273 machine instantiates **only the Exynos UART** with a **dummy IRQ and no interrupt
 controller** (j273_macos11.c 565-567, 397-414). You must add an xHCI (or virtio-input-mmio)
 device **plus a real GIC/AIC + IRQ routing + matching DeviceTree nodes** so XNU probes them,
 then hope XNU's `IOUSBHIDDriver` attaches — **unproven**; Apple's USB personality matching
 may refuse a non-Apple xHCI.
 - *Richer userland:* The 1.5 GiB SU recovery ramdisk is a stripped bash/ls/sbin userland — no
 WindowServer/Aqua. Enrich it (prior-work recipe): extract arm64e binaries from
 `022-10310-098.dmg` via `apfs-fuse` (cross-platform), `hdiutil resize -size 1.5G`
 (**macOS-host-only**, one-time), copy bins, strip launch daemons for a bash profile, keep the
 `IMGPF_NOJOP` NOP. Buys more CLI tools, **not** a GUI.
- **Effort:** L (input pulls in a whole GIC/AIC + DT subtree and is brittle; rootfs enrichment is
 M but needs a one-time Mac for `hdiutil`).
- **Go/No-Go:** Emulated USB-HID enumerates *and* XNU delivers events → graphical-console
 interactivity. Doesn't attach after reasonable effort → stop; serial remains the only proven
 input modality.

### Phase E — Windows ARM *(parallel/optional)*
- **Goal:** Reproduce the Linux result on Windows-on-ARM, natively. **TCG needs no accelerator,
 so WHPX is irrelevant.**
- **Steps:** Build in MSYS2 **CLANGARM64** (Clang-based aarch64 env):
 `pacman -S mingw-w64-clang-aarch64-{toolchain,glib2,pixman,gtk3,SDL2,zstd,ninja,pkgconf} flex
 bison python`; `./configure --target-list=aarch64-softmmu --disable-werror --enable-sdl
 --extra-cflags="-Wno-error"`; `make -j`. Windows codegen uses the `#elif defined(_WIN32)`
 branch (VirtualAlloc PAGE_EXECUTE_READWRITE — RWX by default, no entitlements, no slowdown);
 `flush_icache_range` uses portable `__builtin___clear_cache`. Confirm serial bash before
 enabling `-display sdl/gtk`. Do **not** use the MSYS2 `mingw-w64-clang-aarch64-qemu` package
 (modern 9.x+, won't run this xnu machine type) — build *this* 5.1.0 fork. Interim fallback: an
 x86-64 MSYS2 build runs under Windows-on-ARM x86 emulation (slow but functional).
- **Effort:** M-L (least-trodden path; budget extra time for mingw header drift / small shims —
 5.1.0 predates Meson-era Windows fixes).
- **Go/No-Go:** `qemu-system-aarch64.exe` boots to serial bash → mirror Phase C flags. Same
 graphical ceiling as Linux.

---

## 3. Honest Infeasibility Section — where the wall is

Three **independent walls** sit between bash-on-serial and a real macOS desktop on the
cross-platform path; two are hard structural blockers:

- **Wall 1 — Rootfs (mechanical, high-effort):** A graphical desktop needs the full multi-GB
 macOS *system* volume (WindowServer, SkyLight, CoreGraphics, Aqua, Dock, loginwindow, all of
 `/System/Library`) — a different, much larger APFS install, not an enrichment of the 1.5 GiB
 recovery ramdisk. "Just engineering" but large.
- **Wall 2 — Display driver (the decisive structural wall):** `xnu-ramfb` is a one-way dumb
 `boot_args.Video` framebuffer that only feeds XNU's **early-boot software videoconsole** — a
 boot/log/panic surface, **never** a desktop. WindowServer/SkyLight do **not** draw to a
 `boot_args.Video` framebuffer; they require an `IOMobileFramebuffer`/IOGPU-family driver
 (AppleCLCD/disp0/AGX on real HW, or Apple PVG under the virtualizer) to publish an
 `IOFramebuffer` service. **No generic/software IOFramebuffer driver exists for arm64 macOS**,
 and the J273 DeviceTree exposes no QEMU-probeable display controller (only
 `displayport0`/`ext-displayport0` tunables). Writing such a kext is a from-scratch arm64e
 driver RE project — larger than the entire boot effort to date — compounded by
 TrustCache/library-validation kext-loading walls. eShard's iOS Apple-logo result only came
 from an undocumented, version-specific QuartzCore CPU-fallback hack that itself broke on
 AMX/vImage and GPU-compressed surfaces — research-grade, not a plan.
- **Wall 3 — Input (smaller, real):** No HID path exists; solvable-ish via emulated USB-HID +
 GIC + DT (Phase D) but only matters after Wall 2, and likely fights Apple USB personality
 matching.

**The one path to a real macOS GUI — and why it fails the goal:** Apple PVG
(`AppleParavirtualizedGraphics`) via QEMU's `vmapple` machine + `apple-gfx-mmio` gives a genuine
accelerated display **with integrated input**. But it (1) is **absent** from this 5.1.0 fork (no
`hw/vmapple`, no `apple-gfx`), (2) calls into the **macOS host's** PVG framework so it requires
Apple-Silicon + **HVF — not portable to Linux/Windows ARM**, and (3) supports only **macOS 12**
guests, not the 11.1 kernel here. PVG = right GUI, wrong host; TCG = right host, no GUI driver.

**Realistic ceiling on the cross-platform path:** a **graphical kernel-console window** (T1:
clears to background, fills with iso_font kernel-log text, shows panic UI) over VNC/GTK/SDL,
backed by a fast interactive bash-on-serial kernel demo — exactly the prior baseline's achievement plus
a console FB. A clickable Aqua desktop is **not** reachable with this artifact.

---

## 4. Immediate Next 3 Actions (once the W^X fix lands)

1. **Stand up the Linux ARM build (Phase B).** Provision a fast ARM host (Oracle OCI Ampere A1
 always-free, AWS Graviton3/4, or Apple Silicon under Asahi), copy the tree + assets,
 `make distclean`, `git submodule update --init dtc capstone ...`, then configure with
 **`--python=python3.11 --disable-werror`** + the `-Wno-*` CFLAGS. **Verify serial parity
 first** (~5086 lines, 113 kexts, interactive bash) before touching graphics.
2. **Fix the ramfb depth/geometry bug, then run the one decisive graphics experiment (Phase C).**
 Make `boot_args.Video` depth and the QEMU surface format **consistent (32bpp `x8r8g8b8`)** in
 `xnu_get_video_bootargs` + `xnu_ramfb.c`/`xnu_fb_cfg.h`. Then set `xnu-ramfb=on`, drop
 `-nographic`, add `-vnc :0`, re-add explicit `-serial`, drop `-noprogress`, boot, connect a
 VNC viewer to host:5900, and look for cleared screen / iso_font text / panic dialog.
 Cross-check the `AllocatedData.ramfb` PA in the QEMU monitor for non-zero pixels.
3. **Make the go/no-go call and set expectations.** Pixels → T1 proven; document it as the
 deliverable (graphical kernel console, cross-platform), optionally polish geometry, then weigh
 Phase D input work. No pixels even at correct depth → the FB needs deeper boot progress (XNU
 11156 console init past today's TCG wall); record that and **do not** pursue T2/T3. In
 parallel, kick off Phase E (Windows MSYS2 CLANGARM64) only if a Windows ARM target is needed.
