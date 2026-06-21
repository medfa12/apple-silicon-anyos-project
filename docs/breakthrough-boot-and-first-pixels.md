# Boot-to-userland + first real pixels on a non-Apple host

This documents three connected breakthroughs on the modern `vmapple` / KVM line.
Together they take a modern set-up macOS guest from *wedged before userland* to
*booted to a full userland with a continuously compositing WindowServer, and the
real macOS desktop wallpaper rendered on a non-Apple ARM host with no GPU.*

> **Reproducible, with no material from us.** Everything below is original
> analysis and original modifications to open-source QEMU and the from-scratch
> graphics path. To reproduce any of it you must supply **your own** macOS restore
> image / set-up disk (one you are licensed to use); we ship no Apple binaries,
> kernelcache, memory dumps, device-tree, keys, or images. macOS, Metal, and
> Apple Silicon are trademarks of Apple Inc.; this is interoperability research.

The honest end state: the **wallpaper and a band of composited UI** render on the
non-Apple host. The full per-layer UI is **not yet** routed to the scanout — see
[§3.4](#34-the-honest-remaining-gap). This is a *partial* desktop, deliberately
described as such.

---

## 1. Boot-to-userland: the emulated-AES completion wedge

### Symptom

A modern set-up macOS guest under KVM on a non-Apple ARM host reached early kernel
init and then **wedged before userland** — no `launchd`, no WindowServer, no
daemons. The stall was a kernel thread blocked in a `commandSleep`, waiting on a
completion interrupt from the emulated AES engine that never arrived.

### Mechanism

Early boot drives the keystore/AES path that derives a set of special keys
(the "SecureRoot" / FDR-class derivation). Walking the device protocol:

1. The driver issues a `CMD_KEY` that selects a **builtin** key **by index**, then
   a `CMD_DATA` that AES-wraps a small block with the selected key.
2. The AES device model carried a table of builtin keys, but only a few slots had a
   valid `key_len`; other indices had `key_len == 0`.
3. When `createSpecialKeys` selected one of the zero-length slots, the device's
   `cmd_data` handler rejected the operation at its key-length switch (the algorithm
   resolved to *invalid*) and returned failure **without draining the command
   FIFO**.
4. Because the FIFO was not drained, the trailing **`CMD_FLAG` completion
   terminator** (the `0x88000000 | tag` word that marks the end of the command
   batch) was never decoded as a `CMD_FLAG` — it accreted behind the stuck
   `CMD_DATA` instead.
5. `cmd_flag()` therefore never ran → the device's completion status bit was never
   raised → the accelerator's interrupt handler never tail-called its
   `_completeAES` → the driver's `commandSleep(cmd)` blocked **forever**.

So a single missing key length, several commands earlier, silently swallowed the
completion edge the whole boot was sleeping on.

### Fix

In the AES device model (`patches/qemu-vmapple/aes.c` in the development tree):

- **Give every builtin key slot a valid AES-256 `key_len`**, so any index the guest
  selects produces a usable algorithm and `cmd_data` succeeds rather than bailing.
- **Drain the FIFO on payload-complete**, so the `CMD_FLAG` terminator is always
  reached and decoded — which is what actually raises the completion status and
  fires the interrupt the driver sleeps on.

### Result

The completion interrupt fires, `commandSleep` wakes, and boot advances past the
keystore derivation. The guest now reaches a **full userland** — WindowServer plus
on the order of **120 system daemons** (Bluetooth, networking, audio, a complete
APFS root) — under KVM on a non-Apple ARM host.

> Note: this is an *emulator/firmware* completion-handshake fix. The keys
> themselves are arbitrary placeholder bytes in the device model; the bug and the
> fix are entirely about the device's command-FIFO drain and completion-interrupt
> protocol, not cryptographic content.

---

## 2. Producer-unblock: a set-up disk, persisted

### Symptom

Reaching userland is necessary but not sufficient: with a **fresh, never-activated**
macOS disk the compositor still issued **zero** accelerated render submits
(`op-0x37 = 0`) — it parked indefinitely.

### Mechanism

A fresh disk lands in **Setup Assistant**, which performs a network activation step.
With no real activation backend, the guest parks in an activation Mach-stall: the
first-paint *producer* (the compositor's first-frame path) is gated behind
activation completing, so it never runs and never submits GPU work. This is a
**userland activation/lifecycle stall, not a GPU gate** — an important
reclassification, because earlier work had been hunting for a missing graphics edge
when the real block was upstream in Setup Assistant.

A second, subtler trap: running with QEMU's throwaway `-snapshot` overlay means the
guest's **first-boot finalize never persists**, so even an already-set-up disk can
be knocked back into a not-yet-finalized state on every run.

### Fix

- Boot a **set-up** macOS disk (already activated, autologin) so there is no
  activation Mach-stall to park behind.
- Boot it **without `-snapshot`** (persistent) so the first-boot finalize completes
  and stays completed.

### Result

WindowServer reaches **continuous compositing**: on the order of **50,000+**
`op-0x37` accelerated render submits over a run, where a fresh disk produced **zero**.
The producer is unblocked and steadily submitting real composite work — the
precondition for any real pixels to exist.

---

## 3. First real pixels: resolving the scanout surface page list

With a continuously compositing producer, the host PVG device finally has real
surfaces to map. One indirection in the surface descriptor stood between "mapped but
empty" and the real wallpaper.

### 3.1 What a surface descriptor is

The framework-free Apple PVG (`apple-gfx`) device backend
(`gpu-translator/apple-gfx-native.c`) models the iosfc (IOSurface) path. Each
compositing surface has a small **geometry descriptor** (≈0x200 bytes: width,
height, stride, pixel format, a base offset, etc.). To present a surface the host
must turn it into actual guest-RAM pixel pages — it needs the surface's **page-list
array**: one entry per 16 KiB pixel page of the backing.

### 3.2 The bug: `pages = 0`

The resolver assumed the page-list array was either inline in the 0x200 descriptor
or findable by scanning the descriptor for a plausible pointer/PFN. For these
surfaces neither held: the page list is **not** inline, and the descriptor's first
word did not look like a raw pointer. So the resolver returned `pages = 0` — the
surface was *mapped but never filled*, and the screen showed nothing real.

### 3.3 The mechanism: word-0 is itself a PTE

The descriptor's **word 0 is itself a page-table entry**, in the on-wire mapper
encoding `PFN << 2 | present`. It does **not** point at the pixels directly — it
points at a **separate page that holds the surface's page-list array**. That array
is `page_count` entries of the same `PFN << 2 | present` form (one per pixel page),
then zero-padded. The page list is therefore **one indirection away** via word 0,
and the old resolver never tried decoding word 0 in PTE form (it has the present bit
set, so a raw-pointer test rejects it).

The fix in `pg_native_resolve_pages` adds this primary path: read word 0, and if its
present bit is set, treat it as a PTE — `((word0 >> 2) << page_shift)` is the GPA of
the page-list array. Validate that the target is a coherent array of present PTEs of
the expected length, then walk it to recover every pixel page's GPA.

Worked example from a live set-up Tahoe run: a scanout surface's `word0 = 0x001d8909`
→ `(0x1d8909 >> 2) << 14 = 0x1d8908000`, a clean **507-entry** PFN table — exactly
the page count for a 1920×1080 BGRA8 surface (`1920 * 1080 * 4 ≈ 0x7e9000` bytes ÷
16 KiB ≈ 507 pages). The earlier inline/scan heuristics are kept as fallbacks; the
word-0 indirection is the real wire encoding.

### 3.4 Result and the honest remaining gap

With the word-0 indirection, the scanout IOSurfaces resolve from `pages = 0` to
their real page lists (e.g. 507 pages), and the VNC output shows the **real macOS
desktop wallpaper** plus a band of composited UI — rendered on a non-Apple ARM host
with no GPU.

**What is not yet on screen:** the host translator renders the guest's `op-0x37`
composite into its **own** Vulkan image, but does **not yet write that rendered
composite back into the resolved scanout IOSurface**. So the layer the translator
produces is not yet routed to the scanout, and the full per-layer UI is not fully
visible. The single remaining step on this line is a **VK-composite → scanout
write-back** (copy the translator's rendered image into the resolved scanout
surface's pages). One source-surface class — host-shared IOSurfaces, as opposed to
the GPU-virtual-address class — is also a known remaining sub-case for the per-layer
resolver.

This is a **partial** desktop: real wallpaper pixels, real continuous compositing,
real surface resolution — but the composited UI is not yet fully routed to the
scanout. We describe it that way on purpose.

---

## 4. Why this matters

Before this work the modern line stopped *before userland* and any talk of pixels
was hypothetical. Now, on a non-Apple ARM host under KVM, with **your own** set-up
macOS disk:

- the guest **boots to a full userland** (WindowServer + ~120 daemons),
- WindowServer **composites continuously** (tens of thousands of real `op-0x37`
  render submits), and
- the **real macOS wallpaper renders on screen** with the surface page lists
  correctly resolved.

The remaining gap is narrow and well-defined — route the translator's composite
back into the resolved scanout surface — and it is the last step between "real
wallpaper + UI band" and a full guest-rendered desktop on non-Apple hardware.
