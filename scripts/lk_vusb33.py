#!/usr/bin/env python3
"""LK bring-up tweaks, round 3.

1) Enable VUSB33 -- the USB PHY's 3.3 V rail. The vendor PHY driver
   (mu3phy/mt6755/mtk-phy-asic.c, phy_init_soc) does exactly this as the
   very first step for MT6353; without it the PHY registers are readable
   but the transceiver has no supply, so the D+ pull-up never appears on
   the wire even though the MAC sets SOFT_CONN.

2) Drop the GPIO10 override: GPIO_CTP_RST_PIN turned out not to be the
   touch reset (Android keeps it low while the touch works), so leave the
   pin at its DCT default instead of driving it high.
"""
import re

P = "/home/yao14/mt6755/lk/platform/mt6755/platform.c"
s = open(P).read()

old_vldo = """	pmic_set_register_value(PMIC_LDO_VLDO28_EN_0, 1);
	pmic_set_register_value(PMIC_LDO_VLDO28_EN_1, 1);
	dprintf(CRITICAL, "[BRINGUP] VLDO28 (touch) enabled\\n");
"""
new_vldo = """	pmic_set_register_value(PMIC_LDO_VLDO28_EN_0, 1);
	pmic_set_register_value(PMIC_LDO_VLDO28_EN_1, 1);
	dprintf(CRITICAL, "[BRINGUP] VLDO28 (touch) enabled\\n");

	/*
	 * bring-up: power the USB PHY.
	 *
	 * The vendor SSUSB PHY driver enables VUSB33 as the first step of
	 * phy_init_soc() (CONFIG_MTK_PMIC_CHIP_MT6353 branch). Without that
	 * rail the PHY's registers still read and write normally -- they live
	 * on the always-on 1.8 V domain -- but the transceiver has no supply,
	 * so driving SOFT_CONN in the MAC produces no D+ pull-up and the host
	 * never sees a device.
	 */
	pmic_set_register_value(PMIC_LDO_VUSB33_EN, 1);
	dprintf(CRITICAL, "[BRINGUP] VUSB33 (usb phy) enabled\\n");
"""

assert old_vldo in s, "VLDO28 block not found"
s = s.replace(old_vldo, new_vldo, 1)

old_gpio = """	/*
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
new_gpio = """	/*
	 * GPIO10 (GPIO_CTP_RST_PIN) is deliberately left alone: Android keeps
	 * it low while the touch controller works, so it is not the reset line
	 * and driving it high only added noise to the picture.
	 */
"""

if old_gpio in s:
    s = s.replace(old_gpio, new_gpio, 1)
    print("GPIO10 override removed")
else:
    print("warning: GPIO10 block not found, left as is")

open(P, "w").write(s)
print("patched", P)
