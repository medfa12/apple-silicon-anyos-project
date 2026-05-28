# Graphics patch — make `xnu-ramfb=on` render the kernel console to a framebuffer

`0001-ramfb-graphical-console.patch` activates the fork's `xnu_ramfb` device so
the XNU kernel software-renders its (verbose) boot console — kext bring-up,
IOKit, `launchd`, and the `bash-3.2#` prompt — into a linear framebuffer you can
view over VNC/GTK/SDL. `SPDX-License-Identifier: GPL-2.0-only`; derivative of
QEMU 5.1.0 + the Cylance/BlackBerry macOS-11 fork; no Apple material included.

The `xnu_ramfb` device existed in the fork but was **dead code** — the upstream
demo always ran `xnu-ramfb=off`, so this path had never been compiled or run.
Three fixes were needed:

| File | Fix |
|---|---|
| `hw/display/Makefile.objs` | Add `xnu_ramfb.o` to the build — it was never compiled, so the `xnu_ramfb` device type was unregistered and `qdev_new()` asserted at startup. |
| `hw/display/xnu_ramfb.c` | API drift vs QEMU 5.1: `DeviceUnrealize` dropped its `Error**` parameter, and `dc->props` was replaced by `device_class_set_props()`. |
| `include/hw/arm/xnu_fb_cfg.h` + `hw/display/xnu_ramfb.c` | Depth consistency: the kernel was told 16bpp while the host surface was 24bpp `r8g8b8` — garbled output. Reconciled to 32bpp (`V_DEPTH=32`, `linesize = width*4`, surface `x8r8g8b8`). |

## Run

Build as in [`../tcg/README.md`](../tcg/README.md), then launch with
`xnu-ramfb=on`, a display backend, and a verbose kernel cmdline so the video
console has text to paint:

```
qemu-system-aarch64 -M macos11-j273-a12z,...,\
  kern-cmd-args="kextlog=0xfff cpus=1 rd=md0 serial=2 -v",xnu-ramfb=on \
  -cpu max -m 4G -display vnc=:0          # then connect a VNC viewer to host:5900
```

(Headless capture without a viewer: `-display none` plus the monitor command
`screendump frame.ppm`, which forces a framebuffer refresh and writes a PPM.)

Note: with the framebuffer active the kernel routes its console to the **video
console** rather than the UART, so serial may go quiet — that is expected and
confirms the video path is live.

## Scope

This is a graphical kernel **console** (boot/verbose log/panic text). It is
**not** a desktop: WindowServer/Aqua require an `IOMobileFramebuffer` + GPU
driver stack that does not exist for emulated hardware. See
[`../../docs/ROADMAP_crossplatform_gui.md`](../../docs/ROADMAP_crossplatform_gui.md)
for the full display-tier analysis.
