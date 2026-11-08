#!/usr/bin/env python3
"""Grow a DTB's totalsize and append zero padding.

LK's boot path edits the device tree in place with fdt_add_subnode(), which
refuses to write past fdt_totalsize(). A dtc-built blob has
totalsize == file size, so there is no room at all and LK would fail with
-FDT_ERR_NOSPACE when it tries to append /chosen/framebuffer@...

dtc's own -p option pads the blob but also folds the padding into
totalsize in a way that depends on the dtc version, so do it explicitly here:
bump totalsize and append the bytes, then LK has somewhere to write.
"""

import argparse
import struct
import sys

FDT_MAGIC = 0xD00DFEED


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dtb")
    ap.add_argument("-n", "--bytes", type=lambda x: int(x, 0), default=0x10000,
                    help="padding to append (default 64 KiB)")
    ap.add_argument("-o", "--output", help="write here instead of in place")
    args = ap.parse_args()

    data = bytearray(open(args.dtb, "rb").read())
    magic, total = struct.unpack(">2I", data[:8])
    if magic != FDT_MAGIC:
        sys.exit(f"{args.dtb}: not an FDT blob")

    if total != len(data):
        print(f"note: totalsize {total} != file size {len(data)}")

    struct.pack_into(">I", data, 4, total + args.bytes)
    data += b"\x00" * args.bytes

    out = args.output or args.dtb
    with open(out, "wb") as f:
        f.write(data)

    print(f"{out}: totalsize {total} -> {total + args.bytes}, "
          f"file {len(data)} bytes (+{args.bytes:#x})")


if __name__ == "__main__":
    main()
