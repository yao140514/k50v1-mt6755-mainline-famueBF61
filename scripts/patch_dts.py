#!/usr/bin/env python3
"""Inject a padded /chosen/bootargs into the board DTS.

MTK's LK does ASSERT(fdt_bootargs) in app/mt_boot/bootargs.c when the
property is missing, and later rewrites it with the full command line.
So the property must exist and be long enough to hold the merged cmdline.
"""

import re
import sys

DTS = "F:/BF61/mainline/dts/mt6755-k50v1.dts"
CMDLINE = "console=ttyS0,921600n8 earlycon loglevel=8 ignore_loglevel"
RESERVE = 1024

block = f'''	chosen {{
		stdout-path = "serial0";
		bootargs = "{CMDLINE.ljust(RESERVE)}";
	}};'''

src = open(DTS, encoding="utf-8").read()
new, n = re.subn(r"\tchosen \{.*?\n\t\};", block, src, count=1, flags=re.S)
if n != 1:
    sys.exit(f"chosen block not matched (n={n})")

open(DTS, "w", encoding="utf-8", newline="\n").write(new)
print(f"bootargs reserved {RESERVE} bytes, cmdline: {CMDLINE}")
