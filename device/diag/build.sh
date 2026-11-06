#!/bin/bash
# Build a21sdiag.img: a21sdiag packed in front of Android's kernel in the A21s's boot image.
#
#   ANDROID_BOOT=...  your saved stock boot.img to chain to (no default)
#   LLD=...           an ELF ld.lld (default: `ld.lld`, else `zig ld.lld`)
#   GZIP=1            pack the kernel gzip-compressed (second test: does S-Boot inflate it?)
#
# Output (git ignores all of it): a21sdiag.bin (bare-metal) and a21sdiag.img (boot v2, 36 MiB).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
OUT="${OUT:-$HERE}"
CFLAGS="--target=aarch64-linux-gnu -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
	-fno-pic -fno-pie -mcmodel=small -mgeneral-regs-only -mstrict-align -Os -Wall -Wextra -I$HERE"

[ -f "$HERE/font8x16.h" ] || swift "$ROOT/tools/mkfont8x16.swift" > "$HERE/font8x16.h"

clang $CFLAGS -c "$HERE/start.S" -o "$OUT/start.o"
clang $CFLAGS -c "$HERE/a21sdiag.c" -o "$OUT/a21sdiag.o"

# Position independence: only PC-relative relocations may remain (adrp/add/ldr lo12, branches).
# An absolute one (R_AARCH64_ABS64/ABS32, MOVW_UABS) would break wherever S-Boot loads us.
bad=$(objdump -r "$OUT/start.o" "$OUT/a21sdiag.o" | grep -E 'R_AARCH64_(ABS|MOVW_UABS|PREL64)' || true)
if [ -n "$bad" ]; then
	echo "a21sdiag: absolute relocations, the program would not be position independent:" >&2
	echo "$bad" >&2
	exit 1
fi
echo "a21sdiag: compiled, no absolute relocations"

if [ -z "$LLD" ]; then
	if command -v ld.lld >/dev/null; then LLD=ld.lld
	elif command -v zig >/dev/null; then LLD="zig ld.lld"
	else
		echo "a21sdiag: no ld.lld (brew install lld, or zig); stopping after compile" >&2
		exit 2
	fi
fi
$LLD -T "$HERE/link.ld" --oformat binary "$OUT/start.o" "$OUT/a21sdiag.o" -o "$OUT/a21sdiag.bin"
ls -l "$OUT/a21sdiag.bin"

BOOT="${ANDROID_BOOT:?build.sh: pass ANDROID_BOOT=/path/to/your/stock/boot.img}"
[ -f "$BOOT" ] || { echo "build.sh: no boot image at $BOOT" >&2; exit 1; }
python3 "$ROOT/tools/chainboot.py" ${GZIP:+--gzip} "$BOOT" "$OUT/a21sdiag.bin" "$OUT/a21sdiag.img"
shasum -a 256 "$OUT/a21sdiag.img"
