# Comparison: our vmapple work vs zhuowei/qemu a12z-macos

Clone: a local checkout of zhuowei/qemu at `https://github.com/zhuowei/qemu` branch `a12z-macos`
(latest commit `b99ee1b778`).

## TL;DR

Zhuowei's branch and ours both boot XNU on QEMU, but the targets diverge
in three structural ways:

| | zhuowei a12z-macos | this repo (vmapple) |
|------------------|-------------------------------------------------|--------------------------------------------------|
| SoC target | t8015 (iPhone X / A11) → later t8020 (A12Z DTK) | vmapple (VMA2MACOS, Apple Virtualization.framework) |
| Kernel format | Raw arm64 Mach-O (XNU 4570.x, ~2018-19) | Fileset kernelcache (XNU 11156.x, 2026) |
| Backend | QEMU TCG (software emulation) | QEMU + Apple HVF (hardware virtualization) |
| boot_args Rev/Ver| Rev=2, Ver=2 (deviceTreeLength is u32) | Rev=3, Ver=2 (deviceTreeLength is u64) |
| PAC handling | Disabled in `pauth_helper.c` (TCG NOPs) | Real PAC keys set via `hv_vcpu_set_sys_reg` |
| TLB invalidate | TCG controls TLB directly | Apple HVF caches stage-1 by (VA, TTBR1); unbreakable from host |

The TCG vs HVF gap is the biggest one. Zhuowei can intercept any guest
instruction (PAC AUT, MSR to private sysregs, TLBI). We can't — HVF runs
guest code natively on the M-series CPU and only returns on the limited
exit set HVF exposes. That's why our work has so much more focus on
guest-side instruction injection and patch sequencing.

## Where we converge on solutions

### 1. boot_args construction (cf. their commit `9db4e86019`)

Zhuowei's `macho_setup_bootargs` writes a `xnu_arm64_boot_args` struct at
a fixed PA, sets `boot_args.deviceTreeP` to a placeholder, calls
`write_bootloader` to emit a small trampoline that loads X0 = bootargs
and branches to the entry PC.

Our earlier work (`hw/vmapple/vmapple.c` lines 706–803) does the same
thing for the modern Rev=3 layout. A key insight from zhuowei, which took a while to rediscover: **bootargs must follow the kernel image in PA
order, not overlap it**. We initially placed the DT inside the kernel image PA range, hit the
resulting "Device tree pointer outside of device tree region" panic, and
eventually relocated the DT to PA `0x75800000`.

### 2. Mach-O segment loading (their `arm_load_macho`)

Zhuowei iterates `LC_SEGMENT_64` once to find low/high VA, then again to
copy each segment to `(vmaddr - low_addr) + kernel_load_offset`. Their
`VAtoPA` macro is `((VA & 0x3fffffff) + mem_base + kernel_load_offset)`
— masks the VA to 30 bits because their kernels use a fixed
`0xfffffff007004000`-style virtBase.

We added an equivalent direct-load path that
writes the kernel into `machine->ram` instead of using
`load_image_targphys` (which deferred to reset). We use the segment's
`vmaddr - virtBase + physBase` formula since our kernels carry full
64-bit VAs.

### 3. PAC disable (their commit `16613b67ad`)

Zhuowei stubs `pauth_key_enabled() -> false`, makes `xpaci`/`xpacd`
identity functions, and `pacga` returns 0. This works because TCG runs
the PAC helpers in software.

We can't do that on HVF — the M-series CPU runs `PACIA` natively. So
We added:
- HVF init writes 10 PAC key halves via `hv_vcpu_set_sys_reg` (so the
 keys exist consistently)
- 473k PAC AUT/PAC* instructions patched to non-auth equivalents in the
 kernel image
- ELR/LR strip in the wildpc handler

Same outcome (PAC out of the way), but the HVF path costs ~500x more
code.

### 4. Apple IMP-DEF sysreg stubbing (their commits `23a68d233e`,
`56b68b92ce`, `dc2c6d0f2c`)

Zhuowei adds entries in `target/arm/helper.c` for KTRR, MIGSTS, etc.:
```c
{ .name = "MIGSTS_EL1", .state = ARM_CP_STATE_AA64,
 .opc0 = 3, .opc1 = 4, .crn = 15, .crm = 0, .opc2 = 4,
 .resetvalue = 0x2, .access = PL1_RW, ... }
```
Plus `target/arm/translate-a64.c` `handle_sync` falls through to ignore
invalid MSR ops instead of trapping.

We can't add ARMCPRegInfo entries because HVF doesn't consult them. Our
equivalent is the HVC-trap autopatch: replace specific MSR/MRS sites
with `HVC #magic`, host handler emulates. Less general but functional.

### 5. TrustCache region (their commits `2d187a6e90`, `234be70bef`,
`37767ad9b9`)

Zhuowei loads a TrustCache file at the address from boot_args, sized to
1 MiB. Our earlier work fixed the DT-side TrustCache PA but our
TrustCache region just contains zeros — the kernel only validates
placement during skip-AVPBooter, not contents.

## Where we diverge — unique HVF challenges

These are problems zhuowei never hit because TCG controls the CPU:

- **HVF stage-1 cache opacity** (earlier work): host PT writes don't
 invalidate HVF's cached translations. Confirmed repeatedly
 with every host-side TLB-bust attempt. Only guest-side `TLBI VAE1`
 works, requires guest-side instruction injection.
- **TTBR1 silent write drop** (earlier work, earlier work): `hv_vcpu_set_sys_reg(TTBR1_EL1)`
 returns success but HVF keeps using old TTBR1. Required 12-instruction
 guest-side `MSR TTBR1_EL1` inject at vbar+0x200 (earlier work,
 earlier work).
- **HVF wedge** (earlier work): after first faultfix resume, `hv_vcpu_run`
 doesn't return for 5+ minutes. No equivalent in TCG.
- **PT corruption from kernel data writes** (earlier work): AVPBooter's
 cloned page tables share PA range with kernel's `__DATA_CONST`.
 Required TTBR1 swap to fresh PA at `0x150000000`.

## What we could borrow from zhuowei

1. **`arm_load_macho` two-pass segment loader** is cleaner than our
 one-shot `fread` into `machine->ram`. Could simplify our skip-AVPBooter
 load path.
2. **`b25a3160c9` "use local copy of Mach-O header"** — we depend on
 system Mach-O headers. Vendoring them would make builds more portable.
3. **`27d3152db1` "Increase ramdisk size limit"** — we don't load a
 ramdisk; if we ever boot to userspace, this is the pattern to copy.
4. **`b33ae7365e` "add empty memory regions where t8015 memory and
 interrupt controller's supposed to be"** — we already do this in
 vmapple.c (`create_gic`, `create_aic` etc.) but their pattern of
 stubbed-out regions for unknown peripherals is worth mirroring if we
 ever need new SoC peripherals.
5. **Their `write_bootloader` trampoline** is simpler than our
 `vmapple_skip_avpbooter` PC-set approach. Doesn't matter for HVF
 correctness but might be cleaner.

## What zhuowei can't help with

Most of our wall set:
- HVF cache opacity (earlier work)
- Guest-side instruction injection mechanics (earlier work+)
- Faultfix lazy mapping handler (earlier work)
- All the post-MMU-on walls (zone bootstrap, kauth, ledger, etc.) —
 zhuowei's branch never reached far enough for these to fire because
 XNU 4570 had different init sequences and their boot was incomplete.

## Conclusion

zhuowei's branch is a useful sanity check on the boot_args struct
layout (we already have ours correct per Rev=3) and Mach-O loading
strategy. It's not a roadmap forward — the bulk of our remaining work
(HVF cache opacity, faultfix tuning, vmapple-specific carveouts,
modern XNU init quirks) is genuinely novel and not present in their
work.

The most important takeaway: their PAC disable confirms that **PAC is
incidental to booting XNU**. Disabling it doesn't break boot. Validates
our PAC-disable approach.
