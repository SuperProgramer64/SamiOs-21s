# Reproducing the tested Panfrost sources

`mesa-25.0.7-a21s.patch` is the complete delta from the official Mesa 25.0.7 archive to the source tree used for the A21s Panfrost build. It includes the HakiOS common patches, the A32 Panfrost/DRM integration, and the local Meson/OSMesa fixes. **Apply this complete patch alone; do not apply the component patches on top of it.**

Original archive: [Mesa 25.0.7](https://archive.mesa3d.org/mesa-25.0.7.tar.xz).

SHA256: `592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6`.

```sh
shasum -a 256 mesa-25.0.7.tar.xz
tar xf mesa-25.0.7.tar.xz
cd mesa-25.0.7
git apply --check /path/to/SamiOs-21s/graphics/mesa-25.0.7-a21s.patch
git apply /path/to/SamiOs-21s/graphics/mesa-25.0.7-a21s.patch
```

The publication process verified the archive checksum, applied the patch to pristine sources, and compared all 54 changed source files byte-for-byte with the tested tree. A generated patch is not by itself a fresh full graphics build; see the recorded validation limits.

`build-panfrost.sh` generates a macOS/iOS cross file and builds OSMesa with `softpipe,panfrost`. It expects the patched Mesa tree, the common tree's prepared compiler wrappers/SDK, Meson, Ninja, Python build dependencies, Bison and Flex on PATH. Set `MESA_TREE` to the patched tree and optionally `JOBS` (default 3). `package-panfrost.sh` builds the matching EAGL shim and GL diagnostic from the common source.

The `a32-reference/` directory preserves the original component patch, AIR fix and DRM shim with their license. They are reference material; the complete A21s patch already incorporates the applicable tested changes. The optional AIR source in the patch does not mean a Metal runtime is enabled or complete.

No compiled graphics library, vendor firmware or Apple shader library is included.
