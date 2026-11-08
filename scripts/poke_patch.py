#!/usr/bin/env python3
"""Insert raw UART breadcrumb pokes into MTK LK early init.

Writes single chars straight to UART0 THR (0x11002000), bypassing
uart_init_early() and the dprintf/DEBUGLEVEL machinery entirely.

Trace          where it died
-----          -------------
(nothing)      before kmain  (crt0 / arch setup)
K only         thread_init_early or arch_early_init
K A            platform_init_interrupts
K A B          platform_early_init_timer
K A B C        mt_gpio_set_default
K A B C D      uart_init_early
K A B C D E    mtk_wdt_init
K A B C D E F  i2c_hw_init
K A B C D E F G pmic_init   (H = platform_early_init done)
"""
import io
import os

LK = os.path.expanduser("~/mt6755/lk")

MACRO = (
    "/* --- early boot breadcrumbs (bring-up diagnosis) --- */\n"
    "#define EARLY_POKE(c) do { *(volatile unsigned char *)0x11002000 = (unsigned char)(c); } while (0)\n"
)


def replace_once(src, old, new, tag):
    n = src.count(old)
    if n != 1:
        print(f"  !! {tag}: anchor appears {n} times, expected 1 -> skipped")
        return src
    print(f"  ok  {tag}")
    return src.replace(old, new)


def add_macro(src):
    if "EARLY_POKE(c)" in src:
        return src
    return MACRO + src


# ---------- platform.c ----------
p = os.path.join(LK, "platform/mt6755/platform.c")
with io.open(p, encoding="utf-8") as f:
    s = f.read()

if "EARLY_POKE('A')" in s:
    print("platform.c already patched")
else:
    print("=== platform/mt6755/platform.c ===")
    s = replace_once(
        s,
        "\tplatform_init_interrupts();",
        "\tEARLY_POKE('A');\n\tplatform_init_interrupts();\n\tEARLY_POKE('B');",
        "A/B around platform_init_interrupts",
    )
    s = replace_once(s, "\tplatform_early_init_timer();",
                     "\tplatform_early_init_timer();\n\tEARLY_POKE('C');", "C timer init")
    s = replace_once(s, "\tmt_gpio_set_default();",
                     "\tmt_gpio_set_default();\n\tEARLY_POKE('D');", "D gpio default")
    s = replace_once(s, "\tuart_init_early();",
                     "\tuart_init_early();\n\tEARLY_POKE('E');", "E uart init")
    s = replace_once(s, "\tmtk_wdt_init();",
                     "\tmtk_wdt_init();\n\tEARLY_POKE('F');", "F wdt init")
    s = replace_once(s, "\ti2c_hw_init();",
                     "\ti2c_hw_init();\n\tEARLY_POKE('G');", "G i2c init")
    s = replace_once(s, "\tpmic_init();",
                     "\tpmic_init();\n\tEARLY_POKE('H');", "H pmic init")
    s = add_macro(s)
    with io.open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write(s)

# ---------- main.c ----------
p = os.path.join(LK, "kernel/main.c")
with io.open(p, encoding="utf-8") as f:
    s = f.read()

if "EARLY_POKE('K')" in s:
    print("main.c already patched")
else:
    print("=== kernel/main.c ===")
    s = replace_once(s, "void kmain(void)\n{",
                     "void kmain(void)\n{\n\tEARLY_POKE('K');", "K kmain entry")
    s = add_macro(s)
    with io.open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write(s)

print("done")
