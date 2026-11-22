// SPDX-License-Identifier: GPL-2.0
/*
 * Charging IC bring-up probe.
 *
 * The stock overlay wires three charging-related devices on i2c1:
 *
 *	eta6937@6a		(ETA6937 switching charger)
 *	switching_charger@2d	(generic MTK node, second charger?)
 *	msa311@62		(G-sensor -- answers, so the bus is alive)
 *
 * Only 0x62 answered a QUICK write, which some ICs simply NACK, so this
 * probe walks the candidate addresses with a plain byte READ (the most
 * permissive form) and dumps whichever one answers.
 *
 * Temporary: delete once the real charger driver is bound.
 */

#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>

#define CHG_BUS		1
#define CHG_REG_LAST	0x0f	/* ETA6937 register space is 0x00-0x0f */

static const unsigned short chg_addrs[] = { 0x6a, 0x2d, 0x62 };

static int __init mt6755_charger_probe(void)
{
	struct i2c_adapter *ad;
	int a, i;

	ad = i2c_get_adapter(CHG_BUS);
	if (!ad) {
		pr_info("chgprobe: bus %d not present\n", CHG_BUS);
		return 0;
	}

	for (a = 0; a < ARRAY_SIZE(chg_addrs); a++) {
		struct i2c_client *cl;
		int i, v, ack = 0;

		cl = i2c_new_dummy_device(ad, chg_addrs[a]);
		if (IS_ERR(cl))
			continue;

		for (i = 0; i <= CHG_REG_LAST; i++) {
			v = i2c_smbus_read_byte_data(cl, i);
			if (v < 0)
				pr_info("chgprobe: %#04x reg %#04x = %d\n",
					chg_addrs[a], i, v);
			else
				pr_info("chgprobe: %#04x reg %#04x = %#04x\n",
					chg_addrs[a], i, v);
			ack++;
		}

		pr_info("chgprobe: %#04x probe done (%d reads)\n",
			chg_addrs[a], ack);
		i2c_unregister_device(cl);
	}

	i2c_put_adapter(ad);
	return 0;
}
late_initcall(mt6755_charger_probe);
