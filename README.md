# SamiOs-21s

Experimental HakiOS/XNU port for the **Samsung Galaxy A21s, Exynos 850**, using an iPhone X iOS 16.7.16 (20H392) userspace supplied by the builder.

This repository collects the common kernel changes, A21s loader and device tools, Mali/Panfrost integration, native wallpaper fixes, tests, and engineering notes from the work through **6 November 2026**. It is a source snapshot, not a complete iOS distribution or a finished daily-use operating system.

## Current status

| Component | Evidence and remaining work |
|---|---|
| Boot and desktop | SpringBoard, Settings, Notes and other interfaces have run on the physical A21s. |
| CPUs | Eight Cortex-A55 cores enabled with the Exynos/GICv2 and PSCI adaptations. |
| Storage | CMD18 multiblock eMMC reads measured around 42 MiB/s of effective transfer time. CMD17 remains a checked fallback. |
| Persistent data | Writable, journaled HFS+ `/private/var` on a strictly bounded eMMC window; persistence verified across runs. The iOS system APFS stays read-only. |
| Graphics | Mali-G52 acceleration through Mesa 25.0.7 **Panfrost/OpenGL ES**. A physical run recorded 51,197 job chains with zero reported job failures. This is **not a working Metal/Vulkan bridge**. |
| Native wallpapers | Original iOS wallpaper appeared on the physical device after an initial black interval. WallpaperKit screen-class/bundle selection and guarded 20H392 CoreAnimation hooks are included. The full picker and every collection are not yet verified. |
| Touch | TD4150 firmware loading and events work. The v11 candidate adds strict inline TCM continuation reassembly and unread MOVE coalescing; physical gesture refinement remains under test. |
| Buttons | Volume events work. Power/lock behavior is not established. |
| Wi-Fi / Bluetooth | Samsung SCSC/WLBT hardware identified. Register/interrupt and firmware startup research is documented; **no working XNU radio driver yet**. This is the next priority. |
| Apps | Several apps still crash, hang or lack hardware/services. See [application status](docs/applications.md). |

## Layout

- [`kernel/`](kernel/): HakiOS common sources, XNU patch, drivers, userland hooks, tests and build tooling.
- [`device/`](device/): Exynos A21s chain loader, guarded storage/log tools and device tests.
- [`graphics/`](graphics/): A32-derived Panfrost changes, Darwin DRM transport, and a patch reproducing the tested Mesa source tree.
- [`docs/`](docs/): current findings, build notes, storage boundaries, testing evidence and provenance.
- [`tools/`](tools/): source verification, firmware-header generation, and the USB-disconnect restart gate.
- [`patches/`](patches/): changes against the documented common-tree base, for review alongside the full sources.

Start with [build notes](docs/build.md), [architecture and changes](docs/changes.md), [graphics and wallpapers](docs/graphics.md), and [wireless bring-up](docs/wireless.md).

## Verification

On a host with Python 3, Clang and the test dependencies:

```sh
python3 -m pip install -r tools/requirements-tests.txt
python3 tools/verify.py
```

These execute real driver/parser/callback code with bounded hardware substitutes. Passing host tests does not establish radio connectivity, perfect gestures, or application compatibility on a phone. QEMU does not emulate the Exynos SCSC radios or Mali-G52 hardware.

## Source-only distribution

No IPSW, Apple userspace/cache/metallib, phone firmware, boot/partition image, private logs, serial number or personal data is included. Obtain required images and firmware from your own device and software copy. Do not format or replace an existing persistent data volume when updating a kernel payload.

## Credits and licensing

This work builds on [HakiOS common](https://github.com/oosexgang/hakios-xnu-kernel), the HakiOS A21s/A32 device projects, [Bruhboy666dev's A217F work](https://github.com/Bruhboy666dev/ios-a217f/tree/codex/a217f-profile-20261102), and [Schoosh/simpleXNU's A135F port](https://github.com/simpleXNU/ios-a135f). The imported touch and USB sources retain their attribution. Mesa/Panfrost and the Darwin DRM transport retain their upstream notices.

Kernel modifications retain **APSL 2.0** terms; loader, tooling and other original material retain their stated licenses, generally **MIT**. See [LICENSE](LICENSE), [LICENSE.APSL](kernel/LICENSE.APSL), [NOTICE](NOTICE.md), and the component-specific notices. Naming this collection SamiOs-21s does not change upstream ownership or licenses.
