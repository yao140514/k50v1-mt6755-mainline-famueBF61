#!/usr/bin/env python3
"""Enable VLDO28 (touch supply) from LK, before the kernel starts."""
import sys

p = 'platform/mt6755/platform.c'
s = open(p).read()

old = """	mt_set_gpio_ies(92, 1);
	mt_set_gpio_ies(93, 1);
	EARLY_POKE('G');
"""

new = """	mt_set_gpio_ies(92, 1);
	mt_set_gpio_ies(93, 1);
	EARLY_POKE('G');

	/*
	 * bring-up: power the touch panel.
	 *
	 * The vendor device tree says vtouch-supply = <&mt_pmic_vldo28_ldo_reg>,
	 * so the touch controller runs from VLDO28. Nothing in the mainline
	 * kernel can switch that rail on (no MT6353 driver exists), and with the
	 * rail off the controller only answers its I2C address through the ESD
	 * diodes while its register file stays dead -- exactly what we measured:
	 * ACK at 0x38, deterministic garbage on every read.
	 *
	 * Turning it on here means the kernel inherits a powered panel without
	 * needing any PMIC driver. VLDO28 is a fixed 2.8 V share LDO, so only
	 * its two enable inputs have to be asserted.
	 */
	pmic_set_register_value(PMIC_LDO_VLDO28_EN_0, 1);
	pmic_set_register_value(PMIC_LDO_VLDO28_EN_1, 1);
	dprintf(CRITICAL, "[BRINGUP] VLDO28 (touch) enabled\\n");
"""

if old not in s:
    sys.exit('anchor block not found in platform.c')
s = s.replace(old, new, 1)

anchor = 'extern int i2c_hw_init(void);'
if anchor not in s:
    sys.exit('extern anchor not found')
if 'pmic_set_register_value' not in s.split(anchor)[0]:
    s = s.replace(anchor, anchor + '\nextern unsigned short pmic_set_register_value(PMU_FLAGS_LIST_ENUM flagname, unsigned int val);', 1)

open(p, 'w').write(s)
print('platform.c patched')
