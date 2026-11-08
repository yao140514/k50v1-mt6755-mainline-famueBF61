#!/usr/bin/env python3
"""Force LK's UART log on and extend breadcrumbs past platform_early_init."""
import io
import os

LK = os.path.expanduser("~/mt6755/lk")


def replace_once(src, old, new, tag):
    n = src.count(old)
    if n != 1:
        print(f"  !! {tag}: anchor x{n} (expected 1) -> skipped")
        return src
    print(f"  ok  {tag}")
    return src.replace(old, new, 1)


# ---------- platform.c: force log_enable ----------
p = os.path.join(LK, "platform/mt6755/platform.c")
with io.open(p, encoding="utf-8") as f:
    s = f.read()

if "force_log_on" in s:
    print("platform.c already has force_log_on")
else:
    print("=== platform/mt6755/platform.c ===")
    s = replace_once(
        s,
        "\t/* initialize the uart */\n\tuart_init_early();",
        "\t/* initialize the uart */\n"
        "\t/* force_log_on */\n"
        "\tif (g_boot_arg) {\n"
        "\t\tg_boot_arg->log_enable = 1;\n"
        "\t\tg_boot_arg->log_dynamic_switch = 1;\n"
        "\t}\n"
        "\tuart_init_early();",
        "force log_enable before uart init",
    )
    with io.open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write(s)

# ---------- main.c: extend breadcrumbs ----------
p = os.path.join(LK, "kernel/main.c")
with io.open(p, encoding="utf-8") as f:
    s = f.read()

if "EARLY_POKE('I')" in s:
    print("main.c already extended")
else:
    print("=== kernel/main.c ===")
    s = replace_once(s, "\tplatform_early_init();",
                     "\tplatform_early_init();\n\tEARLY_POKE('I');", "I after platform_early_init")
    s = replace_once(s, "\ttarget_early_init();",
                     "\ttarget_early_init();\n\tEARLY_POKE('J');", "J after target_early_init")
    s = replace_once(s, '\tdprintf(INFO, "LK_VER_TAG = %s\\n", LK_VER_TAG);',
                     '\tdprintf(INFO, "LK_VER_TAG = %s\\n", LK_VER_TAG);\n\tEARLY_POKE(\'L\');',
                     "L after welcome banner")
    s = replace_once(s, "\theap_init();", "\theap_init();\n\tEARLY_POKE('M');", "M after heap_init")
    s = replace_once(s, "\tthread_init();", "\tthread_init();\n\tEARLY_POKE('N');", "N after thread_init")
    s = replace_once(s, "\ttimer_init();", "\ttimer_init();\n\tEARLY_POKE('O');", "O after timer_init")
    with io.open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write(s)

print("done")
