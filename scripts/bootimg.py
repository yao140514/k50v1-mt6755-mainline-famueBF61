#!/usr/bin/env python3
"""Inspect / repack a legacy (header v0) Android boot image.

MTK layout used on this device: kernel section = Image.gz + appended DTB.
"""

import argparse
import struct
import sys

FIELDS = [
    ("kernel_size", "<I"),
    ("kernel_addr", "<I"),
    ("ramdisk_size", "<I"),
    ("ramdisk_addr", "<I"),
    ("second_size", "<I"),
    ("second_addr", "<I"),
    ("tags_addr", "<I"),
    ("page_size", "<I"),
]


def read_header(path):
    with open(path, "rb") as f:
        head = f.read(2048)
    if head[:8] != b"ANDROID!":
        sys.exit(f"not an Android boot image: magic={head[:8]!r}")
    hdr = {"magic": head[:8]}
    off = 8
    for name, fmt in FIELDS:
        (val,) = struct.unpack_from(fmt, head, off)
        hdr[name] = val
        off += 4
    hdr["header_version"] = struct.unpack_from("<I", head, 40)[0]
    hdr["os_version"] = struct.unpack_from("<I", head, 44)[0]
    hdr["product"] = head[48:64].rstrip(b"\x00").decode("ascii", "replace")
    hdr["cmdline"] = head[64:576].rstrip(b"\x00").decode("ascii", "replace")
    hdr["id"] = head[576:608]
    hdr["extra_cmdline"] = head[608:1632].rstrip(b"\x00").decode("ascii", "replace")
    return hdr


def cmd_info(args):
    h = read_header(args.image)
    print(f"magic          : {h['magic']!r}")
    for name, _ in FIELDS:
        print(f"{name:14} : {h[name]:>12}  0x{h[name]:x}")
    print(f"header_version : {h['header_version']}")
    print(f"os_version     : {h['os_version']}")
    print(f"product        : {h['product']!r}")
    print(f"cmdline        : {h['cmdline']}")
    print(f"id             : {h['id'].hex()}")
    if h["extra_cmdline"]:
        print(f"extra_cmdline  : {h['extra_cmdline']!r}")


def pad(data, page):
    rem = len(data) % page
    return data if rem == 0 else data + b"\x00" * (page - rem)


def make_dtbo(dtb):
    """Wrap a DTB the way MTK stores it in the boot image DTBO section.

    Layout: 32 byte table header + 32 byte entry + DTB at offset 64.
    LK reads the section size from header word 1648, then takes the main
    device tree out of it and copies it to the address in word 1652.
    """
    size = len(dtb)
    total = 64 + size
    hdr = struct.pack(">8I", 0xD7B7AB1E, total, 32, 32, 1, 32, 2048, 0)
    entry = struct.pack(">4I", size, 64, 0, 0) + b"\x00" * 16
    return hdr + entry + dtb


def cmd_pack(args):
    kernel = open(args.kernel, "rb").read()
    dtb_len = 0
    dtbo = b""
    if args.dtb:
        dtb = open(args.dtb, "rb").read()
        if dtb[:4] != b"\xd0\x0d\xfe\xed":
            sys.exit("dtb does not start with FDT magic")
        kernel += dtb
        dtb_len = len(dtb)
        dtbo = make_dtbo(dtb)

    ramdisk = open(args.ramdisk, "rb").read() if args.ramdisk else b""
    page = args.page_size

    hdr = bytearray(2048)
    hdr[0:8] = b"ANDROID!"
    struct.pack_into("<I", hdr, 8, len(kernel))
    struct.pack_into("<I", hdr, 12, args.kernel_addr)
    struct.pack_into("<I", hdr, 16, len(ramdisk))
    struct.pack_into("<I", hdr, 20, args.ramdisk_addr)
    struct.pack_into("<I", hdr, 24, 0)
    struct.pack_into("<I", hdr, 28, args.second_addr)
    struct.pack_into("<I", hdr, 32, args.tags_addr)
    struct.pack_into("<I", hdr, 36, page)
    struct.pack_into("<I", hdr, 40, args.header_version)
    struct.pack_into("<I", hdr, 44, args.os_version)
    struct.pack_into("<I", hdr, 1632, args.dtb_size)
    struct.pack_into("<Q", hdr, 1636, args.dtb_addr)
    # MTK private extension, right after the v2 header (1644).
    # LK reads these three words to locate and place the appended DTB:
    #   1644  u32  extension size (0x67c = 1660)
    #   1648  u32  dtb size
    #   1652  u32  dtb load address
    if args.mtk_ext or dtbo:
        struct.pack_into("<I", hdr, 1644, 1660)
        struct.pack_into("<I", hdr, 1648, len(dtbo))
        struct.pack_into("<I", hdr, 1652, args.mtk_dtb_addr)
    hdr[48:64] = args.product.encode()[:16].ljust(16, b"\x00")
    hdr[64:576] = args.cmdline.encode()[:512].ljust(512, b"\x00")

    import hashlib
    digest = hashlib.sha1()
    for chunk in (kernel, ramdisk):
        digest.update(chunk)
    digest.update(struct.pack("<I", len(kernel) + len(ramdisk)))
    hdr[576:608] = digest.digest().ljust(32, b"\x00")

    out = bytes(hdr) + pad(kernel, page) + pad(ramdisk, page) + pad(dtbo, page)
    if args.pad_to and len(out) < args.pad_to:
        out += b"\x00" * (args.pad_to - len(out))
    with open(args.output, "wb") as f:
        f.write(out)
    print(f"wrote {args.output}: {len(out)} bytes")
    print(f"  kernel  {len(kernel)} B  (Image.gz {len(kernel) - dtb_len} + dtb {dtb_len})")
    print(f"  ramdisk {len(ramdisk)} B")
    print(f"  hdr ver {args.header_version}, page {page}")
    print(f"  cmdline {args.cmdline}")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("info")
    p.add_argument("image")
    p.set_defaults(func=cmd_info)

    p = sub.add_parser("pack")
    p.add_argument("-o", "--output", required=True)
    p.add_argument("--kernel", required=True, help="Image.gz")
    p.add_argument("--dtb", help="dtb appended after kernel")
    p.add_argument("--ramdisk")
    p.add_argument("--cmdline", default="")
    p.add_argument("--page-size", type=int, default=2048)
    p.add_argument("--kernel-addr", type=lambda x: int(x, 0), default=0x40080000)
    p.add_argument("--ramdisk-addr", type=lambda x: int(x, 0), default=0x45000000)
    p.add_argument("--second-addr", type=lambda x: int(x, 0), default=0x40F00000)
    p.add_argument("--tags-addr", type=lambda x: int(x, 0), default=0x4E000000)
    p.add_argument("--os-version", type=lambda x: int(x, 0), default=0)
    p.add_argument("--header-version", type=int, default=0)
    p.add_argument("--dtb-size", type=lambda x: int(x, 0), default=0,
                   help="value for the v2 dtb_size field; LK uses it to locate the appended DTB")
    p.add_argument("--dtb-addr", type=lambda x: int(x, 0), default=0)
    p.add_argument("--mtk-ext", action="store_true",
                   help="always write the MTK private extension header")
    p.add_argument("--mtk-dtb-addr", type=lambda x: int(x, 0), default=0x44000000,
                   help="address LK should copy the DTB to (MTK ext field at 1652)")
    p.add_argument("--pad-to", type=lambda x: int(x, 0), default=0,
                   help="pad output to this many bytes (e.g. 0x1000000 for a 16 MiB partition)")
    p.add_argument("--product", default="")
    p.set_defaults(func=cmd_pack)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
