# hakios-a21s

HakiOS for the **Samsung Galaxy A21s (SM-A217F / SM-A217M)**: Samsung **Exynos 850**,
8 × Cortex-A55, GPU **Mali-G52 MP1**, 4 GiB, eMMC. The goal is the A32's: the iPhone X's
unmodified iOS 16 user space (16.7.16, 20H392) on Apple's open XNU (xnu-8796), with this
phone's own loader and drivers, and the GPU working from the start, since the A21s's Mali-G52
is the same Bifrost GPU the A32 already runs CoreAnimation and Metal on.

This is the beginning of the port: the hardware has been read off a running phone
([`docs/hardware-a21s.md`](docs/hardware-a21s.md)) and the first bare-metal diagnostic is ready
([`diag/`](diag/)). Nothing has run on the bare metal yet.

## What carries over from the A32, and what does not

| | from `hakios-a32` | new for the A21s |
| --- | --- | --- |
| iOS user space, identity | all (iPhone X, `mkidentity.py`, the two `default.metallib`) | — |
| GPU | job manager, MMU, power-up of L2/tiler/shaders, test jobs, `RedwoodMali`, Mesa panfrost (G52 0x7212), Bifrost compiler, `y2sios` Metal | power domain (PMU `pd-g3d`, maybe through an SMC), CMU_G3D clock, core mask 0x1 |
| boot | the chain in `boot` in front of Android's kernel, the return to Android | S-Boot instead of LK, an uncompressed Image, 36 MiB `boot`, a load address to be found |
| interrupts | — | **GIC-400 (GICv2)** in XNU |
| display | the loader's framebuffer/console idea | DECON/DSIM (Exynos), 720 × 1600 BOE TDDI |
| touch | the TCM protocol (`tcm.h`), the firmware packer | the Exynos USI SPI controller |
| storage | the read-only `xnusys` idea | DW MMC; `xnusys` in the free tail of `super` (3.85 GiB, `tools/xnusys.py`), `userdata` untouched |
| PMIC, buttons, battery | — | S2MPU12 behind ACPM; volume on GPIOs |

## What is here

- `diag/`: `a21sdiag`, the first program in front of Android's kernel (MIT).
- `tools/chainboot.py`: packs a program in front of the A21s's own Android kernel (MIT).
- `tools/probe-a21s.sh`, `tools/probe-a21s-dt.sh`: the read-only probes the hardware notes
  come from (run them as root on the phone; their output stays off the repository).
- `tools/mkidentity.py`: `xnuloader/identity.h` from your own IPSW's DeviceTree.
- `tools/xnusys.py`: where the iOS system image goes (the free tail of `super`), and writing it there.
- `docs/hardware-a21s.md`: what was found, with addresses.

## Not here, by design

No file of iOS, no firmware of the phone, no boot/partition image, no `identity.h`, no probe
output (it holds the phone's serials). See `AGENTS.md`.

## License

MIT (`LICENSE`); the kernel pieces, when they come, APSL 2.0.
