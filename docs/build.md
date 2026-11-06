# Building and updating

This is a source snapshot of a working development environment, not a one-command installer. Review the component-specific build scripts. Private inputs, generated identity files and firmware must be supplied locally.

## Host and kernel

The tested host was macOS with Xcode and a cross-build environment. Python 3, Clang, an ELF `ld.lld` (or Zig), and the dependencies listed by HakiOS are required. Avoid excessive parallelism on a memory-constrained host; three build jobs were used for the recent candidates.

```sh
cd kernel
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
./xnu/apply.sh
JOBS=3 ./xnu-deps/setup.sh
bash tools/make-kernel.sh -j3
```

The base is Apple's open-source `xnu-8796.121.2`. `xnu/hakios.patch` includes changes to Apple's files; `xnu/files/` and `drivers/` hold the additional sources. The 4096-byte injection configuration and 2048-byte environment buffer changes are in the patch, not only in an untracked build tree.

The kernel build wrapper distinguishes actual compilation/link failures from this configuration's known unsupported kext symbol-set checks. Read its final verdict rather than ignoring all errors. No prebuilt kernel or proprietary firmware header is included. The A21s TD4150 firmware is loaded separately; the unrelated FocalTech firmware path refuses startup when its private header is absent.

## iOS system and userland

Use your own iPhone X 16.7.16 / 20H392 IPSW. The build SDK is 16.5; this is a compiler input and does not mean the installed system or guarded runtime methods are 20F66.

```sh
cd kernel
BUILD=20H392 KEEP=1 bash sysimage/unpack.sh /path/to/your/20H392_Restore.ipsw
```

Follow `kernel/userland/build.sh` for the system image, stage0/init, injection libraries and ramdisk. Keep the migration helper and generate migration state from the actual `SystemVersion.plist` through `seed-migration.py`. Preserve the full GL libraries when assembling a ramdisk; enabling `gl=1` without them does not provide acceleration.

The exact tested Panfrost source recipe is under `graphics/`. Native-display/wallpaper changes are in `display_native.c`; CoreAnimation changes are in `inject.c` / `ca_profile.h`. The native profile is `hakios_native_ui=1 fbwidth=720 fbheight=1600 fbscale=2 fbdpi=270 fbsubtype=2436`.

Stock PosterBoard/PaperBoard jobs and the injection target names must be retained. The repository includes common job generation, but the historical common `build.sh` is not claimed to reproduce every private ramdisk customization automatically. Inspect the generated job and injection configuration before staging a candidate.

## A21s loader and payload

The A21s loader is in `device/loader/`; its boot-image builder requires a saved Android boot image. Generate `device/xnuloader/identity.h` from your own software copy using `device/tools/mkidentity.py`. It is intentionally ignored.

`STEP`, `XNU_CORES`, `XNU_WDT_RUN`, `XNU_EMMC_ROOT`, `XNU_STORAGE_ARGS`, `XNU_PSCI_LEARN`, `XNU_PSCI_TRAMPOLINE`, and `XNU_IMPORT_ARGS` select the loader profile through its documented compiler flags. Do not use the early default diagnostic profile as the final installed configuration. The tested profile uses eight CPUs, a 180-minute watchdog and the guarded persistent-data/log regions.

`kernel/xnuloader/mkpayload.py` packs the kernel, ramdisk, locally supplied `touchfw`, version marker and command line. v11 adds `gl=1 glcensus=1 touch_inline_cont=1 touch_coalesce=1` to the installed loader's arguments. Do not claim a newly built payload matches the recorded artifact hashes unless you actually compare it.

## Device writes

Read `device/AGENTS.md` and [storage](storage.md). The guarded `device/tools/xnusys.py` must validate the actual device layout. Always select the phone explicitly with its serial, supplied locally. A custom ROM may change the reported Android model name; hardware identification cannot rely on that string alone.

Do not create a fresh APFS/data filesystem merely to update a payload. Preserve the existing HFS+ data volume. Before a restart, save the preceding eMMC/pmsg logs, verify payload readback and the installed boot image, and use `tools/xnu-run-safe.sh` only with the matching loader.

## Tests and QEMU

```sh
python3 tools/verify.py
```

Install `tools/requirements-tests.txt` for the loader instruction tests. After building the kernel, `python3 tools/verify.py --extended` also runs the PSCI instruction/reset-vector tests. Those tests require the generated XNU source/build offsets and are not part of the dependency-light default command.

The common tree retains QEMU scripts and virtual input/block drivers. Use a private clone of any persistent data snapshot, one emulator at a time. Software rendering can be very slow and some wallpaper extensions time out; a black QEMU capture does not overturn the physical wallpaper observation. QEMU cannot validate TD4150 SPI timing, Exynos power sequencing, SCSC radio connectivity or Mali hardware execution.
