#!/usr/bin/env python3
"""Release the touch controller from reset (GPIO10 = CTP_RST) in LK."""
import sys

p = 'platform/mt6755/platform.c'
s = open(p).read()

old = '\tdprintf(CRITICAL, "[BRINGUP] VLDO28 (touch) enabled\\n");\n'

new = old + """
	/*
	 * bring-up: release the touch controller from reset.
	 * GPIO10 is GPIO_CTP_RST_PIN in cust_gpio_usage.h and the boot GPIO
	 * table leaves it driven low, so the chip never leaves reset unless
	 * something raises it. No driver does that yet.
	 */
	mt_set_gpio_mode(10, GPIO_MODE_00);
	mt_set_gpio_dir(10, GPIO_DIR_OUT);
	mt_set_gpio_out(10, GPIO_OUT_ONE);
	dprintf(CRITICAL, "[BRINGUP] CTP reset released (GPIO10 high)\\n");
"""

if old not in s:
    sys.exit('anchor not found')
if 'GPIO10 high' in s:
    sys.exit('already patched')
s = s.replace(old, new, 1)
open(p, 'w').write(s)
print('GPIO10 release added')
