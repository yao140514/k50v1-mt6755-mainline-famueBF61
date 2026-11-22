// SPDX-License-Identifier: GPL-2.0
/*
 * Charging IC bring-up probe.
 *
 * The stock overlay puts "swithing_charger" at 0x6b on i2c1, but the boot
 * census shows the only live device on that bus is at 0x62, and LK already
 * talked to it there (its REG00 is the input-current limit, like the BQ2589x
 * family).  Dump the whole register space so the chip can be identified and
 * a proper power-supply driver chosen.
 *
 * Temporary: delete once the real charger driver is bound.
 */

#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>

#define CHG_BUS		1
#define CHG_ADDR	0x62
#define CHG_REG_LAST	0x14	/* BQ2589x: REG14 is the chip ID */

static int __init mt6755_charger_probe(void)
{
	struct i2c_adapter *ad;
	struct i2c_client *cl;
	int i, v;

	ad = i2c_get_adapter(CHG_BUS);
	if (!ad) {
		pr_info("chgprobe: bus %d not present\n", CHG_BUS);
		return 0;
	}

	cl = i2c_new_dummy_device(ad, CHG_ADDR);
	if (IS_ERR(cl)) {
		pr_info("chgprobe: no device at %#04x on bus %d\n",
			CHG_ADDR, CHG_BUS);
		i2c_put_adapter(ad);
		return 0;
	}

	for (i = 0; i <= CHG_REG_LAST; i++) {
		v = i2c_smbus_read_byte_data(cl, i);
		if (v < 0)
			pr_info("chgprobe: reg %#04x = %d\n", i, v);
		else
			pr_info("chgprobe: reg %#04x = %#04x\n", i, v);
	}

	i2c_unregister_device(cl);
	i2c_put_adapter(ad);
	return 0;
}
late_initcall(mt6755_charger_probe);
