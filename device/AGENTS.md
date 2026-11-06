# AGENTS.md

Notes for AI coding agents (and people) working in this repository. Read it before changing
anything; it follows `hakios-a32`'s rules, with this phone's differences.

## What this is

HakiOS for the **Samsung Galaxy A21s (SM-A217F / SM-A217M)**: Samsung **Exynos 850**, 8 ×
Cortex-A55, **Mali-G52 MP1**, 4 GiB, eMMC. The target is the same as the A32's: Apple's open
XNU (xnu-8796) and the iPhone X's iOS 16 user space (now **16.7.16, 20H392**), started by this
phone's own loader. The shared kernel, user space and GPU stack live in the organisation's
common repositories (`hakios-xnu-kernel`, `hakios-metal`); this is the device repository.

The phone is a Samsung **Exynos** one: the boot chain is Samsung's S-Boot (not LK), the PMIC is
an S2MPU12 behind the ACPM (not a MediaTek pwrap), the interrupt controller is a GIC-400
(GICv2, not GICv3), storage is DW MMC (not MSDC). Do not carry MediaTek or Qualcomm assumptions
over. The GPU *is* the A32's family (Mali-G52, Bifrost v7): that is where code is shared.

## Layout

| path | what | license |
| --- | --- | --- |
| `diag/` | `a21sdiag`: the first chainboot/framebuffer/GPU-power diagnostic | MIT |
| `tools/` | `chainboot.py` (A21s: raw Image, 36 MiB boot), `probe-a21s*.sh`, `mkidentity.py`, `mkfont8x16.swift` | MIT |
| `docs/` | how each part was found, register by register | MIT |
| `device.conf` | the phone and the state of each component | |

To come, in this order: `loader/` (the Exynos successor of `mtkloader`: relocation, DT, boot
arguments, display, eMMC), `kernel/` (APSL 2.0: GICv2, the Mali's Exynos power-up, touch,
buttons), the run/log loop.

## Rules

1. **Nothing of Apple's in the tree**: no iOS binary, image, cache, shader library, font,
   picture or screenshot, and no header derived from iOS firmware (`xnuloader/identity.h` is
   generated from the user's own IPSW by `tools/mkidentity.py` and ignored by git).
2. **Nothing of the vendor's**: no firmware, no partition image, no boot image, no device tree
   taken off a phone. The probe scripts read the phone; their outputs stay off the repository
   (`probe-*.txt` is ignored) because they contain the phone's serial numbers.
3. **No personal data**: no serial numbers (`androidboot.serialno`, `ap_serial`, `em.did`, …),
   no home-directory paths, no tokens, no Wi-Fi names or passwords.
4. **Licenses**: `kernel/` will be APSL 2.0; everything else is MIT. Samsung's and Linux's GPL
   sources (the Exynos 850 kernel, `mali_kbase`) may be read for register addresses and order
   and named where used, never copied.
5. **Names**: new code uses `hakios_*`, `kern.hakios.*`, `/dev/hakios-*`.

## Working on the phone: safety

- Flash **only `boot`** (mmcblk0p18, 36 MiB), and the iOS image into the `xnusys` region with
  `tools/xnusys.py` (the free tail of `super`, never its allocated extents). The A21s is **legacy non-A/B**: there is no
  `boot_a`/`boot_b`. Never write `efs`, `sec_efs`, `param`, `up_param`, `keystorage`,
  `vbmeta*`, `super`'s allocated extents or the bootloader partitions. S-Boot does not inflate
  gzip kernels: `boot` holds a raw Image only (a gzip one boot-loops).
- Always pass the serial (`adb -s`, `$HAKIOS_SERIAL`): a second phone may be connected.
- Keep the user's stock `boot`, `recovery` and `dtb` images on the Mac; flashing `boot` back
  from TWRP is how the phone is restored.
- Runs are short and one at a time. Do not raise CPU or GPU voltages; `vdd_gpu` stays as the
  PMIC has it.
- Read first: every new hardware step is behind a boot-arg, reads before it writes, and says its
  result in the log.

## Reading a run's log

Android's ramoops has no console zone here. Bare-metal code writes into the **pmsg** zone at
`0xf0f14000` (16 KiB, header "DBGC"); after the next Android boot it is
`/sys/fs/pstore/pmsg-ramoops-0` (root; `strings` it, logd's records are mixed in).

## State, briefly

`device.conf` and `docs/hardware-a21s.md`. a21sdiag has run on the bare metal (2026-11-04): S-Boot
chains it at 0x80080000 at EL1, the GPU is powered at hand-over, the display is video mode.
