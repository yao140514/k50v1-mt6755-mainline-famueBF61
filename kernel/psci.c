// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 * Copyright (C) 2013 ARM Limited
 *
 * Author: Will Deacon <will.deacon@arm.com>
 */

#define pr_fmt(fmt) "psci: " fmt

#include <linux/init.h>
#include <linux/of.h>
#include <linux/smp.h>
#include <linux/delay.h>
#include <linux/psci.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/bits.h>

#include <uapi/linux/psci.h>

#include <asm/cpu_ops.h>
#include <asm/errno.h>
#include <asm/smp_plat.h>

static int __init cpu_psci_cpu_init(unsigned int cpu)
{
	return 0;
}

static int __init cpu_psci_cpu_prepare(unsigned int cpu)
{
	if (!psci_ops.cpu_on) {
		pr_err("no cpu_on method, not booting CPU%d\n", cpu);
		return -ENODEV;
	}

	return 0;
}

/*
 * ---------------------------------------------------------------------------
 * MT6755 secondary CPU power-up
 * ---------------------------------------------------------------------------
 *
 * ATF on this SoC answers PSCI CPU_ON with SUCCESS but leaves the SPM alone:
 * MP0_CPUx_PWR_CON reads the same value before and after the call, PWR_RST_B
 * still 0.  The core stays in reset, never writes secondary_data.status, and
 * Linux prints
 *
 *	CPU1: failed in unknown state : 0x0
 *
 * So the sequence has to be replayed here.  The vendor's
 * spm_mtcmos_ctrl_cpuN(STA_POWER_ON) is, in order:
 *
 *	PWR_ON | PWR_ON_2ND -> wait PWR_STATUS and PWR_STATUS_2ND
 *	PWR_ISO = 0
 *	L1 SRAM_PDN = 0     -> wait SRAM_PDN_ACK
 *	udelay(1)
 *	SRAM_ISOINT_B = 1
 *	SRAM_CKISO = 0
 *	PWR_CLK_DIS = 0
 *	PWR_RST_B = 1
 *
 * A core that is already powered but not online (at cold boot ATF leaves
 * MP1_CPU0/1 on, parked outside Linux) has to be cycled first, otherwise the
 * reset release below does nothing useful.
 */
#define MT6755_SPM_BASE		0x10006000
#define MT6755_SPM_SIZE		0x1000
#define SPM_POWERON_CONFIG_EN	0x000
#define SPM_PWR_STATUS		0x180
#define SPM_PWR_STATUS_2ND	0x184
#define SPM_REGWR_EN		(0xb16u << 16 | 0x1)

/* Per-core PWR_CON bits; identical layout for every core on this SoC. */
#define PWR_RST_B		BIT(0)
#define PWR_ISO			BIT(1)
#define PWR_ON			BIT(2)
#define PWR_ON_2ND		BIT(3)
#define PWR_CLK_DIS		BIT(4)
#define SRAM_CKISO		BIT(5)
#define SRAM_ISOINT_B		BIT(6)
#define SRAM_PDN		BIT(0)
#define SRAM_PDN_ACK		BIT(8)

static const u16 mt6755_pwr_con[8] = {
	0x208, 0x20c, 0x210, 0x214, 0x21c, 0x220, 0x224, 0x228,
};
static const u16 mt6755_l1_pdn[8] = {
	0x248, 0x24c, 0x250, 0x254, 0x260, 0x264, 0x268, 0x26c,
};
/* cluster level: MP0 then MP1 */
static const u16 mt6755_top_con[2] = { 0x204, 0x218 };
static const u16 mt6755_l2_pdn[2]  = { 0x240, 0x258 };

static void __iomem *mt6755_spm;

static u32 spm_rd(u32 off)
{
	return readl(mt6755_spm + off);
}

static void spm_wr(u32 off, u32 val)
{
	writel(val, mt6755_spm + off);
}

static int spm_wait(u32 off, u32 mask, bool set, int us)
{
	int i;

	for (i = 0; i < us; i++) {
		if (!!(spm_rd(off) & mask) == set)
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

static void mt6755_cluster_on(int cl)
{
	/* MP0_CPUTOP is bit8, MP1_CPUTOP is bit13 */
	u32 bit = BIT(8 + cl * 5);
	u32 con = mt6755_top_con[cl];
	u32 l2  = mt6755_l2_pdn[cl];

	if ((spm_rd(SPM_PWR_STATUS) & bit) &&
	    (spm_rd(SPM_PWR_STATUS_2ND) & bit))
		return;

	spm_wr(con, spm_rd(con) | PWR_ON | PWR_ON_2ND);
	spm_wait(SPM_PWR_STATUS, bit, true, 1000);

	spm_wr(con, spm_rd(con) & ~PWR_ISO);

	spm_wr(l2, spm_rd(l2) & ~SRAM_PDN);
	spm_wait(l2, SRAM_PDN_ACK, false, 1000);
	udelay(1);

	spm_wr(con, spm_rd(con) | SRAM_ISOINT_B);
	spm_wr(con, spm_rd(con) & ~SRAM_CKISO);
	spm_wr(con, spm_rd(con) & ~PWR_CLK_DIS);
	spm_wr(con, spm_rd(con) | PWR_RST_B);
}

static void mt6755_cpu_on(unsigned int cpu)
{
	u32 con = mt6755_pwr_con[cpu];
	u32 l1  = mt6755_l1_pdn[cpu];
	/* PWR_STATUS: bit8 = MP0_CPUTOP, bit9..12 = MP0_CPU0..3,
	 *             bit13 = MP1_CPUTOP, bit14..17 = MP1_CPU0..3
	 */
	u32 bit = BIT(9 + cpu + (cpu >= 4));

	spm_wr(SPM_POWERON_CONFIG_EN, SPM_REGWR_EN);
	mt6755_cluster_on(cpu >= 4);

	if (spm_rd(con) & PWR_ON) {
		/* powered but parked outside Linux: cycle it */
		spm_wr(con, spm_rd(con) | PWR_ISO | SRAM_CKISO);
		spm_wr(con, spm_rd(con) & ~SRAM_ISOINT_B);
		spm_wr(l1, spm_rd(l1) | SRAM_PDN);
		spm_wr(con, spm_rd(con) & ~PWR_RST_B);
		spm_wr(con, spm_rd(con) | PWR_CLK_DIS);
		spm_wr(con, spm_rd(con) & ~(PWR_ON | PWR_ON_2ND));
		mdelay(1);
	}

	spm_wr(con, spm_rd(con) | PWR_ON | PWR_ON_2ND);
	if (spm_wait(SPM_PWR_STATUS, bit, true, 20000)) {
		pr_err("CPU%u: SPM power-on timed out (PWR_STATUS=%08x)\n",
		       cpu, spm_rd(SPM_PWR_STATUS));
		return;
	}
	spm_wait(SPM_PWR_STATUS_2ND, bit, true, 20000);

	spm_wr(con, spm_rd(con) & ~PWR_ISO);

	spm_wr(l1, spm_rd(l1) & ~SRAM_PDN);
	spm_wait(l1, SRAM_PDN_ACK, false, 20000);
	udelay(1);

	spm_wr(con, spm_rd(con) | SRAM_ISOINT_B);
	spm_wr(con, spm_rd(con) & ~SRAM_CKISO);
	spm_wr(con, spm_rd(con) & ~PWR_CLK_DIS);
	spm_wr(con, spm_rd(con) | PWR_RST_B);
}

static void mt6755_secondary_boot(unsigned int cpu)
{
	if (cpu == 0 || cpu > 7)
		return;

	if (!of_machine_is_compatible("mediatek,mt6755"))
		return;

	if (!mt6755_spm) {
		mt6755_spm = ioremap(MT6755_SPM_BASE, MT6755_SPM_SIZE);
		if (!mt6755_spm) {
			pr_err("cannot map SPM, CPU%u stays offline\n", cpu);
			return;
		}
	}

	mt6755_cpu_on(cpu);
}

static int cpu_psci_cpu_boot(unsigned int cpu)
{
	phys_addr_t pa_secondary_entry = __pa_symbol(secondary_entry);
	int err = psci_ops.cpu_on(cpu_logical_map(cpu), pa_secondary_entry);
	if (err && err != -EPERM)
		pr_err("failed to boot CPU%d (%d)\n", cpu, err);

	/*
	 * PSCI returned success, so ATF has the entry point; what it did not do
	 * is release the core from reset.  Do that now, while __cpu_up() is
	 * still waiting for cpu_running.
	 */
	if (!err)
		mt6755_secondary_boot(cpu);

	return err;
}

#ifdef CONFIG_HOTPLUG_CPU
static bool cpu_psci_cpu_can_disable(unsigned int cpu)
{
	return !psci_tos_resident_on(cpu);
}

static int cpu_psci_cpu_disable(unsigned int cpu)
{
	/* Fail early if we don't have CPU_OFF support */
	if (!psci_ops.cpu_off)
		return -EOPNOTSUPP;

	/* Trusted OS will deny CPU_OFF */
	if (psci_tos_resident_on(cpu))
		return -EPERM;

	return 0;
}

static void cpu_psci_cpu_die(unsigned int cpu)
{
	/*
	 * There are no known implementations of PSCI actually using the
	 * power state field, pass a sensible default for now.
	 */
	u32 state = PSCI_POWER_STATE_TYPE_POWER_DOWN <<
		    PSCI_0_2_POWER_STATE_TYPE_SHIFT;

	psci_ops.cpu_off(state);
}

static int cpu_psci_cpu_kill(unsigned int cpu)
{
	int err;
	unsigned long start, end;

	if (!psci_ops.affinity_info)
		return 0;
	/*
	 * cpu_kill could race with cpu_die and we can
	 * potentially end up declaring this cpu undead
	 * while it is dying. So, try again a few times.
	 */

	start = jiffies;
	end = start + msecs_to_jiffies(100);
	do {
		err = psci_ops.affinity_info(cpu_logical_map(cpu), 0);
		if (err == PSCI_0_2_AFFINITY_LEVEL_OFF) {
			pr_info("CPU%d killed (polled %d ms)\n", cpu,
				jiffies_to_msecs(jiffies - start));
			return 0;
		}

		usleep_range(100, 1000);
	} while (time_before(jiffies, end));

	pr_warn("CPU%d may not have shut down cleanly (AFFINITY_INFO reports %d)\n",
			cpu, err);
	return -ETIMEDOUT;
}
#endif

const struct cpu_operations cpu_psci_ops = {
	.name		= "psci",
	.cpu_init	= cpu_psci_cpu_init,
	.cpu_prepare	= cpu_psci_cpu_prepare,
	.cpu_boot	= cpu_psci_cpu_boot,
#ifdef CONFIG_HOTPLUG_CPU
	.cpu_can_disable = cpu_psci_cpu_can_disable,
	.cpu_disable	= cpu_psci_cpu_disable,
	.cpu_die	= cpu_psci_cpu_die,
	.cpu_kill	= cpu_psci_cpu_kill,
#endif
};

