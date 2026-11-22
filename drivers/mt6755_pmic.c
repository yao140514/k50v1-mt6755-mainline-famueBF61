// SPDX-License-Identifier: GPL-2.0
/*
 * MT6353 PMIC access through PWRAP (WACS2) -- bring-up driver.
 *
 * Mainline has no pwrap driver for MT6755, but the bootloader leaves the
 * PWRAP block (0x1000D000) fully initialised (cipher, DIO, clocks) and
 * nothing in the kernel touches it, so the WACS2 interface can be used
 * as-is.  The programming model mirrors LK's pmic_wrap_init_v1.c:
 *
 *   WACS2_CMD    base+0xA0   write<<31 | (adr>>1)<<16 | wdata
 *   WACS2_RDATA  base+0xA4   FSM state in bits [18:16], data in [15:0]
 *   WACS2_VLDCLR base+0xA8   write 1 to clear the read-valid flag
 *
 * The driver dumps the charger block and switches the MT6353 integrated
 * linear charger on following LK's pchr_turn_on_charging() sequence:
 * leave USBDL mode (we are always plugged into USB and USBDL locks the
 * charger), arm the 32 s charger watchdog (which then needs regular
 * kicking -- a delayed work does that), set the 450 mA input limit and
 * finally assert CSDAC_EN + CHR_EN + auto mode.  The vendor-shipped
 * register defaults define the charge profile; the PMIC provides the
 * over-voltage / over-temperature hardware protection.
 *
 * Temporary: replace by a proper power_supply driver once the register
 * map is fully understood.
 */

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>

#define PWRAP_BASE		0x1000D000
#define PWRAP_SIZE		0x100

#define PWRAP_WACS2_CMD		0xA0
#define PWRAP_WACS2_RDATA	0xA4
#define PWRAP_WACS2_VLDCLR	0xA8

#define WACS_FSM_IDLE		0x0
#define WACS_FSM_WFVLDCLR	0x6
#define WACS_FSM_MASK		0x7
#define WACS_FSM_SHIFT		16

/* MT6353 PMIC registers (16-bit, byte addresses) */
#define MT6353_CHR_CON0		0x0F8C
#define MT6353_CHR_CON2		0x0F90
#define MT6353_CHR_CON4		0x0F94	/* CS_VTH [3:0] */
#define MT6353_CHR_CON13	0x0FA6	/* CHRWDT TD/WR/EN */
#define MT6353_CHR_CON15	0x0FAA	/* CHRWDT_INT_EN */
#define MT6353_CHR_CON16	0x0FAC	/* USBDL_RST */
#define MT6353_CHR_CON20	0x0FB4	/* BC11_RST */
#define MT6353_CHR_CON25	0x0FBE	/* CSDAC_MODE */
#define MT6353_EN_STATUS1	0x020E
#define MT6353_EN_STATUS2	0x0210
#define MT6353_TOP_RST_STATUS	0x02AE
#define MT6353_AUXADC_RQST0_SET	0x0E9A
#define MT6353_AUXADC_ADC23	0x0E2E	/* CH0 BATSNS out + RDY bit15 */
#define MT6353_AUXADC_ADC25	0x0E32	/* CH1 ISENSE out + RDY bit15 */

/* CHR_CON0 bits */
#define RG_VCDT_HV_EN		BIT(0)
#define RG_PCHR_AUTOMODE	BIT(2)
#define RG_CSDAC_EN		BIT(3)
#define RG_CHR_EN		BIT(4)
#define RGS_CHRDET		BIT(5)

/* CHR_CON2 bits */
#define RG_VBAT_CV_EN		BIT(1)
#define RG_CS_EN		BIT(3)

/* charger watchdog bits (CHR_CON13) */
#define CHRWDT_WR		BIT(8)
#define CHRWDT_EN		BIT(4)

#define KICK_INTERVAL		msecs_to_jiffies(20000)

static void __iomem *pwrap_base;

static int pwrap_update(u32 adr, u16 mask, u16 set);

static void charger_wdt_kick(struct work_struct *w);

static DECLARE_DELAYED_WORK(wdt_kick, charger_wdt_kick);

static int pwrap_wait_state(u32 want, int tries)
{
	while (tries-- > 0) {
		u32 v = readl(pwrap_base + PWRAP_WACS2_RDATA);

		if (((v >> WACS_FSM_SHIFT) & WACS_FSM_MASK) == want)
			return 0;
		udelay(4);
	}
	return -ETIMEDOUT;
}

static int pwrap_read(u32 adr, u32 *val)
{
	int ret;

	ret = pwrap_wait_state(WACS_FSM_IDLE, 512);
	if (ret)
		return ret;

	writel((0u << 31) | ((adr >> 1) << 16), pwrap_base + PWRAP_WACS2_CMD);

	ret = pwrap_wait_state(WACS_FSM_WFVLDCLR, 512);
	if (ret)
		return ret;

	*val = readl(pwrap_base + PWRAP_WACS2_RDATA) & 0xFFFF;
	writel(1, pwrap_base + PWRAP_WACS2_VLDCLR);
	return 0;
}

static int pwrap_write(u32 adr, u16 val)
{
	int ret;

	ret = pwrap_wait_state(WACS_FSM_IDLE, 512);
	if (ret)
		return ret;

	writel((1u << 31) | ((adr >> 1) << 16) | val, pwrap_base + PWRAP_WACS2_CMD);
	(void)readl(pwrap_base + PWRAP_WACS2_CMD);	/* make sure it lands */
	return 0;
}

/* read-modify-write a PMIC register */
static int pwrap_update(u32 adr, u16 mask, u16 set)
{
	u32 v;
	int ret;

	ret = pwrap_read(adr, &v);
	if (ret)
		return ret;
	return pwrap_write(adr, (u16)(((v & ~mask) | set) & 0xFFFF));
}

static void dump_reg(const char *name, u32 adr)
{
	u32 v = 0;

	if (pwrap_read(adr, &v) == 0)
		pr_info("mt6353: %-24s [0x%04x] = 0x%04x\n", name, adr, v);
	else
		pr_info("mt6353: %-24s [0x%04x] read failed\n", name, adr);
}

static void charger_wdt_kick(struct work_struct *w)
{
	/* the charger watchdog re-arms the timeout and clears on expiry */
	if (pwrap_base)
		pwrap_update(MT6353_CHR_CON13, CHRWDT_WR, CHRWDT_WR);
	schedule_delayed_work(&wdt_kick, KICK_INTERVAL);
}


/*
 * AUXADC: request a conversion, wait for RDY, convert.  Channels 0/1 use
 * raw * 3 * 1800 / 32768 (matches the preloader: raw 20432 -> 3367 mV).
 */
static int auxadc_read_mv(int ch, u32 out_reg)
{
	u32 v;
	int tries = 20;

	pwrap_write(MT6353_AUXADC_RQST0_SET, 1u << ch);
	while (tries-- > 0) {
		if (pwrap_read(out_reg, &v) != 0)
			return -EIO;
		if (v & BIT(15))
			break;
		msleep(1);
	}
	if (tries < 0)
		return -ETIMEDOUT;

	v &= 0x7FFF;
	return (int)(v * 3 * 1800 / 32768);
}

static int auxadc_read_checked(int ch, u32 out_reg)
{
	int mv = auxadc_read_mv(ch, out_reg);

	if (mv < 0 || mv > 6000)
		return -1;
	return mv;
}

static bool charger_active(void)
{
	u32 v = 0;

	if (pwrap_read(MT6353_CHR_CON0, &v) != 0)
		return false;
	return (v & (RGS_CHRDET | RG_CHR_EN)) == (RGS_CHRDET | RG_CHR_EN);
}

static int bat_get_prop(struct power_supply *psy,
			enum power_supply_property psp,
			union power_supply_propval *val)
{
	int mv_bat, mv_i;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = charger_active() ?
			      POWER_SUPPLY_STATUS_CHARGING :
			      POWER_SUPPLY_STATUS_DISCHARGING;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		mv_bat = auxadc_read_checked(0, MT6353_AUXADC_ADC23);
		val->intval = (mv_bat > 0) ? mv_bat * 1000 : 0;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		mv_bat = auxadc_read_checked(0, MT6353_AUXADC_ADC23);
		mv_i = auxadc_read_checked(1, MT6353_AUXADC_ADC25);
		if (mv_bat < 0 || mv_i < 0) {
			val->intval = 0;
		} else {
			/* 68 mOhm sense resistor, LK CUST_R_SENSE */
			val->intval = (mv_i - mv_bat) * 1000 / 68 * 1000;
		}
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		mv_bat = auxadc_read_checked(0, MT6353_AUXADC_ADC23);
		if (mv_bat < 0) {
			val->intval = 0;
		} else {
			int cap = (mv_bat - 3400) * 100 / (4200 - 3400);

			val->intval = clamp(cap, 0, 100);
		}
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LIPO;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static enum power_supply_property bat_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TECHNOLOGY,
};

static const struct power_supply_desc bat_desc = {
	.name = "battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = bat_props,
	.num_properties = ARRAY_SIZE(bat_props),
	.get_property = bat_get_prop,
};

static struct power_supply *bat_psy;

static int __init mt6353_pmic_init(void)
{
	u32 v;
	int i;

	pwrap_base = ioremap(PWRAP_BASE, PWRAP_SIZE);
	if (!pwrap_base) {
		pr_err("mt6353: cannot ioremap PWRAP\n");
		return 0;
	}

	/* sanity: read EN_STATUS1 -- fails if PWRAP is not alive */
	if (pwrap_read(MT6353_EN_STATUS1, &v) != 0) {
		pr_info("mt6353: PWRAP not responding, PMIC dump skipped\n");
		iounmap(pwrap_base);
		pwrap_base = NULL;
		return 0;
	}

	pr_info("mt6353: PWRAP access ok, dumping charger block\n");
	for (i = 0; i < 10; i++)
		dump_reg("CHR_CON", MT6353_CHR_CON0 + i * 2);

	if (pwrap_read(MT6353_CHR_CON0, &v) == 0) {
		pr_info("mt6353: VBUS %s, charger HW %s, auto-mode %s\n",
			(v & RGS_CHRDET) ? "present" : "absent",
			(v & RG_CHR_EN) ? "on" : "off",
			(v & RG_PCHR_AUTOMODE) ? "on" : "off");

		if (!(v & RG_CHR_EN)) {
			/* leave USBDL (usb download lock) mode */
			pwrap_update(MT6353_CHR_CON16, BIT(2), BIT(2));
			/* BC1.2 detection reset */
			pwrap_update(MT6353_CHR_CON20, BIT(1), BIT(1));

			/* watchdog: 32 s timeout, arm it and enable */
			pwrap_update(MT6353_CHR_CON13, 0xF, 0x3);
			pwrap_update(MT6353_CHR_CON13,
				     CHRWDT_WR | CHRWDT_EN, CHRWDT_WR | CHRWDT_EN);
			pwrap_update(MT6353_CHR_CON15, BIT(0), BIT(0));

			/* input current limit: CS_VTH 0xC = 450 mA */
			pwrap_update(MT6353_CHR_CON4, 0xF, 0xC);

			/* the actual switch */
			pwrap_update(MT6353_CHR_CON0,
				     RG_CSDAC_EN | RG_CHR_EN,
				     RG_CSDAC_EN | RG_CHR_EN);
			pwrap_update(MT6353_CHR_CON25, BIT(2), BIT(2));

			/* battery CV + current sense on */
			pwrap_update(MT6353_CHR_CON2,
				     RG_VBAT_CV_EN | RG_CS_EN,
				     RG_VBAT_CV_EN | RG_CS_EN);
			pwrap_update(MT6353_CHR_CON0,
				     RG_VCDT_HV_EN | RG_PCHR_AUTOMODE | RG_CHR_EN,
				     RG_VCDT_HV_EN | RG_PCHR_AUTOMODE | RG_CHR_EN);

			pwrap_read(MT6353_CHR_CON0, &v);
			pr_info("mt6353: charging requested, CHR_CON0 now %#05x\n", v);
		}
	}

	/*
	 * Diagnostics: did any of the writes stick at all?  TD is a benign
	 * field -- write 0x3 and read it back to separate "WACS2 write is
	 * broken" from "the charger hardware clears the enable bits".
	 */
	{
		u32 td = 0;

		pwrap_update(MT6353_CHR_CON13, 0xF, 0x3);
		pwrap_read(MT6353_CHR_CON13, &td);
		pr_info("mt6353: write test: TD bits = %#04x (%s)\n",
			td & 0xF, ((td & 0xF) == 0x3) ? "writes stick" : "writes lost");

		dump_reg("CHR_CON13 (wdt)", MT6353_CHR_CON13);
		dump_reg("CHR_CON15 (wdt int)", MT6353_CHR_CON15);
		dump_reg("CHR_CON16 (usbdl)", MT6353_CHR_CON16);
		dump_reg("CHR_CON20 (bc11)", MT6353_CHR_CON20);
		dump_reg("CHR_CON25 (csdac mode)", MT6353_CHR_CON25);
		dump_reg("TOP_RST_STATUS", MT6353_TOP_RST_STATUS);
	}

	/* keep kicking the watchdog regardless of the state */
	schedule_delayed_work(&wdt_kick, KICK_INTERVAL);

	dump_reg("EN_STATUS1", MT6353_EN_STATUS1);
	dump_reg("EN_STATUS2", MT6353_EN_STATUS2);

	/* /sys/class/power_supply/battery -- standard battery query point */
	{
		struct platform_device *pdev;

		pdev = platform_device_register_simple("mt6353-battery", -1,
						       NULL, 0);
		if (!IS_ERR(pdev)) {
			bat_psy = devm_power_supply_register(&pdev->dev,
							     &bat_desc, NULL);
			if (IS_ERR(bat_psy))
				bat_psy = NULL;
		}
		if (bat_psy)
			pr_info("mt6353: battery registered: cat /sys/class/power_supply/battery/uevent\n");
	}

	pr_info("mt6353: probe done\n");
	return 0;
}
late_initcall(mt6353_pmic_init);

MODULE_DESCRIPTION("MT6353 PMIC/PWRAP bring-up driver (charger + diagnostics)");
MODULE_LICENSE("GPL");
