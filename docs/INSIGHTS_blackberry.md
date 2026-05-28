# BlackBerry "Strong ARMing with MacOS" — extracted insights

Source: BlackBerry Research & Intelligence Team, 2021-05-18
(archived at web.archive.org)

This article documents booting **macOS 11 Big Sur ARM64e (J273/A12Z DTK,
build 20C69)** to a bash prompt under QEMU 5.1.0 + TCG on Linux. They
based on `alephsecurity/xnu-qemu-arm64` (iOS path) and extended for
macOS. Their repo: https://github.com/cylance/macos-arm64-emulation

Most relevant to our project (vmapple + HVF, XNU 11156.x):

## 1. Apple IMP-DEF MSR/system register catalog

They needed support for **24 more Apple registers** beyond what
Aleph's iOS work added (12 registers). Specifically:

```
EHID1, EHID10, EHID4
MIGSTS_EL1, KERNELKEYLO_EL1, KERNELKEYHI_EL1, VMSA_LOCK_EL1
APRR_EL0, APRR_EL1, APRR_MASK_EN_EL1, APRR_MASK_EL0
CTRR_LOCK, CTRR_A_LWR_EL1, CTRR_A_UPR_EL1, CTRR_CTL_EL1
ACC_CTRR_A_LWR_EL2, ACC_CTRR_A_UPR_EL2, ACC_CTRR_CTL_EL2, ACC_CTRR_LOCK_EL2
CYC_CFG, CYC_OVRD, IPI_SR, UPMCR0, UPMPCM
```

These are read/written by XNU during early boot. On TCG they had to add
`ARMCPRegInfo` entries. **On HVF we can't** — the M-series CPU handles
them natively. If the kernel writes a non-existent register, HVF
returns an EC=0x18 trap which we handle in `target/arm/hvf/hvf.c`.

Our PAC work covers KERNELKEY*. APRR/CTRR are
write-locks that the kernel reads to determine SoC features —
we encounter these via the EC=0x18 sysreg-trap path.

## 2. APCTL_EL1.MKEYVld spin loop

Their first wall was a tight loop at:
```
mrs x0, s3_4_c15_c0_4 ; APCTL_EL1
and x1, x0, #0x2 ; MKEYVld bit
cbz x1, 0x479f4388 ; spin until set
```

They patched the kernel to **manually set MKEYVld=1** via `MSR APCTL_EL1, X1`
in their initial-branch trampoline. Equivalent to our earlier work
PAC key initialization via `hv_vcpu_set_sys_reg`.

## 3. Device tree modifications (critical, many parallel to our work)

They had to modify the captured DT extensively:

```
cpus/cpu0/state = "running" # avoid pe_identify_machine infinite loop
arm-io/ranges[0] = 0x100000000 # MMIO base
remove dockchannel-uart # use default uart0
chosen/dram-base = 0 # avoid arm_init panic
chosen/dram-size = 0
chosen/lock-regs/amcc/* (MANY properties) # avoid 0xfffffe0007b2af00 panic
```

Plus the `amcc-ctrr-a` sub-tree needs ~17 properties
(aperture-count, page-size-shift, lower/upper-limit, lock, etc.)
all initialized to 0 (length 4 each).

**Direct applicability**: earlier device-tree work fixed TrustCache and
BootKC-ro carveouts but we haven't touched `cpus/cpu0/state` or
`chosen/lock-regs/amcc`. If we hit a similar wall later, these are
known fixes.

## 4. NVRAM proxy data

`chosen/nvram-proxy-data` must be non-zero and contain at least one
32-byte partition named `"common"` (or `"system"`). Format:
- offset 0: skip 16 bytes
- offset 16: `kIODTNVRAMOFPartitionName` ("common" + nulls, 12 bytes)
- offset 28: u16 padding
- offset 30: u16 size_in_16_byte_units (their value: 2 -> 32 bytes total)

`chosen/nvram-total-size` must also be set to a valid non-zero
value (max 65536).

**Worth checking**: does our captured DT have these populated? If
we ever reach `IODTNVRAM::init` we'll need them.

## 5. Timer: GTIMER_VIRT vs GTIMER_PHYS — POTENTIALLY KEY

> **The solution was incredibly simple: MacOS uses a virtual timer.
> iOS uses a physical timer. Therefore, QEMU must specify a virtual
> timer with GTIMER_VIRT instead of GTIMER_PHYS when linking the timer
> to FIQ.**

Their critical fix in `j273_macos11.c`:
```c
qdev_connect_gpio_out(cpudev, GTIMER_VIRT,
 qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
```

**This directly relates to our IRQ-panic-at-kick-1500 wall from
earlier work in AVPBooter mode!** XNU was enabling
CNTV_CTL_EL0 (virtual timer), but we routed CNTP_PHYS to FIQ.
Result: timer fires but kernel doesn't see the interrupt class
it expected, hits IRQ panic.

Earlier experiments explored CNTP/CNTV writes and found that NOPing them
didn't help — because we never wired GTIMER_VIRT to anything.

If we ever go back to AVPBooter mode or this issue resurfaces in
Path 1 (which uses CNTV per earlier work's CNTFRQ work), check
`hw/vmapple/vmapple.c` for how the GIC's timer line is wired.
For HVF, the wiring is mostly hardcoded inside HVF itself — we
might need to find Apple's private channel for virtual-timer FIQ
delivery.

## 6. JOP/PAC disable for shell binaries

Their final wall before bash: `load_machfile` enables `IMGPF_NOJOP`
for binaries with identifiers like `com.apple.bash`, `com.apple.zsh`,
etc. This sets `TH_DISABLE_USER_JOP` on the thread. Then `Ldisable_jop`
validates SCTLR_EL1 == `0x7454599d`, freezes if mismatch.

**Their fix**: NOP the `ORR W8, W8, #0x80000000` instruction in
`load_machfile` to keep IMGPF_NOJOP off.

**Relevance**: if we ever reach userland init, this is a known
late-stage patch point. The disabled-strings list also includes:
- com.apple.security.cs.disable-library-validation
- com.apple.private.cs.automator-plugins
- com.apple.private.security.clear-library-validation
- com.apple.perl5, com.apple.perl, org.python.python
- com.apple.expect, com.tcltk.wish, com.tcltk.tclsh
- com.apple.ruby, com.apple.bash, com.apple.zsh, com.apple.ksh

## 7. NOP IDA-based offsets they found (for J273/20C69)

```c
#define INITIAL_BRANCH_VADDR_20C69 0xfffffe0007ac4580
#define BZERO_COND_BRANCH_VADDR_20C69 0xfffffe0007ab8a3c
#define SLIDE_SET_INST_VADDR_20C69 0xfffffe000806b438
#define CORE_TRUST_CHECK_20C69 0xfffffe0008cb6538
#define DISABLE_IMGPF_NOJOP_20C69 0xfffffe000806b234
```

Our kernel is XNU 11156.x (2026), theirs is 7195.60.x (2020), so
these specific VAs don't transfer. But the **categories** of patches
do: an initial branch trampoline, a bzero conditional, a slide-set
instruction, a core trust check, an IMGPF_NOJOP disable.

## 8. EL2-register-as-EL1 hack

They modified `target/arm/helper.c`:
```c
case 4: case 5:
 /* min_EL EL2 */
 mask = PL1_RW; // changed from PL2_RW
```

Because they couldn't switch QEMU to has_el2=true without losing the
ability to set up MMU. **We may face the same constraint in HVF mode**
— need to verify whether Apple HVF lets us run guest code at EL2 or
forces EL1.

## 9. Things they had to handle that we haven't yet

If we reach launchd/userland (still aspirational):

- **IORTC service** — kernel blocks waiting for it in IOKitInitializeTime.
 Their fix was actually solving the timer (#5), not the RTC itself.
- **`No task-access server configured`** — non-fatal warning, ignore.
- **iOS vs macOS binary compatibility** — RootlessJB's iOS-bash won't
 run on macOS kernel. We need actual macOS arm64e binaries from
 BaseSystem.dmg or equivalent.
- **APFS ramdisk** — extract via apfs-fuse to get bash/ls/sbin etc.,
 copy onto an HFS+ ramdisk resized via hdiutil (only step that
 requires actual macOS host).

## 10. Achievements

For perspective on what's possible: they got to a working bash prompt
with keyboard input on a stripped-down macOS 11 Big Sur ARM64e
kernel in TCG. Boot time 50s. ~5086 lines of kernel output, 113 kexts.
`shutdown -h now` panics with "Halt/Restart Timed Out" — even they
didn't get clean shutdown.

The interactive shell on serial works after the GTIMER_VIRT fix.

## Most actionable items for next steps

1. **Cross-check our DT against their amcc-ctrr-a / dram-base/size /
 cpu state additions**. If pmap_enter_pv or downstream walls trace
 to AMCC lock-region access, that DT subtree is our fix.

2. **Verify our nvram-proxy-data has at least one valid partition**
 formatted per their spec. Our captured DT may not have this.

3. **If we hit timer-related stalls**: investigate Apple HVF's
 virtual-timer FIQ delivery. We hit this earlier in AVPBooter
 mode; the HVF path may avoid it (CNTFRQ is set) but the
 IRQ-class wiring might still bite if launchd or kernel threads
 try to use timer-based sleeps.

4. **Track potential late patch points** (load_machfile NOP for
 bash JOP) for if/when we reach userland.

## Confirmation of validity

Their PAC disable approach is mirror-equivalent to ours. Their
device tree modifications align with our carveout
fixes. The GTIMER_VIRT discovery is a textbook example of the
exact kind of subtle XNU/QEMU mismatch we've been navigating.

Their work is a strong validation that macOS-on-QEMU is achievable
end-to-end; ours is harder because we're on HVF (no instruction-level
intercept) and a 6-years-newer XNU (much more elaborate boot
sequence with kauth, ledger, pmap_enter_pv, etc.).
