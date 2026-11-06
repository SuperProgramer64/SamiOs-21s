#!/usr/bin/env python3
"""
xnuloader/identity.h from YOUR OWN IPSW's DeviceTree (im4p): the root, /product and
/product/haptics properties the loader copies into XNU's device tree. The output is Apple's
data: it is generated on your machine and never committed (.gitignore).

  unzip -j your.ipsw Firmware/all_flash/DeviceTree.d22ap.im4p -d /tmp/dt
  python3 -I tools/mkidentity.py /tmp/dt/DeviceTree.d22ap.im4p xnuloader/identity.h
"""
import os
import struct
import subprocess
import sys
import tempfile

# what the loader writes itself (or dt_begin does)
SKIP_ROOT = {"name", "platform-name", "serial-number", "mlb-serial-number", "model-number",
             "region-info", "regulatory-model-number", "#address-cells", "#size-cells"}


def der(buf, i):
    tag = buf[i]
    i += 1
    n = buf[i]
    i += 1
    if n & 0x80:
        k = n & 0x7f
        n = int.from_bytes(buf[i:i + k], "big")
        i += k
    return tag, i, n


def im4p_payload(raw):
    tag, i, n = der(raw, 0)
    assert tag == 0x30, "not DER"
    items = []
    end = i + n
    while i < end and len(items) < 4:
        _, j, m = der(raw, i)
        items.append(raw[j:j + m])
        i = j + m
    assert items[0] == b"IM4P", "not an IM4P"
    data = items[3]
    if data[:4] in (b"bvx2", b"bvxn"):
        with tempfile.TemporaryDirectory() as d:
            a, b = os.path.join(d, "in"), os.path.join(d, "out")
            open(a, "wb").write(data)
            subprocess.run(["compression_tool", "-decode", "-a", "lzfse", "-i", a, "-o", b], check=True)
            data = open(b, "rb").read()
    assert data[:8] != b"complzss", "LZSS payload: not handled"
    return data


def node(buf, i):
    np_, nc = struct.unpack_from("<II", buf, i)
    i += 8
    props = []
    for _ in range(np_):
        name = buf[i:i + 32].split(b"\0")[0].decode()
        i += 32
        ln, = struct.unpack_from("<I", buf, i)
        i += 4
        ln &= 0x7fffffff
        props.append((name, buf[i:i + ln]))
        i += (ln + 3) & ~3
    kids = []
    for _ in range(nc):
        k, i = node(buf, i)
        kids.append(k)
    return {"props": props, "kids": kids}, i


def child(n, name):
    for k in n["kids"]:
        if dict(k["props"]).get("name", b"").rstrip(b"\0").decode() == name:
            return k
    sys.exit(f"no /{name} node")


def table(out, sym, props):
    data = bytearray()
    rows = []
    for name, val in props:
        assert len(name) < 32, name
        rows.append((name, len(data), len(val)))
        data += val
    out.append(f"static const struct identity_prop {sym}[] = {{")
    for name, off, ln in rows:
        out.append(f'\t{{ "{name}", {off}u, {ln}u }},')
    out.append("};")
    out.append(f"static const unsigned char {sym}_data[{max(len(data), 1)}] = {{")
    for k in range(0, len(data), 16):
        out.append("\t" + " ".join(f"0x{b:02x}," for b in data[k:k + 16]))
    out.append("};\n")


def main():
    raw = open(sys.argv[1], "rb").read()
    root, _ = node(im4p_payload(raw), 0)
    product = child(root, "product")
    haptics = child(product, "haptics")
    out = ["/* Generated from your own IPSW's DeviceTree by tools/mkidentity.py. Apple's data: never commit. */",
           "#ifndef XNULOADER_IDENTITY_H", "#define XNULOADER_IDENTITY_H\n", "#include <stdint.h>\n",
           "struct identity_prop {", "\tchar name[32];          /* a char array, not a pointer: exyloader is position independent */", "\tuint32_t offset;", "\tuint32_t length;", "};\n"]
    table(out, "identity_root", [p for p in root["props"] if p[0] not in SKIP_ROOT])
    table(out, "identity_product", [p for p in product["props"] if p[0] != "name"])
    table(out, "identity_haptics", [p for p in haptics["props"] if p[0] != "name"])
    out.append("#endif")
    open(sys.argv[2], "w").write("\n".join(out) + "\n")
    for n, src in (("root", root), ("product", product), ("haptics", haptics)):
        print(n, len(src["props"]), "properties")


if __name__ == "__main__":
    main()
