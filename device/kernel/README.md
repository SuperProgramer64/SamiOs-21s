# The A21s's pieces of XNU

APSL 2.0, like everything built into XNU. They go over the common tree (`hakios-xnu-kernel`,
`xnu-8796.121.2`): `hakios-xnu-kernel.patch` is every change to it in one diff (it applies to a
clean clone with `git apply`), and the files below are copies of what it adds, kept here to be read.

| file | in the common tree | what |
| --- | --- | --- |
| `osfmk/arm64/gic_v2_irq.h` | `xnu/files/osfmk/arm64/` (new) | the **GICv2 (GIC-400)** the Exynos 850 has: memory-mapped distributor and CPU interface, non-secure Group 1, the EL1 virtual timer on PPI 27, SGI 0/1 for `cpu_signal()` and the stop order, SPIs attached by drivers, routed by CPU-interface number (each processor learns its own from `GICD_ITARGETSR0` and records it against its MPIDR) |
| (in the patch) | `xnu/files/osfmk/arm64/gic_v3_irq.c` | includes `gic_v2_irq.h`; every public call (`gic_v3_irq_cpu_init`, `_signal`, `_signal_stop`, `_spi_attach/mask/unmask/detach`, `_quiesce`, `_cpu_shutdown`) goes to the v2 code when `/arm-io/gic` is compatible with `arm,gic-400`, `arm,cortex-a15-gic` or `arm,gic-v2`. Anything else (the A32, the Qualcomm phones, QEMU) takes the GICv3 path unchanged |
| `bsd/dev/hakios.mali.c` | `drivers/gpu/` (new driver `MALI`) | the Mali-G52 brought up from the kernel, the A32's driver with an Exynos 850 path: **`mali=1`** the A32 (MT6769: topckgen, SPM, mfgcfg, unchanged), **`mali=2`** the A21s (Exynos 850: checks the PMU's `pd-g3d` is on, leaves the clock as S-Boot set it, maps the GPU at `0x11500000`, checks it is a G52; never switches the domain on itself, that needs EL3). Steps 4–6 (reset, L2/tiler/shaders with the masks the GPU reports, the two test jobs) are shared |
| `iokit/Kernel/hakios.mali.cpp` | `drivers/gpu/` | `RedwoodMali`, the panfrost DRM interface Mesa draws through; unchanged from the A32 (every parameter is read from the GPU, the timestamp is 26 MHz on both phones) |
| (in the patch) | `drivers/gpu/conf`, `drivers/map` | the `MALI` driver: both files, `CONFIG_HAKIOS_MALI` |
| (in the patch) | `drivers/input/hakios.input.c`, `xnu/files/bsd/sys/hakios.input.h`, `drivers/core/hakios.stubs.c` | `hakios_mali_init()` started beside `hakios_adreno_init()`; an empty one when the driver is left out |
| (in the patch) | `xnu/files/iokit/Kernel/MiniIOSurface.h`, `drivers/gpu/hakios.gpu.cpp` | the A32's `maliSeq` on every surface, and `MiniIOSurfaceDrawn()` waiting for that Mali job (`redwood_mali_wait`) before the display shows the surface; under `#ifdef HAKIOS_MALI` |
| (in the patch) | `docs/boot-args.md`, `configs/boot-args.lst`, `configs/symbols.tsv` | `mali`, `mali_fastmap`, `mali_trace`, regenerated with `tools/boot-args.py` |
| (in the patch) | `sysimage/unpack.sh`, `userland/build.sh` | `BUILD=20H392`: the system image from the 16.7.16 IPSW |

Nothing else in the kernel touches the GIC: `GenericARMPE.cpp`, `psci_smp.c`, `loader_return.c` and
the drivers only call the functions above, and nothing outside `gic_v3_irq.c` uses the `ICC_*`
system registers (which a GICv2 does not have).

## What is not ported, and why

| A32 piece | A21s |
| --- | --- |
| `MiniIOMobileFramebuffer.cpp`'s `mtk_kick` | not needed: the A21s's panel is video mode (DECON `psr_mode` 0), the common framebuffer driver draws into S-Boot's framebuffer as the loader hands it over (`fbwidth`/`fbheight`/`fbscale`) |
| `hakios.fts.c` (STM FTS on bit-banged I2C) | a different chip and bus: the A21s's touch is Synaptics TCM over SPI (Exynos USI) - a new driver |
| `hakios.mtkkeys.c` (MTK keypad, MT6358) | new: volume on Exynos GPIOs (`gpio_keys`), power on the S2MPU12 behind ACPM |
| `hakios.mtkbattery.c`, `hakios.mtkcpufreq.c`, `hakios.scp.c` | MediaTek-only (MT6358 ADC, MediaTek PLLs, the SCP hub) |

## What the loader has to say

`/arm-io/gic` in the device tree exyloader builds (step 3), relative to `/arm-io`'s ranges:

```
compatible = "arm,gic-400"
reg = <0x12a01000 - base, 0x1000,     the distributor
       0x12a02000 - base, 0x2000>     the CPU interface
```

the CPUs' `reg` as their MPIDRs' low affinity fields: `0x000, 0x100, ... 0x700` (the A55s have
MPIDR.MT set, the core number is Aff1), and `mali=2` in the boot arguments.

## Built (2026-11-04)

The common tree builds XNU on macOS with Xcode, without `sudo`:

```sh
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
xnu/apply.sh && JOBS=6 xnu-deps/setup.sh       # once
KERNEL_LOG=kernel-build.log tools/make-kernel.sh -j6
```

With everything above: **26 of 26 drivers**, `hakios-kernel` 16.9 MB; no compiler warning in the
files touched (the 52 warnings of the build are libtool's "has no symbols" for empty stubs), and
`tools/make-kernel.sh` judged the failures to be only the expected kext symbol-set checks. In the
binary: `_gic2_irq_handler`, `_hakios_mali_init`, `_RedwoodMaliStart`, `_redwood_mali_wait`, the
"mali: Exynos 850: PD_G3D on ..." message, and `MiniIOSurfaceDrawn` branching to
`redwood_mali_wait`. `tools/drivers-check.py` and `tools/boot-args.py --check` pass.

**Not run on the phone yet**: that needs exyloader's steps 2–4.

On this Mac (a Ryzen Hackintosh) a 12-job build froze the machine once; 6 jobs went through.
