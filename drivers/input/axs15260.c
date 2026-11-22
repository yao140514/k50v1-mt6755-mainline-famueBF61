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
	struct gpio_desc *reset;
	int i, ret;

	reset = devm_gpiod_get_optional(&client->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(reset))
		return dev_err_probe(&client->dev, PTR_ERR(reset),
				     "cannot get reset gpio\n");

	if (reset) {
		/* assert, then release, and give the firmware time to boot */
		gpiod_set_value_cansleep(reset, 0);
		msleep(30);
		gpiod_set_value_cansleep(reset, 1);
		msleep(150);
		dev_info(&client->dev, "reset released\n");
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
