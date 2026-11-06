#!/bin/bash
# Build exyloader.img: exyloader packed in front of Android's kernel in the A21s's boot image.
#
#   ANDROID_BOOT=...  your saved stock boot.img to chain to (no default)
#   LLD=...           an ELF ld.lld (default: `ld.lld`, else `zig ld.lld`)
#   STEP=N            how far the loader goes (see exyloader.c; default 1)
#
# Output (git ignores all of it): exyloader.bin and exyloader.img (boot v2, 36 MiB, raw Image:
# S-Boot does not inflate gzip). Shares diag/'s entry (start.S), link script and debug output.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
DIAG="$ROOT/diag"
STEP="${STEP:-1}"
CFLAGS="${EXTRA_CFLAGS:-} --target=aarch64-linux-gnu -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
	-fno-pic -fno-pie -mcmodel=small -mgeneral-regs-only -mstrict-align -Os -Wall -Wextra \
	-I$HERE -I$DIAG -DSTEP=$STEP -DXNU_FIRST=${XNU_FIRST:-0}"

[ -f "$DIAG/font8x16.h" ] || swift "$ROOT/tools/mkfont8x16.swift" > "$DIAG/font8x16.h"

clang $CFLAGS -c "$HERE/start.S" -o "$HERE/start.o"
clang $CFLAGS -c "$HERE/exyloader.c" -o "$HERE/exyloader.o"

bad=$(objdump -r "$HERE/start.o" "$HERE/exyloader.o" | grep -E 'R_AARCH64_(ABS|MOVW_UABS|PREL64)' || true)
if [ -n "$bad" ]; then
	echo "exyloader: absolute relocations, the program would not be position independent:" >&2
	echo "$bad" >&2
	exit 1
fi
echo "exyloader: compiled (step $STEP), no absolute relocations"

if [ -z "$LLD" ]; then
	if command -v ld.lld >/dev/null; then LLD=ld.lld
	elif command -v zig >/dev/null; then LLD="zig ld.lld"
	elif [ -x "$HOME/.local/zig/zig" ]; then LLD="$HOME/.local/zig/zig ld.lld"
	else
		echo "exyloader: no ld.lld (zig's: ~/.local/zig/zig ld.lld); stopping after compile" >&2
		exit 2
	fi
fi
$LLD -T "$DIAG/link.ld" --oformat binary "$HERE/start.o" "$HERE/exyloader.o" -o "$HERE/exyloader.bin"
rm -f "$HERE/start.o" "$HERE/exyloader.o"
ls -l "$HERE/exyloader.bin"

BOOT="${ANDROID_BOOT:?build.sh: pass ANDROID_BOOT=/path/to/your/stock/boot.img}"
[ -f "$BOOT" ] || { echo "build.sh: no boot image at $BOOT" >&2; exit 1; }
python3 "$ROOT/tools/chainboot.py" "$BOOT" "$HERE/exyloader.bin" "$HERE/exyloader.img"
shasum -a 256 "$HERE/exyloader.img"
