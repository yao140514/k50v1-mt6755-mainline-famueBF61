// SPDX-License-Identifier: GPL-2.0
/*
 * AXS15260 touch controller - bring-up probe.
 *
 * The panel FPC carries an AXS15260 behind the same i2c bus as the LCD bias
 * chip (0x3b answers, 0x5d does not).  Before writing a real input driver we
 * need to know whether the chip is simply held in reset: the vendor DTS wires
 *
 *	RST  = GPIO10   (pinctrl "rstoutput0/1" in the stock overlay)
 *	INT  = GPIO1    (EINT0, "eint0default")
 *
 * so this driver releases reset, waits for the controller to boot and then
 * dumps its register space.  Once the protocol is known the dump can be
 * replaced by a proper input driver.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>

#define AXS_NAME	"axs15260"

static int axs_probe(struct i2c_client *client)
{
	struct gpio_desc *reset, *intg;
	int i, ret;

	reset = devm_gpiod_get_optional(&client->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(reset))
		return dev_err_probe(&client->dev, PTR_ERR(reset),
				     "cannot get reset gpio\n");

	/* INT level tells us whether the chip is alive even before i2c works */
	intg = devm_gpiod_get_optional(&client->dev, "int", GPIOD_IN);
	if (IS_ERR(intg))
		intg = NULL;
	if (intg)
		dev_info(&client->dev, "int gpio level = %d\n",
			 gpiod_get_value_cansleep(intg));

	if (reset) {
		dev_info(&client->dev,
			 "reset gpio: dir=%s raw_before=%d\n",
			 gpiod_get_direction(reset) ? "in" : "out",
			 gpiod_get_value_cansleep(reset));
		/*
		 * With the DT flag as active-high, logical 1 drives the pad high
		 * = reset asserted, logical 0 = line low = running (this is how
		 * Android keeps the pin during normal operation).  Pulse high,
		 * then land back on low.
		 */
		gpiod_set_value_cansleep(reset, 1);
		msleep(30);
		gpiod_set_value_cansleep(reset, 0);
		msleep(150);
		dev_info(&client->dev, "reset released (gpio now %d)\n",
			 gpiod_get_value_cansleep(reset));
	}

	for (i = 0; i < 0x10; i++) {
		ret = i2c_smbus_read_byte_data(client, i);
		dev_info(&client->dev, "reg %#04x = %d\n", i, ret);
	}
	for (i = 0x80; i < 0x90; i++) {
		ret = i2c_smbus_read_byte_data(client, i);
		dev_info(&client->dev, "reg %#04x = %d\n", i, ret);
	}

	dev_info(&client->dev, "probe done (no input driver yet)\n");
	return 0;
}

static const struct of_device_id axs_of_match[] = {
	{ .compatible = "axs,axs15260" },
	{ }
};
MODULE_DEVICE_TABLE(of, axs_of_match);

static struct i2c_driver axs_driver = {
	.driver = {
		.name = AXS_NAME,
		.of_match_table = axs_of_match,
	},
	.probe = axs_probe,
};
module_i2c_driver(axs_driver);

MODULE_DESCRIPTION("AXS15260 touch controller bring-up probe");
MODULE_LICENSE("GPL");
