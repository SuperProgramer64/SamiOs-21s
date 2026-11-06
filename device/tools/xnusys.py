#!/usr/bin/env python3
"""
xnusys on the A21s: the iOS system image in the unallocated tail of `super`, without touching
`userdata`.

  xnusys.py plan  [--serial S]                       read only: where the region is, and why
  xnusys.py check IMAGE [--serial S]                 read only: would IMAGE fit
  xnusys.py write IMAGE --serial S --i-understand    push IMAGE and dd it into the region
  xnusys.py verify IMAGE --serial S                  read the region back and compare hashes
  xnusys.py payload PAYLOAD --serial S --i-understand the XNU payload (xnuloader/mkpayload.py) into the
                                                     region's last 64 MiB, where exyloader reads it
  xnusys.py var-plan --serial S                     plan a journaled HFS+ data window after APFS
  xnusys.py var-write IMAGE --serial S --i-understand initialize that window, refusing an existing HFS volume
  xnusys.py var-verify IMAGE --serial S              compare the data window with IMAGE (before use)

Why there: the A32 carves a 4 GiB `xnusys` from the end of `userdata`, which means formatting
it. On this phone the dynamic partitions (odm, product, system, vendor) use 5.4 GB of the
9.3 GiB `super`, all at its start. The rest of `super` is free and contiguous:

  - from the last allocated extent to the end of the space liblp manages (the "block device
    size" in super's metadata): liblp could allocate here, but only when partitions are
    created or resized, i.e. when a ROM is flashed;
  - from there to the end of the `super` partition: liblp never uses it.

The region starts 1 MiB-aligned after the last allocated extent (plus a 64 MiB guard) and ends
at the end of `super`. Every run re-reads super's metadata on the phone: if an extent has
grown into the region (a new ROM, a resized partition), it refuses. Writes go to
/dev/block/by-name/super with seek, so they cannot leave that partition.

The region's last 64 MiB are the XNU payload's (kernel + RAM disk, read by exyloader step 2 at
xnusys_lba + xnusys_sectors - 131072); the system image gets the rest. Payload files
may use at most 60 MiB; the last 4 MiB are reserved for HAKLOG01 console records.

What the loader needs is printed by `plan`: the region's first sector on mmcblk0 and its
length, for the boot arguments (xnusys_lba=, xnusys_sectors=).

**Flashing another ROM (or resizing dynamic partitions) may hand this space back to Android
and overwrite the iOS image; the next `plan`/`verify` says so. It never harms Android.**
"""
import argparse
import hashlib
import os
import struct
import subprocess
import sys

SECTOR = 512
ALIGN = 2048                 # 1 MiB in sectors
GUARD = 64 * 2048            # 64 MiB left free after the last allocated extent
PAYLOAD = 64 * 2048          # the region's last 64 MiB: the XNU payload (exyloader's PAYLOAD_SECTORS)
LOG_SECTORS = 4 * 2048      # final 4 MiB of payload reservation: checksummed console records

def payload_room():
    return PAYLOAD - LOG_SECTORS

LP_GEOMETRY_MAGIC = 0x616c4467
LP_HEADER_MAGIC = 0x414c5030


def adb(serial, *args, binary=False, check=True):
    cmd = ["adb"] + (["-s", serial] if serial else []) + list(args)
    r = subprocess.run(cmd, capture_output=True, check=False)
    if check and r.returncode != 0:
        raise SystemExit(f"adb {' '.join(args)}: {r.stderr.decode(errors='replace').strip()}")
    return r.stdout if binary else r.stdout.decode(errors="replace").strip()


def sh(serial, cmd, binary=False):
    state = adb(serial, "get-state", check=False)
    if state == "recovery":
        return adb(serial, "exec-out", cmd, binary=binary)
    return adb(serial, "exec-out", f"su -c '{cmd}'", binary=binary)


def super_layout(serial):
    """(super start on mmcblk0, super sectors, liblp device sectors, allocated extents)"""
    start = int(sh(serial, "cat /sys/class/block/$(basename $(readlink -f /dev/block/by-name/super))/start"))
    size = int(sh(serial, "cat /sys/class/block/$(basename $(readlink -f /dev/block/by-name/super))/size"))
    head = sh(serial, "dd if=/dev/block/by-name/super bs=4096 count=32 2>/dev/null", binary=True)
    g = head[4096:8192]
    magic, = struct.unpack_from("<I", g, 0)
    if magic != LP_GEOMETRY_MAGIC:
        raise SystemExit("super: no liblp geometry; not a dynamic-partition super?")
    h = head[4096 * 3:]
    hm, major, minor, hsz = struct.unpack_from("<IHHI", h, 0)
    if hm != LP_HEADER_MAGIC:
        raise SystemExit("super: no liblp metadata header")
    tables_size, = struct.unpack_from("<I", h, 44)
    t = h[hsz:hsz + tables_size]
    p_off, p_n, p_es = struct.unpack_from("<III", h, 80)
    e_off, e_n, e_es = struct.unpack_from("<III", h, 92)
    b_off, b_n, b_es = struct.unpack_from("<III", h, 116)
    if b_n != 1:
        raise SystemExit(f"super: {b_n} block devices in the metadata; expected 1")
    _, _, _, lp_bytes = struct.unpack_from("<QIIQ", t, b_off)
    exts = [struct.unpack_from("<QIQI", t, e_off + i * e_es) for i in range(e_n)]
    used = []
    for i in range(p_n):
        o = p_off + i * p_es
        name = t[o:o + 36].split(b"\0")[0].decode()
        _, first, count, _ = struct.unpack_from("<IIII", t, o + 36)
        for ns, ttype, tdata, _ in exts[first:first + count]:
            if ttype == 0:   # LP_TARGET_TYPE_LINEAR
                used.append((tdata, tdata + ns, name))
    return start, size, lp_bytes // SECTOR, sorted(used), (major, minor)


def region(serial):
    start, size, lp_sectors, used, ver = super_layout(serial)
    last = max(b for _, b, _ in used)
    first = (last + GUARD + ALIGN - 1) // ALIGN * ALIGN
    if first >= size:
        raise SystemExit("super: no free tail left")
    return dict(super_start=start, super_size=size, lp_sectors=lp_sectors, used=used, ver=ver,
                last=last, first=first, sectors=size - first)


def show(r):
    gib = lambda s: s * SECTOR / 2 ** 30
    print(f"super: mmcblk0 sectors {r['super_start']}..{r['super_start'] + r['super_size']} "
          f"({gib(r['super_size']):.2f} GiB), liblp v{r['ver'][0]}.{r['ver'][1]} manages the first "
          f"{gib(r['lp_sectors']):.2f} GiB")
    for a, b, n in r["used"]:
        print(f"  {n:10s} super sectors {a:>9}..{b:>9}  {gib(b - a) * 1024:8.1f} MiB")
    print(f"last allocated sector in super: {r['last']} (+{GUARD // 2048} MiB guard, 1 MiB aligned)")
    inside = max(0, r["lp_sectors"] - r["first"])
    print(f"xnusys region: super sectors {r['first']}..{r['super_size']}, {r['sectors']} sectors = "
          f"{gib(r['sectors']):.3f} GiB ({gib(inside):.2f} GiB inside liblp's space, "
          f"{gib(r['sectors'] - inside):.2f} GiB beyond it)")
    print(f"for the loader: xnusys_lba={r['super_start'] + r['first']} xnusys_sectors={r['sectors']}")
    print(f"system image: at most {gib(r['sectors'] - PAYLOAD):.3f} GiB; payload: the last 64 MiB, "
          f"mmcblk0 sector {r['super_start'] + r['first'] + r['sectors'] - PAYLOAD}")


def image_info(path, r, room):
    n = os.path.getsize(path)
    if n % SECTOR:
        raise SystemExit(f"{path}: {n} bytes, not a multiple of {SECTOR}")
    if n > room * SECTOR:
        raise SystemExit(f"{path}: {n / 2 ** 30:.3f} GiB, more than the "
                         f"{room * SECTOR / 2 ** 30:.3f} GiB it may have")
    sha = hashlib.sha256()
    with open(path, "rb") as f:
        for blk in iter(lambda: f.read(1 << 22), b""):
            sha.update(blk)
    return n, sha.hexdigest()


def var_layout(r, header):
    """A disjoint, MiB-aligned HFS+ window after APFS, before the payload.

    Leave a full MiB guard after the rounded-up APFS container. All addresses
    returned are absolute mmcblk0 sectors; never infer free space from file size.
    """
    if len(header) < 48 or header[32:36] != b"NXSB":
        raise ValueError("var: no APFS container at the root window")
    block, count = struct.unpack_from("<IQ", header, 36)
    if block not in (4096, 8192, 16384, 32768, 65536) or count < 2:
        raise ValueError("var: invalid APFS geometry")
    root = block * count // SECTOR
    if root > r["sectors"] - PAYLOAD:
        raise ValueError("var: APFS leaves no room before the payload")
    base = r["super_start"] + r["first"]
    start = (base + root + ALIGN - 1) // ALIGN * ALIGN + ALIGN
    end = base + r["sectors"] - PAYLOAD
    size = (end - start) // ALIGN * ALIGN
    if size < 64 * ALIGN or end > 2**32:
        raise ValueError("var: no room for a 64 MiB data volume")
    return dict(lba=start, sectors=size, root_sectors=root, payload_lba=end)


def hfs_image_check(path, sectors):
    """Require a complete, matching journaled HFS+ volume, not a DMG wrapper."""
    if os.path.getsize(path) != sectors * SECTOR:
        raise ValueError("var: HFS image size must match the planned data window")
    with open(path, "rb") as f:
        f.seek(1024)
        h = f.read(SECTOR)
    if h[:2] != b"H+":
        raise ValueError("var: expected a raw HFS+ image")
    attrs, = struct.unpack_from(">I", h, 4)
    journal, = struct.unpack_from(">I", h, 12)
    block, count = struct.unpack_from(">II", h, 40)
    if not attrs & (1 << 13) or journal == 0:
        raise ValueError("var: HFS+ must have a journal")
    if block < SECTOR or block & (block - 1) or count == 0 or block * count != sectors * SECTOR:
        raise ValueError("var: HFS volume geometry differs from image size")


def phone_root_header(serial, r):
    return sh(serial, f"dd if=/dev/block/by-name/super bs=512 skip={r['first']} count=8 2>/dev/null", binary=True)


def phone_var_exists(serial, r, v):
    relative = v['lba'] - r['super_start']
    # A damaged primary header is not permission to erase recoverable data.
    for offset in (relative + 2, relative + v['sectors'] - 2):
        h = sh(serial, f"dd if=/dev/block/by-name/super bs=512 skip={offset} count=1 2>/dev/null", binary=True)
        if len(h) != SECTOR:
            raise SystemExit("var: could not read a full volume header; nothing written")
        if h[:2] in (b"H+", b"HX"):
            return True
    return False


def protect_root_update(serial, r, new_bytes):
    """Changing APFS geometry would change var's derived location: require migration instead."""
    try:
        v = var_layout(r, phone_root_header(serial, r))
    except ValueError:
        return  # initial installation, without a valid previous APFS container
    if phone_var_exists(serial, r, v) and new_bytes != v['root_sectors'] * SECTOR:
        raise SystemExit("write: persistent var exists; changing root size requires an explicit data migration")


def var_command(a, r):
    if not a.serial:
        raise SystemExit("var: pass --serial")
    try:
        v = var_layout(r, phone_root_header(a.serial, r))
    except ValueError as e:
        raise SystemExit(str(e))
    print(f"var: mmcblk0 {v['lba']} + {v['sectors']} sectors ({v['sectors'] * SECTOR / 2**30:.3f} GiB)")
    print(f"boot args: emmc_multi=1 emmc_write=1 emmc_var_lba={v['lba']} "
          f"emmc_var_sectors={v['sectors']} var_hfs=emmc1")
    if a.cmd == "var-plan":
        return
    if not a.image:
        raise SystemExit(f"{a.cmd}: needs a raw journaled HFS+ IMAGE")
    try:
        hfs_image_check(a.image, v['sectors'])
    except ValueError as e:
        raise SystemExit(str(e))
    n, digest = image_info(a.image, r, v['sectors'])
    relative = v['lba'] - r['super_start']
    seek_mib = relative // ALIGN
    if a.cmd == "var-write":
        if not a.i_understand:
            raise SystemExit("var-write: add --i-understand")
        # This initializes only. Once data exists, never reformat it implicitly.
        if phone_var_exists(a.serial, r, v):
            raise SystemExit("var-write: an HFS data volume already exists; refusing to erase it")
        remote = "/data/local/tmp/hakios-var.hfs"
        adb(a.serial, "push", a.image, remote)
        # Recheck live extents and root immediately before the first flash write.
        current = region(a.serial)
        if current != r or var_layout(current, phone_root_header(a.serial, current)) != v:
            raise SystemExit("var-write: storage layout changed; nothing written")
        sh(a.serial, f"dd if={remote} of=/dev/block/by-name/super bs=1048576 seek={seek_mib} conv=notrunc,fsync")
        sh(a.serial, "sync")
        sh(a.serial, f"rm {remote}")
    got = sh(a.serial, f"dd if=/dev/block/by-name/super bs=1048576 skip={seek_mib} count={n // 1048576} "
                      "2>/dev/null | sha256sum").split()[0]
    if got != digest:
        raise SystemExit(f"var: MISMATCH expected {digest}, got {got}")
    print(f"var: verified {n} bytes, sha256 {digest}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["plan", "check", "write", "verify", "payload", "var-plan", "var-write", "var-verify"])
    ap.add_argument("image", nargs="?")
    ap.add_argument("--serial", default=os.environ.get("HAKIOS_SERIAL"))
    ap.add_argument("--i-understand", action="store_true",
                    help="write: you have read the warning in this file's header")
    a = ap.parse_args()

    r = region(a.serial)
    show(r)
    if a.cmd.startswith("var-"):
        return var_command(a, r)
    if a.cmd == "plan":
        return
    if not a.image:
        raise SystemExit(f"{a.cmd}: needs IMAGE")
    if a.cmd == "payload":
        # a payload is padded to whole sectors here; the loader reads only what its header says
        data = open(a.image, "rb").read()
        if data[:4] != b"XNUP":
            raise SystemExit(f"{a.image}: not an XNUP payload (xnuloader/mkpayload.py)")
        padded = a.image + ".sectors"
        open(padded, "wb").write(data + b"\0" * (-len(data) % SECTOR))
        a.image = padded
    room = payload_room() if a.cmd == "payload" else r["sectors"] - PAYLOAD
    n, digest = image_info(a.image, r, room)
    print(f"image: {a.image}, {n} bytes ({n / 2 ** 30:.3f} GiB), sha256 {digest}: fits")
    if a.cmd == "check":
        return

    if not a.serial:
        raise SystemExit(f"{a.cmd}: pass --serial (a second phone may be connected)")
    seek_mib = (r["first"] + (r["sectors"] - PAYLOAD if a.cmd == "payload" else 0)) // ALIGN
    count_bytes = n
    if a.cmd in ("write", "payload"):
        if not a.i_understand:
            raise SystemExit("write: add --i-understand (see the warning at the top of this file)")
        if a.cmd == "write":
            protect_root_update(a.serial, r, n)
        remote = "/sdcard/hakios/" + ("payload.img" if a.cmd == "payload" else "xnusys.img")
        print(f"pushing to {remote} ...")
        adb(a.serial, "shell", "mkdir -p /sdcard/hakios")
        adb(a.serial, "push", a.image, remote)
        print(f"writing super at MiB {seek_mib} (mmcblk0 sector {r['super_start'] + seek_mib * ALIGN}) ...")
        sh(a.serial, f"dd if={remote} of=/dev/block/by-name/super bs=1048576 seek={seek_mib} conv=notrunc,fsync")
        sh(a.serial, "sync")
        sh(a.serial, f"rm {remote}")
    # verify (also after write)
    got = sh(a.serial, f"dd if=/dev/block/by-name/super bs=1048576 skip={seek_mib} count={(count_bytes + 1048575) // 1048576} "
                       f"2>/dev/null | head -c {count_bytes} | sha256sum").split()[0]
    print(f"region sha256 {got}")
    if got != digest:
        raise SystemExit("MISMATCH: the region does not hold this image")
    print("region holds the image")


if __name__ == "__main__":
    main()
