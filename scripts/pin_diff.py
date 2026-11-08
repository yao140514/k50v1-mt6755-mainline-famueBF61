#!/usr/bin/env python3
"""Diff Android's runtime GPIO state (mtgpio pin.txt) against the DCT boot
defaults baked into LK (cust_gpio_boot.h).

Android's pin.txt line format is:  <pin>:<MODE><PULL_SEL><DIN><DOUT><PULL_EN><DIR><IES><SMT>
LK's DCT header gives MODE/DIR/PULLEN/DATAOUT per pin, which is the state the
bootloader leaves behind -- i.e. what mainline inherits.
"""
import re
import sys

PIN_TXT = "/mnt/f/BF61/pin.txt"
DCT_HDR = "/home/yao14/mt6755/lk/build-k50v1_64_bsp/inc/cust_gpio_boot.h"

andro = {}
for m in re.finditer(r"^\s*(\d+):([01]{8})\s*$", open(PIN_TXT).read(), re.M):
    p, v = int(m.group(1)), m.group(2)
    andro[p] = dict(mode=int(v[0]), ps=int(v[1]), din=int(v[2]), dout=int(v[3]),
                    pe=int(v[4]), dr=int(v[5]), ies=int(v[6]), smt=int(v[7]))

dct = open(DCT_HDR).read()
mode = {int(m.group(1)): int(m.group(2))
        for m in re.finditer(r"#define GPIO(\d+)_MODE\s+GPIO_MODE_0(\d)", dct)}
dirn = {int(m.group(1)): m.group(2)
        for m in re.finditer(r"#define GPIO(\d+)_DIR\s+GPIO_DIR_(\w+)", dct)}
dout = {int(m.group(1)): m.group(2)
        for m in re.finditer(r"#define GPIO(\d+)_DATAOUT\s+GPIO_OUT_(\w+)", dct)}
pul = {int(m.group(1)): m.group(2)
       for m in re.finditer(r"#define GPIO(\d+)_PULLEN\s+GPIO_PULL_(\w+)", dct)}

print("android pins: %d   dct pins: %d" % (len(andro), len(mode)))

rows = []
for p in sorted(andro):
    a = andro[p]
    d = []
    lm = mode.get(p)
    if lm is not None and a["mode"] != lm:
        d.append("MODE %d->%d" % (lm, a["mode"]))
    lk_dir = 1 if dirn.get(p) == "OUT" else 0
    if a["dr"] != lk_dir:
        d.append("DIR %d->%d" % (lk_dir, a["dr"]))
    lk_pe = 1 if pul.get(p) == "ENABLE" else 0
    if a["pe"] != lk_pe:
        d.append("PULLEN %d->%d" % (lk_pe, a["pe"]))
    lk_out = 1 if dout.get(p) == "ONE" else 0
    if a["dr"] == 1 and a["dout"] != lk_out:
        d.append("DOUT %d->%d" % (lk_out, a["dout"]))
    if d:
        rows.append((p, a, d))

print("\n== pins whose runtime state differs from the boot default ==")
for p, a, d in rows:
    print("pin%-3d %-26s  A[mode=%d dir=%d dout=%d pe=%d din=%d]"
          % (p, " | ".join(d), a["mode"], a["dr"], a["dout"], a["pe"], a["din"]))
print("total: %d" % len(rows))

print("\n== every pin Android drives as a plain GPIO output ==")
for p in sorted(andro):
    a = andro[p]
    if a["mode"] == 0 and a["dr"] == 1:
        print("pin%-3d out=%d din=%d pe=%d   (boot: mode=%s dir=%s out=%s)"
              % (p, a["dout"], a["din"], a["pe"],
                 mode.get(p), dirn.get(p), dout.get(p)))
