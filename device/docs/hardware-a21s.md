> Historical development notes. For the current snapshot, see the top-level README and `docs/` directory. Earlier installation recipes and status claims below may be superseded.

# Galaxy A21s: the hardware, as Android sees it

## On the bare metal: a21sdiag's first run (2026-11-04)

S-Boot ran the chained program and Android came back on its own (~90 s; this ROM recompiles
ART at boot, so the "logo" phase, which is S-Boot's framebuffer with a21sdiag's text on it, lasts
that long). The report, from `pmsg-ramoops-0`:

| read | value | means |
| --- | --- | --- |
| `start` | `0x80080000` | S-Boot loads the kernel at DRAM base + text_offset; the loader relocates from here |
| `dtb` | `0x8a080000` | the device tree S-Boot hands over, 160 MiB above |
| `EL` | **1** | EL2 is kept (Samsung's hypervisor); XNU runs at EL1 anyway |
| `CNTFRQ` | 26000000 | the 26 MHz timer of the device tree |
| `MIDR` | `0x410fd051` | Cortex-A55 r0p1 |
| `MPIDR` | `0x81000000` | MT bit set: the core number is in **Aff1**, not Aff0 (XNU's CPU numbering must use Aff1) |
| `ID_AA64MMFR0` | `0x00101122` | **TGran16 supported** (XNU's 16 KiB pages), 40-bit PA, 16-bit ASIDs |
| `ID_AA64PFR0` | `0x10112222` | GIC field 0: **no GIC system registers**, a memory-mapped GICv2 CPU interface |
| `ID_AA64ISAR1` | `0x00100001` | no pointer authentication (as the iPhone X's arm64 user space expects), LRCPC, DPB |
| `SCTLR_EL1` | `0x00c5183a` | MMU off, D-cache off, I-cache on at hand-over |
| `WTCON` cl0/cl1 | `0x00018021` | **both watchdogs running, with reset**: the loader must kick or stop them |
| `GICD_CTLR` / `TYPER` | `0x1` / `0xfcef` | distributor on; 512 interrupt lines, 8 CPUs, security extensions |
| `GICD_IIDR` / `GICC_IIDR` | `0x0200143b` / `0x0202143b` | ARM **GIC-400** |
| `PD_G3D` conf/status | `0x1` / `0x1` | **the GPU's power domain is on** when S-Boot hands over |
| `GPU_ID` | `0x72120000` | Mali-G52 (product 0x7212) **r0p0**, readable: powered *and* clocked |
| `L2_FEATURES` | `0x07100206` | 64 KiB L2, 64-byte lines |
| shader / tiler / L2 present | `0x1` / `0x1` / `0x1` | one shader core, one tiler, one L2 slice |
| DECON `psr_mode` | 0 | **video mode**: the framebuffer is scanned out continuously; no per-frame trigger (unlike the A32) |
| DSIM | `0x2d0` × `0x640` | 720 × 1600 in the DSIM's own registers |
| framebuffer words | `0xff......` | 32-bit pixels with alpha 0xff |

So the GPU starts out powered and clocked: the Mali bring-up can begin at the A32's step 4
(soft reset, L2/tiler/shader power-up, the two test jobs) without touching the PMU, the CMU or
the PMIC. Those are only needed later, to switch the GPU off and on again or to change its clock.

## As Android sees it

Read on a running SM-A217M (Latin-American A21s; the SM-A217F is the same board) with
`tools/probe-a21s.sh` and `tools/probe-a21s-dt.sh`, as root, **read only**: nothing was written
to any partition or register. Addresses are from the device tree in `/sys/firmware/devicetree`
and from `/proc/iomem`; each is to be confirmed on the bare metal by `a21sdiag` before a driver
relies on it.

The phone the probe ran on had a custom ROM (an Android 15 port) and a custom 4.19 kernel. The
addresses below are the SoC's and the board's; the ROM changes none of them, but the boot
image layout is that ROM's (see *Boot*).

## SoC

| | |
| --- | --- |
| SoC | Samsung **Exynos 850** (S5E3830), `ro.hardware=exynos850` |
| CPU | 8 × **Cortex-A55** (MIDR part 0xd05), PSCI 1.0 (`enable-method = psci`), up to 2210 MHz |
| RAM | 4 GiB LPDDR4 in 7 ranges: `0x80000000 +0x3c900000`, `0x880000000`, `0x8a0000000`, `0x8c0000000`, `0x8e0000000` (+512 MiB each), `0xc0000000 +0x20000000`, `0xe0d00000 +0x1f300000` |
| EL3 | `el3mon_reserve` at `0xbff80000` (+512 KiB); a Samsung hypervisor (`uh` partition) is likely at EL2, so the kernel probably starts at EL1 (a21sdiag says) |
| interrupts | **GIC-400 (GICv2)**: `arm,cortex-a15-gic`, GICD `0x12a01000`, GICC `0x12a02000`, GICH `0x12a04000`, GICV `0x12a06000`. Android's kernel has `CONFIG_ARM_GIC=y` and no GICv3 |
| timer | `arm,armv8-timer`, **26 MHz**, physical timer (`use-physical-timer`) |
| watchdogs | `0x10050000` (cluster 0), `0x10060000` (cluster 1) |
| PMU (power) | `pmu_alive` at `0x11860000` |
| ACPM | the APM core's IPC at `0x02039000` / `0x11900000`: the PMIC is behind it (see *PMIC*) |

The CPU is the important difference from the A32 for XNU: eight A55s, no A75. Like the A32's
cores they are ARMv8.2 without pointer authentication, which is what the iPhone X's (A11,
arm64, not arm64e) iOS 16 user space expects. XNU's 16 KiB pages need the A55's TGran16 (it has
it; a21sdiag prints ID_AA64MMFR0 to make sure).

**The GIC is the big kernel-side item.** The common tree's interrupt code is written for the
GICv3 of the MediaTek and Qualcomm phones; the Exynos 850 has a GICv2 (memory-mapped CPU
interface, no system registers, 8 CPUs max, SGIs through GICD_SGIR). That is a new, small,
well-documented driver, but it has to exist before XNU takes its first interrupt.

## GPU: the part that carries over

| | |
| --- | --- |
| GPU | **Mali-G52**, `gpuinfo: Mali-G52 1 cores r0p0 0x7212` (A32: G52 r1p0, product 0x7402, 2 cores) |
| registers | `0x11500000`, size `0x5000` (`compatible = "arm,mali"`) |
| interrupts | GIC SPI 126 `JOB`, 127 `MMU`, 125 `GPU`, level high |
| cores | shader core mask **0x1** (one core; the A32 has 0x3) |
| power domain | `pd-g3d` in the PMU at `0x11861d00` (configuration), status `0x11861d04` bit 0 (`gpu_pmu_status_local_pwr_mask`); the node has `need_smc`: the domain may have to be switched through EL3 |
| clock | CMU_G3D (cal-id `0x0b040004`), DVFS 377–1001 MHz (`377 433 598 754 865 1001` MHz), boot default 377 MHz |
| voltage | `vdd_gpu` (S2MPU12 buck), 812.5 mV now; default 800 mV in the device tree |
| Android driver | ARM's `mali_kbase` (`/dev/mali0`), power policy `coarse_demand` |

The Mali-G52 is Bifrost (v7) on both phones. What does not depend on the SoC moves as it is:
- the job manager programming, address spaces and page tables, the L2/tiler/shader power-up
  sequence and the two test jobs: steps 4 to 6 of `hakios.mali.c`;
- `RedwoodMali` (the panfrost DRM interface to Mesa), Mesa's panfrost and its Bifrost compiler
  (product 0x7212 is in panfrost's table as G52), the `y2sios` Metal runtime, CoreAnimation on
  the GPU.

What does depend on it is steps 1 to 3 (and the core mask):
- power: instead of MediaTek's SPM domains and bus protection, the Exynos PMU's `pd-g3d`,
  possibly through an SMC (ACPM/EL3), as the `need_smc` property says;
- clock: instead of topckgen's `mfg_sel` mux and mfgcfg's gate, CMU_G3D's PLL/mux/gates;
- voltage: `vdd_gpu` is already up when Android runs; whether it stays up from S-Boot, or the
  loader has to ask ACPM for it, is for a21sdiag's PD_G3D line and the next step to find out;
- one shader core: mask 0x1, half the A32's throughput (expect ~10–15 fps where the A32 had
  21–31 on the same scenes, at 720×1600 instead of 1080×2400 it is less work too).

## Display

| | |
| --- | --- |
| panel | BOE, id `8a6220` (`lcd_type`), **720 × 1600**, 320 dpi, TFT with touch in the panel (TDDI) |
| path | DECON-F `0x13060000` (`samsung,exynos9-decon`, one DSIM, four DPP layers) → DSIM `0x13070000` (+ D-PHY `0x13080000`) |
| layers | DPP at `0x130b1000`, `0x130b2000`, `0x130b3000`, `0x130b4000` (+ DMA `0x13054000`) |
| boot framebuffer | `bootloader_fb` (`exynos,fb_handover`) at **`0xfa000000`**, 20 MiB; `s3cfb.bootloaderfb=0xfa000000` on the command line |
| mode | `trig_mode = 1` and a TE pin (`hw_te_on/off`) in the DECON node: possibly command mode with a hardware TE trigger, to be read off the DSIM registers (a21sdiag logs them) |

If the panel turns out to be video mode, the framebuffer is scanned out continuously and the
A32's DSI "kick" is not needed at all. If it is command mode, DECON's trigger has to be fired per
frame, as on the A32.

## Touch

**Synaptics TCM over SPI**: `sec_touchscreen` on `spi1.0`, driver `synaptics_tcm_spi`, SPI
controller `0x11d00000`. The device tree lists Himax (`hx83112a`) and Novatek nodes too, for
the other panel variants; on this phone the BOE panel's controller is the Synaptics one. The
common tree's loader already speaks TCM over SPI (`mtkloader/tcm.h`, and `tools/mktouchfw.py`
for a TDDI controller that loses its firmware at every reset), written for a MediaTek SPI
controller: the protocol carries over, the SPI controller (Exynos USI) is new.

## Buttons

- Volume up/down: `gpio_keys` (plain GPIOs: the easy part).
- Power: `s2mpu12-power-keys`, i.e. the PMIC, read over ACPM (as on the A32, power is the PMIC's).

## PMIC

**S2MPU12** (`pmic_info=27`), behind the ACPM (`acpm_mfd_bus@11A50000`, i2c-10): its registers
are not on a bus the AP drives directly but through the APM core's mailbox. LDOs seen:
`vdd_ldo1..32`, `vdd_aldo1..2`, `vdd_gpu` (812.5 mV), `vdd_ldo28` 1.8 V (the panel/touch rail,
`himax,lcd_regulators_pwr1`). Charger and haptics are an S2MU106 on I2C `0x13830000`.

## Storage

eMMC (`MMC`, 64 GB class, 122142720 sectors) on **DW MMC** at `0x12100000`
(`androidboot.boot_devices=12100000.dwmmc0`), not MediaTek's MSDC. Partitions are the usual
Samsung legacy non-A/B layout with dynamic partitions:

| partition | size | |
| --- | --- | --- |
| `boot` (mmcblk0p18) | 36 MiB | the only partition HakiOS writes |
| `recovery` (p19) | 53 MiB | TWRP on this phone |
| `dtb` (p20), `dtbo` (p21) | 8 MiB each | |
| `super` (p30) | 9.3 GiB | system/vendor/product, dynamic |
| `userdata` (p35) | 46.7 GiB | where `xnusys` would be carved from (4 GiB), as on the A32 |

## Boot

- Bootloader: Samsung **S-Boot** (Exynos), not LK. Unlocked (`flash.locked=0`, orange state).
- `boot` is an Android boot image **header v2**, page 2048:
  - kernel: **uncompressed** arm64 Image, `text_offset 0x80000`, image_size 0x2b81000, flags 0xa;
    the header's load addresses (`0x10008000` …) are the mkbootimg defaults that S-Boot ignores;
  - ramdisk: uncompressed cpio, ~1.5 MB;
  - dtb: 140 KB inside the boot image (v2's dtb field), the board's overlays in `dtbo`;
  - no AVB footer and no `SEANDROIDENFORCE` trailer.
- With the program in front, the raw image leaves ~3 MB of the 36 MiB.
- **S-Boot does not inflate a gzip kernel** (tested 2026-11-04 with `GZIP=1`: no bars, a boot
  loop, and `/proc/last_kmsg` afterwards held no Linux boot at all). The kernel slot is taken as
  a raw Image, so XNU and its payload cannot travel in `boot`: the loader reads them off the
  eMMC (see *xnusys* below).

## xnusys: in the free tail of `super`, not carved from `userdata`

The A32 carves 4 GiB from the end of `userdata`, which means formatting it. Here `userdata` is
f2fs, mounted straight from `mmcblk0p35` (no dm layer) with 28 GB in use, so carving it is not
free. `super` (9.32 GiB) has room instead: its four dynamic partitions take 5.4 GB, all at the
start, and liblp's metadata (v10, two slots, its backups at the *start* of the device) declares
only 8.50 GiB of the partition as its block device.

| super sectors | what |
| --- | --- |
| 2048 – 11347384 | odm, product, system, vendor (one extent each) |
| 11347384 – 11479040 | 64 MiB guard, left free |
| **11479040 – 19554304** | **xnusys: 8075264 sectors = 3.851 GiB** (3.03 GiB inside liblp's space, 0.82 GiB beyond it) |

On mmcblk0 that is **`xnusys_lba=12216320 xnusys_sectors=8075264`**, contiguous, read as zeros
today (sampled from MiB 5605 to the last MiB of `super`). The A32's iOS system image uses
~2.5 GB of its 4 GiB, so 3.85 GiB is enough if the image is made at that size.

`tools/xnusys.py` computes the region from the phone's own metadata every time and refuses if an
extent has grown into it; it writes through `/dev/block/by-name/super` with `seek`, so a write
cannot leave that partition. The price: **flashing another ROM, or resizing the dynamic
partitions, may hand the 3.03 GiB inside liblp's space back to Android and overwrite the iOS
image** (`xnusys.py verify` would then say so). It never harms Android: liblp only allocates
there when it rewrites the partitions anyway. If that becomes a problem, the image can be
registered in super's metadata as a logical partition of its own (`lpadd`), or moved to a
carved `xnusys` the A32's way.

The GPT, both copies, is saved with the stock images (`gpt-primary.bin`, `gpt-backup.bin`).

## Logs across a boot

Android's ramoops: `mem_address 0xf0f10000`, `mem_size 0x8000`, `record_size 0x4000`,
`pmsg_size 0x4000`, **`console_size 0`**, ecc off. So there is no `console-ramoops`; the
16 KiB **pmsg** zone at `0xf0f14000` is where bare-metal code leaves its text. Android shows it
as `/sys/fs/pstore/pmsg-ramoops-0` (root). The kernel's own (Samsung) `debug_snapshot` area is
bigger and may serve later for XNU's log.
