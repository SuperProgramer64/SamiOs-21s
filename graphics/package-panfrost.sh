#!/bin/bash
# Standalone diagnostic artifacts only. Never contacts or writes the phone.
set -eu
TASK_DIR=$(cd "$(dirname "$0")" && pwd)
COMMON=$(cd "$TASK_DIR/../kernel" && pwd)
source "$COMMON/xnu-env.sh"
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
OUT="$TASK_DIR/out"
mkdir -p "$OUT"
SDK=${SDK:-"$THEOS/sdks/iPhoneOS16.5.sdk"}
CC="$COMMON/xnu-deps/host/bin/xnu-clang"
/usr/bin/strip -x -o "$OUT/libOSMesa.8.dylib" "$TASK_DIR/build/src/gallium/targets/osmesa/libOSMesa.8.dylib"
/usr/bin/install_name_tool -id /redwood/lib/libOSMesa.8.dylib "$OUT/libOSMesa.8.dylib"
/usr/bin/nm -gU "$OUT/libOSMesa.8.dylib" | awk '$3 ~ /^_gl/ { print substr($3, 2) }' > "$OUT/exported-gl.txt"
cp "${MESA_TREE:?Set MESA_TREE}/src/mapi/glapi/registry/gl.xml" "$OUT/gl.xml"
python3 "$COMMON/userland/mkeagl.py" "$COMMON/userland/src/eagl_slots.txt" "$OUT/gl.xml" "$OUT/exported-gl.txt" "$COMMON/userland/src/eagl.c" > "$OUT/eagl_gen.h"
"$CC" -arch arm64 -isysroot "$SDK" -O1 -dynamiclib -install_name /redwood/lib/libhakios.gl.dylib \
 -I"$OUT" -framework CoreFoundation -framework IOKit -framework IOSurface -lobjc \
 "$OUT/libOSMesa.8.dylib" -o "$OUT/libhakios.gl.dylib" \
 "$COMMON/userland/src/eagl.c" "$COMMON/userland/src/gl_osmesa.c"
"$CC" -arch arm64 -isysroot "$SDK" -O1 -framework CoreFoundation -framework IOKit \
 "$OUT/libOSMesa.8.dylib" -o "$OUT/gl_test" "$COMMON/userland/src/gl_test.c"
/usr/bin/file "$OUT/libOSMesa.8.dylib" "$OUT/libhakios.gl.dylib" "$OUT/gl_test"
/usr/bin/otool -L "$OUT/libhakios.gl.dylib" "$OUT/gl_test"
/usr/bin/shasum -a 256 "$OUT/libOSMesa.8.dylib" "$OUT/libhakios.gl.dylib" "$OUT/gl_test"
