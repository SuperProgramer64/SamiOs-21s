# Architecture and implemented changes

## Boot, CPUs and interrupts

The A21s uses Samsung S-Boot, an Exynos 850 and a GIC-400 (GICv2). The loader is a raw ARM64 Image chained ahead of Android; Samsung's loader does not inflate a gzip kernel. The Apple-style device tree is generated locally from the builder's software copy.

The GICv2 implementation supplies timer/SGI/SPI routing through the existing generic interrupt API. The Exynos MPIDR affinity layout is handled explicitly. PSCI function-ID aliases, learned IDs and the loader trampoline are gated and tested. Eight physical cores were observed; QEMU is supplementary evidence, not proof of the Samsung CPU-start sequence.

Relevant sources: `device/loader/`, `kernel/xnu/files/osfmk/arm64/gic_v2_irq.h`, `gic_v3_irq.c`, `psci_smp.c`, and the PSCI regression tests.

## Storage and persistent data

`kernel/drivers/storage/hakios.dwmmc.c` implements the Exynos DW-MMC path. CMD18 is the main multiblock read path, with bounded PIO handling, a checked CMD17 comparison/fallback, R1/error checks, and diagnostics. Physical effective transfer measurements reached approximately 42 MiB/s; this is not a benchmark of every filesystem workload.

Writes are restricted to a separately validated data window and enabled by explicit arguments. A journaled HFS+ filesystem provides writable `/private/var`; the system APFS remains read-only. Existing data is preserved during payload updates. The staging tool validates Android logical-partition extents, capacities, APFS geometry and HFS headers before permitting a write.

The eMMC log writer uses its own bounded reservation. Logs were successfully recovered after orderly exits. See [storage](storage.md).

## Touch and buttons

The TD4150 driver uses Exynos USI/SPI with per-burst FIFO bounds, firmware loading, ready/status validation, bounded retries, touch-report bounds checks, contact release and recovery. A larger prior response cannot expand subsequent ready-state reads beyond 63 bytes.

The physical v10 run produced 4,605 touch reports and 65 padding rejections, with no reported bus failures or touch reinitialization. Eight captured malformed packets had the same inserted `a5 03` continuation prefix after byte 9. Removing that exact transport prefix restored valid report coordinates in tests.

The v11 candidate adds `touch_inline_cont=1`: only that exact report framing is reconstructed, with adjusted terminal and trailing padding checked. Other malformed packets remain rejected. `touch_coalesce=1` replaces consecutive unread MOVE records for the same finger with the latest sample, preserving transitions, buttons and other fingers. Both behaviors are separately gated; their physical gesture quality remains under test.

Volume events use Exynos GPIO keys. Holding both volume keys for approximately three seconds provides an orderly return path that preserves logs better than forcing a hardware reset. Power/lock integration remains incomplete.

## Display, migration and user space

The native profile is 720×1600 at scale 2 (360×800 logical points). Display identity/cache correction, keyboard size fallbacks, and guarded method checks target the measured 20H392 implementation. This includes Spotlight keyboard handling; a successful patch on another iOS build is not assumed to match this one.

Migration state is seeded from the actual system version instead of a fixed 20F66 value. Kernel injection configuration/environment storage was enlarged to 4096/2048 bytes so the wallpaper processes are not silently truncated.

CoreAnimation, EAGL and WallpaperKit changes are described in [graphics](graphics.md). Some services are on-demand; enabling every missing daemon at once previously caused crash loops and is not represented as a general fix.

## Current candidate

The v11 payload was installed and read back with SHA256 verification. The boot image was unchanged. It retains the byte-identical v10 ramdisk and graphics/wallpaper stack, firmware and migration helper. Only the kernel and candidate touch arguments changed. See `validation.md` for hashes and limits.
