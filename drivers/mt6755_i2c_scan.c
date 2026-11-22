// SPDX-License-Identifier: GPL-2.0
/*
 * One-shot I2C census at boot.
 *
 * Neither the touch controller (AXS15260 @0x5d) nor the switching charger
 * (@0x6b in the stock DT) has a driver yet, and the board has several buses
 * (two hardware controllers plus the bit-banged ones on the display FPC).
 * Rather than guess which bus a chip sits on, probe every adapter once and
 * print the addresses that ACK.  Delete once the real drivers exist.
 */

#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kernel.h>

#define SCAN_FIRST	0x08
#define SCAN_LAST	0x77
#define SCAN_BUSES	8

static int __init mt6755_i2c_scan(void)
{
	int bus, addr, found;

	for (bus = 0; bus < SCAN_BUSES; bus++) {
		struct i2c_adapter *adap = i2c_get_adapter(bus);

		if (!adap)
			continue;

		found = 0;
		for (addr = SCAN_FIRST; addr <= SCAN_LAST; addr++) {
			union i2c_smbus_data data;
			int w, r, val = -1;

			w = i2c_smbus_xfer(adap, addr, 0, I2C_SMBUS_WRITE, 0,
					   I2C_SMBUS_QUICK, &data);
			r = i2c_smbus_xfer(adap, addr, 0, I2C_SMBUS_READ, 0,
					   I2C_SMBUS_BYTE, &data);
			if (w < 0 && r < 0)
				continue;
			if (r >= 0)
				val = data.byte;
			pr_info("i2cscan: bus %d (%s) addr %#04x ACK (w=%d r=%d byte=%#04x)\n",
				bus, adap->name, addr, w, r, val);
			found++;
		}
		pr_info("i2cscan: bus %d (%s): %d device(s)\n",
			bus, adap->name, found);
		i2c_put_adapter(adap);
	}
	return 0;
}
late_initcall(mt6755_i2c_scan);
