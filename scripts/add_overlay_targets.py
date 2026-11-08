#!/usr/bin/env python3
"""Add placeholder nodes for every symbol the vendor odmdtbo overlay targets.

LK's ufdt_overlay_do_fixups() aborts the whole boot when a fragment target
cannot be resolved via __symbols__. Our minimal board DTS has none of the
vendor nodes, so the overlay would never apply. Declaring empty nodes with
matching labels lets fixup succeed; the overlay then fills them in.

The label list is taken verbatim from the overlay's __fixups__ section.
"""

import re

DTS = "F:/BF61/mainline/dts/mt6755-k50v1.dts"

SYMBOLS = [
    ("odm", "odm"),
    ("accdet", "accdet"),
    ("pio", "pio"),
    ("touch", "touch"),
    ("kd_camera_hw1", "kd-camera-hw1"),
    ("mtkfb", "mtkfb"),
    ("consys", "consys"),
    ("sleep", "sleep"),
    ("audgpio", "audgpio"),
    ("apuart0", "apuart0"),
    ("xhci0", "xhci0"),
    ("i2c1", "i2c1"),
    ("ext_buck_vmd1", "ext-buck-vmd1"),
    ("i2c0", "i2c0"),
    ("i2c2", "i2c2"),
    ("i2c3", "i2c3"),
    ("i2c4", "i2c4"),
    ("eintc", "eintc"),
    ("msdc1_ins", "msdc1-ins"),
    ("dsi_te", "dsi-te"),
    ("gpio_usage_mapping", "gpio-usage-mapping"),
    ("mt_pmic_vcama_ldo_reg", "mt-pmic-vcama-ldo-reg"),
    ("mt_pmic_vsim1_ldo_reg", "mt-pmic-vsim1-ldo-reg"),
    ("mt_pmic_vsim2_ldo_reg", "mt-pmic-vsim2-ldo-reg"),
    ("mt_pmic_vldo28_ldo_reg", "mt-pmic-vldo28-ldo-reg"),
    ("mt_pmic_vibr_ldo_reg", "mt-pmic-vibr-ldo-reg"),
    ("mt_pmic_vcamd_ldo_reg", "mt-pmic-vcamd-ldo-reg"),
    ("mt_pmic_vcamio_ldo_reg", "mt-pmic-vcamio-ldo-reg"),
    ("auxadc", "auxadc"),
    ("rf_clock_buffer_ctrl", "rf-clock-buffer-ctrl"),
    ("pmic_clock_buffer_ctrl", "pmic-clock-buffer-ctrl"),
    ("keypad", "keypad"),
]

MARK = "/* --- odmdtbo overlay placeholder nodes --- */"

block_lines = [
    "",
    "\t" + MARK,
    "\t/*",
    "\t * The vendor odmdtbo overlay targets these labels. LK resolves them",
    "\t * through __symbols__ and aborts if any one is missing, so they have",
    "\t * to exist even though this board does not use them yet.",
    "\t */",
]
for label, name in SYMBOLS:
    block_lines.append(f"\t{label}: {name} {{ }};")
block = "\n".join(block_lines) + "\n"

src = open(DTS, encoding="utf-8").read()
if MARK in src:
    src = re.sub(r"\n\t/\* --- odmdtbo overlay placeholder nodes --- \*/.*?\n(?=\};)", "", src, flags=re.S)
    src = src.rstrip() + "\n"

idx = -1
m = re.search(r"\n\};\s*\n\s*&uart0", src)
if m:
    idx = m.start()
else:
    idx = src.rfind("\n};")
if idx < 0:
    raise SystemExit("root node end not found")
src = src[:idx] + "\n" + block + src[idx:]

open(DTS, "w", encoding="utf-8", newline="\n").write(src)
print(f"added {len(SYMBOLS)} placeholder nodes")
