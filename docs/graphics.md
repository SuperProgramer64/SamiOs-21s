# Mali acceleration and native iOS wallpapers

## Working path

The physical A21s uses **Mali-G52, GPU ID 0x7212**, with Mesa **25.0.7 Panfrost/OpenGL ES**. The kernel's `mali=2` path uses Exynos registers and checks the existing power-domain state. The A32's `mali=1` power sequence is MediaTek-specific and must not be used on this phone.

`kernel/drivers/gpu/hakios.mali.c` and `hakios.mali.cpp` provide the kernel GPU/DRM-compatible transport, buffers and submission. The Darwin libdrm shim and Mesa Panfrost changes are included under `graphics/`; `kernel/userland/src/eagl.c` and `gl_osmesa.c` expose the GL path to CoreAnimation.

The physical v10 log identified `OpenGL ES 3.1 Mesa 25.0.7; Mali-G52 (Panfrost)`. Its last recorded GPU counter was **51,197 job chains, zero failed**. Frame cadence varied with the interface; this does not establish 60 fps or eliminate CPU/render-tree costs.

QEMU uses a software renderer. Its color-triangle checks validate that route and some interfaces; they do not emulate Mali, GPU power or Samsung radio hardware.

## Native wallpaper fix

`kernel/userland/src/display_native.c` hooks the measured 20H392 WallpaperKit methods only after checking class, selector, method kind, ABI and implementation location:

- `-[WKDefaultWallpaperManager deviceLogicalScreenClass]` selects `@375w-812h@3x~iphone` for this supported native profile.
- `+[WKWallpaperBundle shouldLoadWallpaperBundleAtURL:]` permits the matching stock variants and filters redundant larger-device alternatives while preserving the original decision for other bundles.

The fixed variant string selects a stock resource; it does not change the panel to a 3× screen. No static gradient or substitute wallpaper is installed by this fix.

PosterBoard/PaperBoard jobs and target-process injection entries are retained. The complete working Mesa/EAGL libraries were restored when auditing the earlier v9 candidate, whose ramdisk had omitted them. Guarded 20H392 CoreAnimation function/effect-uniform hooks are in `inject.c` and `ca_profile.h`.

The user confirmed an original wallpaper on the physical v10 device, following a short black interval. The native picker, all wallpaper collections and extension stability are still incomplete. PosterBoard aborts were also recorded, so visual success is not a claim that every wallpaper process is healthy.

## Metal / Vulkan

There is **no working Mali Metal/Vulkan bridge in this A21s snapshot**. A32 notes describe a separate Metal runtime, but its userspace runtime is not installed here. The current acceleration is Panfrost/OpenGL ES. An eventual Metal frontend must be validated against this GPU, shader compiler, IOSurface synchronization and the correct iOS build.

The source-only Mesa build recipe and exact-source patch are described in `../graphics/README.md`. No Apple metallib or compiled driver is redistributed.
