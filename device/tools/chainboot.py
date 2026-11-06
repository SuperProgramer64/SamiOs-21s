#!/usr/bin/env python3
"""
Put a bare-metal program in front of Android's kernel in the A21s's boot image (header v2).

  chainboot.py [--gzip] <android-boot.img> <program.bin> <out.img> [payload.bin]

The A21s (SM-A217F/M, Exynos 850) is started by Samsung's S-Boot, not MediaTek's LK. Its stock
boot image carries an *uncompressed* arm64 Image (text_offset 0x80000), so by default the new
kernel is also uncompressed: program.bin, padded to CHAIN_OFFSET, then Android's own Image, then
an optional payload, 16-aligned. `--gzip` packs the same blob gzip-compressed instead, but
S-Boot does NOT inflate it (tested: a boot loop, no Linux boot in last_kmsg); it is kept only to
repeat that test. XNU and its payload are read off the eMMC instead (tools/xnusys.py).

The program's arm64 header gets the Image's length in res2 (0x28), the payload's offset in res3
(0x30) and a covering image_size (0x10), as on the A32. Header fields, cmdline, ramdisk and dtb
are copied from the Android image; the SHA-1 id is recomputed.
"""
import gzip
import hashlib
import struct
import sys

CHAIN_OFFSET = 0x40000
BOOT_PARTITION = 36 << 20          # /dev/block/by-name/boot: 37748736 bytes


def pad(b, n):
    return b + b'\0' * (-len(b) % n)


def main():
    args = sys.argv[1:]
    use_gzip = '--gzip' in args
    args = [a for a in args if a != '--gzip']
    if len(args) not in (3, 4):
        raise SystemExit(__doc__)
    src, prog_path, out = args[:3]
    payload = open(args[3], 'rb').read() if len(args) > 3 else b''

    d = open(src, 'rb').read()
    assert d[:8] == b'ANDROID!', 'not an Android boot image'
    ksize, _, rsize, _, ssize, _, _, page, hver = struct.unpack_from('<9I', d, 8)
    assert hver == 2, f'header v{hver}, expected 2'
    rdo_size = struct.unpack_from('<I', d, 1632)[0]
    dtb_size = struct.unpack_from('<I', d, 1648)[0]

    def pages(n):
        return (n + page - 1) // page * page

    off = page
    kernel = d[off:off + ksize]; off += pages(ksize)
    ramdisk = d[off:off + rsize]; off += pages(rsize)
    second = d[off:off + ssize]; off += pages(ssize)
    rdo = d[off:off + rdo_size]; off += pages(rdo_size)
    dtb = d[off:off + dtb_size]

    image = gzip.decompress(kernel) if kernel[:2] == b'\x1f\x8b' else kernel
    assert image[0x38:0x3c] == b'ARM\x64', 'Android kernel is not an arm64 Image'
    assert image[0x00:0x04] != b'ANDR', 'nested boot image?'
    linux_text_offset, linux_image_size = struct.unpack_from('<QQ', image, 0x08)

    prog = bytearray(open(prog_path, 'rb').read())
    assert prog[0x38:0x3c] == b'ARM\x64', 'program has no arm64 header'
    assert len(prog) <= CHAIN_OFFSET, f'program too large ({len(prog)} > {CHAIN_OFFSET})'
    prog_text_offset = struct.unpack_from('<Q', prog, 0x08)[0]
    assert prog_text_offset == linux_text_offset, \
        f'program text_offset {prog_text_offset:#x} != Android\'s {linux_text_offset:#x}'

    payload_off = CHAIN_OFFSET + len(pad(image, 16)) if payload else 0
    total = max(CHAIN_OFFSET + linux_image_size, payload_off + len(payload))
    struct.pack_into('<Q', prog, 0x10, (total + 0xfffff) & ~0xfffff)
    struct.pack_into('<Q', prog, 0x28, len(image))
    struct.pack_into('<Q', prog, 0x30, payload_off)
    blob = bytes(prog) + b'\0' * (CHAIN_OFFSET - len(prog)) + image
    if payload:
        blob = pad(blob, 16) + payload
    new_kernel = gzip.compress(blob, 9, mtime=0) if use_gzip else blob

    hdr = bytearray(d[:page])
    struct.pack_into('<I', hdr, 8, len(new_kernel))
    sha = hashlib.sha1()
    for part in (new_kernel, ramdisk, second, rdo, dtb):
        sha.update(part)
        sha.update(struct.pack('<I', len(part)))
    hdr[576:608] = pad(sha.digest(), 32)

    img = bytes(hdr) + pad(new_kernel, page) + pad(ramdisk, page) + pad(second, page) + pad(rdo, page) + pad(dtb, page)
    if len(img) > BOOT_PARTITION:
        raise SystemExit(f'{out}: {len(img)} bytes, more than the {BOOT_PARTITION} of the boot partition '
                         f'({len(img) - BOOT_PARTITION} too many): try --gzip or a smaller payload')
    img += b'\0' * (BOOT_PARTITION - len(img))
    open(out, 'wb').write(img)
    print(f'{out}: {len(img)} bytes (program {len(prog)}, Image {len(image)}, payload {len(payload)}, '
          f'kernel {"gz " if use_gzip else ""}{len(new_kernel)}, free {BOOT_PARTITION - len(img.rstrip(bytes(1)))})')


if __name__ == '__main__':
    main()
