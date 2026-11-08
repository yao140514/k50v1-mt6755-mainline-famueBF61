#!/usr/bin/env python3
"""Bring-up tweak for mtu3 on MT6750.

The vendor SSUSB IP needs much longer than the mainline defaults before its
SYS125 / REF / PLL status bits settle after DEV_PDN is cleared, so widen the
polling windows and print what was actually read.
"""
import sys

P = "/home/yao14/mt6755/linux-6.12.115/drivers/usb/mtu3/mtu3_plat.c"

s = open(P).read()
orig = s

s = s.replace(
    "(check_val == (value & check_val)), 100, 20000);",
    "(check_val == (value & check_val)), 100, 200000);", 1)

s = s.replace(
    "(value & SSUSB_U2_MAC_SYS_RST_B_STS), 100, 10000);",
    "(value & SSUSB_U2_MAC_SYS_RST_B_STS), 100, 100000);", 1)

old = '\t\tdev_err(ssusb->dev, "clks of sts1 are not stable!\\n");'
new = ('\t\tdev_err(ssusb->dev, "clks of sts1 are not stable! '
       'sts1=0x%x want 0x%x\\n", value, check_val);')
if old in s:
    s = s.replace(old, new, 1)
else:
    print("warning: message string not found, leaving it alone")

if s == orig:
    print("nothing changed")
    sys.exit(1)

open(P, "w").write(s)
print("patched", P)
