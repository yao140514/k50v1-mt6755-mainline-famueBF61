// SPDX-License-Identifier: GPL-2.0
/*
 * MT6353 PMIC access through PWRAP (WACS2) -- bring-up driver.
 *
 * Mainline has no pwrap driver for MT6755, but the bootloader leaves the
 * PWRAP block (0x1000D000) fully initialised (cipher, DIO, clocks) and
 * nothing in the kernel touches it, so the WACS2 interface can be used
 * as-is.  The programming model below mirrors LK's pmic_wrap_init_v1.c:
 *
 *   WACS2_CMD    base+0xA0   write<<31 | (adr>>1)<<16 | wdata
 *   WACS2_RDATA  base+0xA4   FSM state in bits [18:16], data in [15:0]
 *   WACS2_VLDCLR base+0xA8   write 1 to clear the read-valid flag
 *
 * On top of that this driver dumps the charger block and switches the
 * MT6353 integrated linear charger on (auto mode).  The PMIC has hardware
 * over-voltage / over-temperature protection, and the vendor preloader uses
 * the same auto mode for its battery-protection path, so the defaults are
 * the vendor-shipped charge profile.
 *
 * Temporary: replace by a proper power_supply driver once the register map
 * is fully understood.
 */

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>

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
#define MT6353_EN_STATUS1	0x020E
#define MT6353_EN_STATUS2	0x0210

/* CHR_CON0 bits */
#define RG_VCDT_HV_EN		BIT(0)
#define RG_PCHR_AUTOMODE	BIT(2)
#define RG_CHR_EN		BIT(4)
#define RGS_CHRDET		BIT(5)

/* CHR_CON2 bits */
#define RG_VBAT_CV_EN		BIT(1)
#define RG_CS_EN		BIT(3)

static void __iomem *pwrap;

static int pwrap_wait_state(u32 want, int tries)
{
	while (tries-- > 0) {
		u32 v = readl(pwrap + PWRAP_WACS2_RDATA);

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

	writel((0u << 31) | ((adr >> 1) << 16), pwrap + PWRAP_WACS2_CMD);

	ret = pwrap_wait_state(WACS_FSM_WFVLDCLR, 512);
	if (ret)
		return ret;

	*val = readl(pwrap + PWRAP_WACS2_RDATA) & 0xFFFF;
	writel(1, pwrap + PWRAP_WACS2_VLDCLR);
	return 0;
}

static int pwrap_write(u32 adr, u16 val)
{
	int ret;

	ret = pwrap_wait_state(WACS_FSM_IDLE, 512);
	if (ret)
		return ret;

	writel((1u << 31) | ((adr >> 1) << 16) | val, pwrap + PWRAP_WACS2_CMD);
	(void)readl(pwrap + PWRAP_WACS2_CMD);	/* make sure the cmd lands */
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
		pr_info("mt6353: %-22s [0x%04x] = 0x%04x\n", name, adr, v);
	else
		pr_info("mt6353: %-22s [0x%04x] read failed\n", name, adr);
}

static int __init mt6353_pmic_init(void)
{
	u32 v;
	int i, ok = 0;

	pwrap = ioremap(PWRAP_BASE, PWRAP_SIZE);
	if (!pwrap) {
		pr_err("mt6353: cannot ioremap PWRAP\n");
		return 0;
	}

	/* sanity: read EN_STATUS1 -- fails if PWRAP is not alive */
	if (pwrap_read(MT6353_EN_STATUS1, &v) != 0) {
		pr_info("mt6353: PWRAP not responding, PMIC dump skipped\n");
		iounmap(pwrap);
		pwrap = NULL;
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
			/* vendor default profile: PMIC auto charge mode */
			pwrap_update(MT6353_CHR_CON2, RG_VBAT_CV_EN | RG_CS_EN,
				     RG_VBAT_CV_EN | RG_CS_EN);
			pwrap_update(MT6353_CHR_CON0,
				     RG_VCDT_HV_EN | RG_PCHR_AUTOMODE | RG_CHR_EN,
				     RG_VCDT_HV_EN | RG_PCHR_AUTOMODE | RG_CHR_EN);
			pwrap_read(MT6353_CHR_CON0, &v);
			pr_info("mt6353: charging requested, CHR_CON0 now %#05x\n", v);
		}
	}

	dump_reg("EN_STATUS1", MT6353_EN_STATUS1);
	dump_reg("EN_STATUS2", MT6353_EN_STATUS2);

	pr_info("mt6353: probe done (%d regs ok)\n", ++ok);
	return 0;
}
late_initcall(mt6353_pmic_init);
