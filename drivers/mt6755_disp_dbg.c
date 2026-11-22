// SPDX-License-Identifier: GPL-2.0
/*
 * Reuse the display pipeline that the MediaTek LK bootloader left running.
 *
 * LK programs MMSYS/DSI, starts the video stream and points OVL0 at a fixed
 * framebuffer; simplefb claims that buffer and fbcon draws the console into
 * it.  One piece of LK's overlay configuration has to be corrected though:
 *
 *   l0con = 0x21ff   ->   AEN = 1, constant alpha = 0xff
 *
 * With AEN set the overlay alpha-blends the layer.  fbcon writes x8r8g8b8
 * pixels whose top byte is zero, so every pixel it draws ends up fully
 * transparent and the panel keeps showing whatever LK left behind -- the
 * console clears the screen but no text ever appears.  Clearing AEN makes
 * the layer opaque and the console shows up.
 *
 * Runs at postcore_initcall so the fix is in effect before fbcon paints
 * anything.  Delete once a real DRM/KMS driver owns the display.
 */

#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>

#define DISP_OVL0_BASE	0x14008000

#define LKFB_ADDR	0x7fa50000UL
#define LKFB_WIDTH	480
#define LKFB_HEIGHT	800

#define OVL_SRC_CON	0x02c
#define OVL_L0_CON	0x030
#define OVL_L0_ADDR	0xf40

#define L_CON_AEN	(1u << 8)	/* alpha blending enable */
#define L_CON_APHA	0xffu		/* constant alpha */

static int __init mt6755_disp_fixup(void)
{
	void __iomem *ovl;
	u32 con, addr, new, src;

	ovl = ioremap(DISP_OVL0_BASE, 0x2000);
	if (!ovl) {
		pr_err("mt6755-disp: cannot map OVL0\n");
		return 0;
	}

	con = readl(ovl + OVL_L0_CON);
	addr = readl(ovl + OVL_L0_ADDR);
	new = (con & ~(L_CON_AEN | L_CON_APHA)) | L_CON_APHA;
	writel(new, ovl + OVL_L0_CON);

	/*
	 * LK leaves more than one overlay layer enabled (src_con = 0x9 means
	 * L0 plus L3).  The extra layer still shows whatever LK last put in
	 * its own buffer, which is why "Your device has been unlocked"
	 * survived a full wipe of L0.  We only need L0: the console lives
	 * there.  Keep bit 0, drop the rest.
	 */
	src = readl(ovl + OVL_SRC_CON);
	if (src & ~1u) {
		writel(src & 1u, ovl + OVL_SRC_CON);
		pr_info("mt6755-disp: ovl srccon %#x -> %#x (only L0)\n",
			src, readl(ovl + OVL_SRC_CON));
	}

	pr_info("mt6755-disp: OVL0 srccon=%#x l0addr=%#x l0con %#x -> %#x\n",
		readl(ovl + OVL_SRC_CON), addr, con,
		readl(ovl + OVL_L0_CON));

	iounmap(ovl);

	/*
	 * fbcon only paints the character cells it outputs, so whatever LK
	 * left in the rest of the buffer stays visible underneath the log
	 * ("Your device has been unlocked..." bleeding through).  Wipe the
	 * whole buffer to opaque black once, before fbcon takes over.  The
	 * alpha byte matters: left at zero those pixels are transparent.
	 */
	{
		void __iomem *fb;
		u32 *p;
		int i;

		fb = ioremap_wc(LKFB_ADDR, LKFB_WIDTH * LKFB_HEIGHT * 4);
		if (fb) {
			p = (u32 *)fb;
			for (i = 0; i < LKFB_WIDTH * LKFB_HEIGHT; i++)
				p[i] = 0xff000000;
			iounmap(fb);
			pr_info("mt6755-disp: framebuffer wiped to opaque black\n");
		}
	}

	return 0;
}
postcore_initcall(mt6755_disp_fixup);
