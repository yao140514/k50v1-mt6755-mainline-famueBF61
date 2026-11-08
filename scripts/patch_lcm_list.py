#!/usr/bin/env python3
"""Register the nt35695B LCM driver in LK's lcm_driver_list.

The repo ships the nt35695B driver source but never lists it in
dev/lcm/mt65xx_lcm_list.c, so lcm_driver_list ends up empty and the
LCM_COMPILE_ASSERT fails.
"""

P = "/home/yao14/mt6755/lk/dev/lcm/mt65xx_lcm_list.c"
NAME = "nt35695B_fhd_dsi_cmd_auo_nt50358_lcm_drv"
MACRO = "NT35695B_FHD_DSI_CMD_AUO_NT50358"

src = open(P, encoding="utf-8", errors="replace").read()
if NAME in src:
    print("already present")
else:
    anchor = "extern LCM_DRIVER hct_otm1282_dsi_vdo_hd_auo;"
    if anchor not in src:
        raise SystemExit("declaration anchor not found")
    src = src.replace(anchor, anchor + f"\nextern LCM_DRIVER {NAME};", 1)

    idx = src.rfind("};")
    if idx < 0:
        raise SystemExit("list terminator not found")
    entry = (
        f"#if defined({MACRO})\n"
        f"       &{NAME},\n"
        f"#endif\n"
    )
    src = src[:idx] + entry + src[idx:]
    open(P, "w", encoding="utf-8", newline="\n").write(src)
    print("patched")

for i, line in enumerate(open(P, encoding="utf-8", errors="replace"), 1):
    if NAME in line or MACRO in line:
        print(f"  {i}: {line.rstrip()}")
