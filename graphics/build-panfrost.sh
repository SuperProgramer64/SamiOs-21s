#!/bin/bash
# Isolated iOS Mesa/Panfrost build. No phone access and no shared-tree edits.
set -eu
TASK_DIR=$(cd "$(dirname "$0")" && pwd)
COMMON=$(cd "$TASK_DIR/../kernel" && pwd)
source "$COMMON/xnu-env.sh"
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
: "${MESA_TREE:?Set MESA_TREE to the patched Mesa 25.0.7 source directory}"
MESA_TREE=$(cd "$MESA_TREE" && pwd)
SDK=${SDK:-"$THEOS/sdks/iPhoneOS16.5.sdk"}
cat > "$TASK_DIR/cross.ini" <<EOF
[binaries]
c = ['$COMMON/xnu-deps/host/bin/xnu-clang', '-arch', 'arm64', '-isysroot', '$SDK']
cpp = ['$COMMON/xnu-deps/host/bin/xnu-clang++', '-arch', 'arm64', '-isysroot', '$SDK', '-fuse-cxa-atexit']
objc = ['$COMMON/xnu-deps/host/bin/xnu-clang', '-arch', 'arm64', '-isysroot', '$SDK']
ar = '/usr/bin/ar'
ranlib = '/usr/bin/ranlib'
strip = '/usr/bin/strip'
pkg-config = 'false'
[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
[properties]
needs_exe_wrapper = true
EOF
cd "$TASK_DIR"
if [ ! -f build/build.ninja ]; then
 python3 -m mesonbuild.mesonmain setup build "$MESA_TREE" --cross-file cross.ini --wrap-mode=nofallback \
  --prefix=/redwood --libdir=lib --buildtype=release -Db_ndebug=true \
  -Dplatforms= -Dgallium-drivers=softpipe,panfrost -Dvulkan-drivers= -Dglx=disabled -Degl=disabled -Dgbm=disabled \
  -Dosmesa=true -Dllvm=disabled -Ddraw-use-llvm=false -Dshared-glapi=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
  -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled -Dvalgrind=disabled \
  -Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-vdpau=disabled -Dgallium-va=disabled \
  -Dgallium-xa=disabled -Dlmsensors=disabled -Dbuild-tests=false > setup.log 2>&1
fi
ninja -C build -j3 src/gallium/targets/osmesa/libOSMesa.8.dylib > build.log 2>&1
