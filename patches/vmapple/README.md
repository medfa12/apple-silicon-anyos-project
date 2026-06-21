# vmapple — booting a modern set-up macOS under KVM on a non-Apple ARM host

`0001-kvm-guest-debug-exception-boot-fixes.patch`

This is original QEMU work that lets a **modern, already-set-up macOS** (the
`vmapple` machine class, macOS 26) boot under **KVM** on a **non-Apple ARM
host** — all the way through userland to the GPU-execution stage — by working
around a host capability gap that otherwise wedges the guest kernel.

> **No Apple software is distributed here.** This patch ships no Apple binaries
> and embeds no Apple code. Its anchors are pinned **at runtime** from the
> guest's own kernel image in RAM; to reproduce you must supply your own macOS
> restore image that you are licensed to use.

## The problem — a host debug-feature gap, not a missing device

On the non-Apple KVM host used here, `ID_AA64DFR0_EL1.DoubleLock` reads `0xf`
("implemented") and KVM will not spoof it to `0`. macOS's XNU therefore can
never *engage* the OS double-lock it expects, so monitor-debug exceptions stay
permanently armed and cannot be masked. The result is a cascade of
debug-exception failures, each of which halts or panics the guest:

1. A **kernel-mode assertion** — `ml_set_interrupts_enabled_with_debug`
   ("debug exceptions enabled in kernel mode") — panics whenever interrupts are
   re-enabled with `PSTATE.D` clear. (It is inlined at hundreds of sites and is
   reached very early.)
2. Once past that, the **same root cause re-surfaces in userland**: spurious EL0
   debug exceptions are delivered to processes as `SIGTRAP`. A critical
   launchd-managed daemon dies on `SIGTRAP`, and XNU panics on its loss.

This is *not* a missing device-model or a PCIe/topology gap — it is a single
host debug-feature limitation manifesting at three levels.

## The fix — KVM-gated, slide-invariant runtime kernel patches

The `vmapple` machine already carries a small periodic guest-RAM patcher. This
patch extends it with debug-exception disarms, **all gated on `kvm_enabled()`**
(TCG/HVF can engage the real double-lock and must keep the live behaviour), and
**all anchored on slide-invariant instruction windows pinned from the running
kernel** (no fixed offsets, no shipped Apple bytes):

- **`[mdscr-kde]`** — NOP the write that sets `MDSCR_EL1.KDE` (kernel debug
  enable).
- **`[mlsie-debug]` / `[mlsie-daif]`** — disarm the
  `ml_set_interrupts_enabled_with_debug` assertion. The robust form anchors the
  assertion's **`DAIF.D`-clear gating branch** (`mrs x8, daif` + `tbz/tbnz
  w8,#9`, forward-validated against the assertion's line-number/string preamble)
  and redirects it to fall through to the real interrupt-enable work.
- **`[mdscr-el0]`** — NOP the `orr` inside XNU's single `MDSCR_EL1`
  read-modify-write chokepoint (`update_mdscr`) so `MDSCR_EL1.SS`/`.MDE` can
  **never** be armed for EL0. With the EL0 single-step / monitor-debug source
  gone, the spurious userland `SIGTRAP`s stop and the critical daemons survive.
  (Live-verified: `MDSCR_EL1 == 0x0` on the running guest.)

**Operational note.** The periodic full-RAM rescan must be allowed to *settle*
(a bounded number of ticks) once the anchors have fired — left running every
tick under the big lock it starves a single-vCPU guest enough that APFS
transactions stall and a fixed-priority daemon trips XNU's scheduler fail-safe.
A settle bound resolves this.

## Result

With the patch the guest boots cleanly past every debug/`SIGTRAP` gate into deep
userland — Bluetooth, networking, audio, a full APFS root, and the paravirtual
GPU stack (`AppleParavirtGPU`) all come up and the guest's compositor begins
submitting real GPU command streams. The **remaining** gate is then on the
*graphics* side — the paravirtual-GPU command-execution / completion contract
(see `../../docs/metal-vulkan-translator.md` and the project `STATUS.md`), not a
boot or debug problem.

## Reproducing

Build `anyos-qemu` with this patch, supply **your own** set-up macOS restore
image, and launch the `vmapple` machine under KVM (`-accel kvm -cpu host`) on a
non-Apple ARM host. The patcher's settle bound is exposed as an environment
knob. GPLv2, as a derivative of QEMU.

---

# `0002-aes-completion-irq-boot-to-userland.patch`

A second, independent boot blocker, this time in the emulated **AES device**
(`hw/vmapple/aes.c`) rather than the CPU debug path. It is a pure QEMU
device-model bug fix and is independent of the accelerator (it bites under TCG
as well as KVM).

> **No Apple software is distributed here.** This patch modifies only the
> open-source QEMU device model and embeds no Apple code.

## The problem — a swallowed completion interrupt

The vmapple AES block has a small command **FIFO** processed *synchronously*:
each 32-bit word written to the FIFO register is appended and the command is
run immediately. A batched command sequence ends with a **`CMD_FLAG`
terminator**, and it is that terminator's handler which raises the device's
**command-complete interrupt** (status bit 5). The in-guest macOS AES driver
arms that interrupt and puts its command gate to **sleep** waiting for it; if
the completion interrupt never arrives, the gate sleeps forever and the guest
kernel **wedges before reaching userland**.

Two device-model bugs conspired to swallow that completion interrupt on the
driver's *builtin-key* path (a `CMD_KEY` selecting one of the device's builtin
keys by index, followed by a `CMD_DATA`, followed by the `CMD_FLAG`
terminator):

1. **Unpopulated builtin-key slots.** `builtin_keys[]` carried a valid key
   length for only a few of its indices. When the driver selected an
   unpopulated index, that slot's key length was `0`, so the data command found
   no matching AES algorithm and **failed**.

2. **"Drain only on success."** `fifo_process()` cleared the FIFO only when a
   command succeeded, so the *failed* `CMD_DATA` stayed buffered. The next word
   written — the `CMD_FLAG` terminator — was then appended to and **misparsed
   as more payload** of the stuck command, so the terminator handler never ran,
   the completion bit was never set, and the driver slept forever. (The
   barrier commands `CMD_DSB` / `CMD_SKG` hit the same trap: the old default
   path marked them *invalid* and never drained them either.)

## The fix — make every command sequence complete

All three changes are ordinary device-model corrections:

- **Populate every builtin-key slot** with a valid AES-256 key length, so
  selecting any index yields a usable cipher and the sequence runs to its
  terminator.
- **Drain the FIFO on payload-complete, not only on success.** A new
  `cmd_payload_elems()` helper reports how many payload words each recognised
  command expects; once those words are present the FIFO is drained even if the
  op itself failed, so a failed command can never swallow the trailing
  `CMD_FLAG` terminator. Only a *genuinely incomplete* command (still buffering
  payload) is left in the FIFO.
- **Treat `CMD_DSB` / `CMD_SKG` as drained no-op barriers** instead of invalid
  commands — they queue and wait on nothing and belong to the same terminated
  batch.

## Result

With the AES command sequence completing, the completion interrupt fires, the
driver's command gate wakes, and the guest **boots through to a full userland**
(WindowServer plus the usual daemon set). This is the prerequisite that lets
the GPU/compositor work in `0001` and the translator docs become reachable at
all.

## Reproducing

Build `anyos-qemu` with this patch applied to `hw/vmapple/aes.c` and boot your
own set-up macOS image on the `vmapple` machine (TCG or KVM). GPLv2, as a
derivative of QEMU.
