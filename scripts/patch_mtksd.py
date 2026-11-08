#!/usr/bin/env python3
"""Local bring-up patch: make mtk-sd tolerate a missing pinctrl provider."""
import io
import os
import re

P = os.path.expanduser("~/mt6755/linux-6.12.115/drivers/mmc/host/mtk-sd.c")
s = io.open(P, encoding="utf-8").read()

if "LOCAL PATCH (bring-up): no pinctrl" in s:
    print("already patched")
    raise SystemExit(0)

T = "\t"

# --- 1. probe(): pinctrl becomes optional -------------------------------
pat = re.compile(
    r"\thost->pinctrl = devm_pinctrl_get\(&pdev->dev\);\n"
    r".*?"
    r"return PTR_ERR\(host->pins_uhs\);\n\t\}\n",
    re.S,
)
new_probe = (
    T + "/*\n"
    + T + " * LOCAL PATCH (bring-up): no pinctrl driver exists for mt6755 in\n"
    + T + " * mainline yet and the bootloader already muxed the MSDC pins, so\n"
    + T + " * treat pinctrl as optional instead of failing the probe.\n"
    + T + " */\n"
    + T + "host->pinctrl = devm_pinctrl_get(&pdev->dev);\n"
    + T + "if (IS_ERR(host->pinctrl)) {\n"
    + T + T + 'dev_warn(&pdev->dev, "no pinctrl, continuing (bring-up)\\n");\n'
    + T + T + "host->pinctrl = NULL;\n"
    + T + T + "host->pins_default = NULL;\n"
    + T + T + "host->pins_uhs = NULL;\n"
    + T + T + "host->pins_eint = NULL;\n"
    + T + "} else {\n"
    + T + T + 'host->pins_default = pinctrl_lookup_state(host->pinctrl, "default");\n'
    + T + T + "if (IS_ERR(host->pins_default)) {\n"
    + T + T + T + 'dev_warn(&pdev->dev, "no pinctrl default, continuing\\n");\n'
    + T + T + T + "host->pins_default = NULL;\n"
    + T + T + "}\n"
    + T + T + 'host->pins_uhs = pinctrl_lookup_state(host->pinctrl, "state_uhs");\n'
    + T + T + "if (IS_ERR(host->pins_uhs)) {\n"
    + T + T + T + 'dev_warn(&pdev->dev, "no pinctrl uhs, continuing\\n");\n'
    + T + T + T + "host->pins_uhs = NULL;\n"
    + T + T + "}\n"
    + T + "}\n"
)
s, n = pat.subn(lambda m: new_probe, s, count=1)
print("probe replaced:", n)
if n != 1:
    raise SystemExit(1)

# --- 2. guard pinctrl_select_state calls -------------------------------
pat2 = re.compile(
    r"\t\tif \(ios->signal_voltage == MMC_SIGNAL_VOLTAGE_180\)\n"
    r"\t\t\tpinctrl_select_state\(host->pinctrl, host->pins_uhs\);\n"
    r"\t\telse\n"
    r"\t\t\tpinctrl_select_state\(host->pinctrl, host->pins_default\);\n"
)
new2 = (
    T + T + "if (ios->signal_voltage == MMC_SIGNAL_VOLTAGE_180) {\n"
    + T + T + T + "if (host->pins_uhs)\n"
    + T + T + T + T + "pinctrl_select_state(host->pinctrl, host->pins_uhs);\n"
    + T + T + "} else {\n"
    + T + T + T + "if (host->pins_default)\n"
    + T + T + T + T + "pinctrl_select_state(host->pinctrl, host->pins_default);\n"
    + T + T + "}\n"
)
s, n2 = pat2.subn(lambda m: new2, s, count=1)
print("select_state guarded:", n2)
if n2 != 1:
    raise SystemExit(1)

# --- 3. eint lookup needs a non-NULL pinctrl ---------------------------
old3 = T + "if ((mmc->pm_caps & MMC_PM_WAKE_SDIO_IRQ) && (mmc->pm_caps & MMC_PM_KEEP_POWER)) {\n"
new3 = (T + "if (host->pinctrl && (mmc->pm_caps & MMC_PM_WAKE_SDIO_IRQ) &&\n"
        + T + "    (mmc->pm_caps & MMC_PM_KEEP_POWER)) {\n")
print("eint anchor:", s.count(old3))
if s.count(old3) != 1:
    raise SystemExit(1)
s = s.replace(old3, new3, 1)

io.open(P, "w", encoding="utf-8", newline="\n").write(s)
print("patched mtk-sd: pinctrl optional")
