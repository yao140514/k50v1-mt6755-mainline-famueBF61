// SPDX-License-Identifier: GPL-2.0
/*
 * Reuse the display pipeline that the MediaTek LK bootloader left running.
 *
 * LK programs MMSYS/DSI, starts the video stream and points OVL0 at a fixed
 * framebuffer; simplefb claims that buffer and fbcon draws the console into
 * it.  Two pieces of LK's overlay configuration have to be corrected:
 *
 *   l0con  = 0x21ff -> AEN = 1, constant alpha 0xff (fbcon pixels otherwise
 *            end up transparent and the console never shows up)
 *   srccon = 0x9    -> 0x1          (L3 still shows LK's own buffer, which is
 *            why the unlock warning survived a full L0 wipe)
 *
 * The top 80 pixel rows of the framebuffer are reserved as a permanent
 * status bar: the simplefb node starts 80 rows into the same memory, so
 * fbcon can never touch them, no matter how far the console scrolls.  The
 * bar is drawn here (postcore, before fbcon) and refreshed once a second:
 *
 *   [ tux ][ tux ][ tux ]  mtk6755 mainline Linux
 *                          6.12.115 by yao140514
 *                          [bat] 3.82V 53% Chg  ######
 *                          21:03:44   bld Nov 22 2026 20:50
 *
 * The penguins come from the kernel's own CLUT224 boot logo, the glyphs
 * from font_vga_8x16.  The logo data is __initconst, so the penguins are
 * blitted once during init; the per-second refresh only rewrites the
 * dynamic columns (battery + clock).  Delete once a real DRM/KMS driver
 * owns the display.
 */

#include <linux/font.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/linux_logo.h>
#include <linux/power_supply.h>
#include <linux/time64.h>
#include <linux/workqueue.h>

#include "mt6755_buildstamp.h"

#define DISP_OVL0_BASE	0x14008000

#define LKFB_ADDR	0x7fa50000UL
#define LKFB_WIDTH	480
#define LKFB_HEIGHT	800

#define BAR_ROWS	80	/* reserved top rows, console starts below */
#define BAR_TXT_X	250

#define OVL_SRC_CON	0x02c
#define OVL_L0_CON	0x030
#define OVL_L0_ADDR	0xf40

#define L_CON_AEN	(1u << 8)	/* alpha blending enable */
#define L_CON_APHA	0xffu		/* constant alpha */

static void __iomem *fb;
static void bar_refresh(struct work_struct *w);
static DECLARE_DELAYED_WORK(bar_work, bar_refresh);

static void bar_pixel(int x, int y, u32 c)
{
	if (x < 0 || x >= LKFB_WIDTH || y < 0 || y >= LKFB_HEIGHT)
		return;
	writel(c, fb + (y * LKFB_WIDTH + x) * 4);
}

static void bar_fill(int x, int y, int w, int h, u32 c)
{
	int i, j;

	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++)
			bar_pixel(x + i, y + j, c);
}

static void bar_text(int x, int y, const char *s, u32 fg)
{
	const unsigned char *glyphs = font_vga_8x16.data;

	for (; *s; s++, x += 8) {
		const unsigned char *g = glyphs + (unsigned char)(*s) * 16;
		int row, col;

		for (row = 0; row < 16; row++) {
			unsigned char bits = g[row];

			for (col = 0; col < 8; col++)
				if (bits & (0x80 >> col))
					bar_pixel(x + col, y + row, fg);
		}
	}
}

/* blit one 80x80 CLUT224 logo into the bar (init time only) */
static void bar_penguin(int x)
{
	const struct linux_logo *logo = &logo_linux_clut224;
	int px, py;

	for (py = 0; py < logo->height; py++) {
		for (px = 0; px < logo->width; px++) {
			u8 idx = logo->data[py * logo->width + px];
			const u8 *rgb = logo->clut + idx * 3;

			bar_pixel(x + px, py,
				  0xff000000u | (rgb[0] << 16) |
				  (rgb[1] << 8) | rgb[2]);
		}
	}
}

/* battery line + clock line: everything that changes over time */
static void bar_dynamic(bool battery_ok)
{
	char buf[64];
	int mv = -1, cap = -1;
	bool charging = false;

	/* battery: 3.82V 53% Chg */
	if (battery_ok) {
		struct power_supply *psy = power_supply_get_by_name("battery");
		union power_supply_propval p;

		if (psy) {
			if (power_supply_get_property(psy,
			    POWER_SUPPLY_PROP_STATUS, &p) == 0)
				charging = p.intval ==
					   POWER_SUPPLY_STATUS_CHARGING;
			if (power_supply_get_property(psy,
			    POWER_SUPPLY_PROP_VOLTAGE_NOW, &p) == 0)
				mv = p.intval / 1000;
			if (power_supply_get_property(psy,
			    POWER_SUPPLY_PROP_CAPACITY, &p) == 0)
				cap = p.intval;
			power_supply_put(psy);
		}
	}

	bar_fill(BAR_TXT_X, 44, LKFB_WIDTH - BAR_TXT_X, 16, 0xff000000u);
	if (mv > 0)
		snprintf(buf, sizeof(buf), "%d.%02dV %d%% %s",
			 mv / 1000, (mv % 1000) / 10, (int)cap,
			 charging ? "Chg" : "Dis");
	else
		snprintf(buf, sizeof(buf), "bat: waiting for driver");
	bar_text(BAR_TXT_X, 44, buf,
		 charging ? 0xff00ff00u : 0xffffffffu);

	/* small rainbow */
	{
		static const u32 rainbow[] = {
			0xffff0000u, 0xffff7f00u, 0xffffff00u, 0xff00ff00u,
			0xff00ffffu, 0xff0000ffu, 0xffff00ffu,
		};
		int i, sx = BAR_TXT_X + 160;

		for (i = 0; i < 7; i++)
			bar_fill(sx + i * 12, 46, 10, 12, rainbow[i]);
	}

	/* live clock (system time, CST) + build stamp */
	bar_fill(BAR_TXT_X, 62, LKFB_WIDTH - BAR_TXT_X, 18, 0xff000000u);
	{
		time64_t now = ktime_get_real_seconds() + 8 * 3600;
		struct tm tm;

		time64_to_tm(now, 0, &tm);
		snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
			 tm.tm_hour, tm.tm_min, tm.tm_sec);
		bar_text(BAR_TXT_X, 62, buf, 0xffffff00u);
	}
	{
		time64_t bld = BUILD_EPOCH + 8 * 3600;
		struct tm bt;

		time64_to_tm(bld, 0, &bt);
		snprintf(buf, sizeof(buf),
			 "bld %04d-%02d-%02d %02d:%02d",
			 bt.tm_year + 1900, bt.tm_mon + 1, bt.tm_mday,
			 bt.tm_hour, bt.tm_min);
		bar_text(BAR_TXT_X + 8 * 9 + 8, 62, buf, 0xff808080u);
	}
}

static void bar_refresh(struct work_struct *w)
{
	bar_dynamic(true);
	schedule_delayed_work(&bar_work, msecs_to_jiffies(1000));
}

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
	 * left in the rest of the buffer stays visible underneath the log.
	 * Wipe the whole buffer to opaque black once, then paint the status
	 * bar into the top rows -- the console (simplefb starts 80 rows down)
	 * can never overwrite them, so the bar is truly resident.
	 */
	fb = ioremap_wc(LKFB_ADDR, LKFB_WIDTH * LKFB_HEIGHT * 4);
	if (fb) {
		u32 *p = (u32 *)fb;
		int i;

		for (i = 0; i < LKFB_WIDTH * LKFB_HEIGHT; i++)
			p[i] = 0xff000000;

		/* static content: penguins + version lines (init data only) */
#ifndef CONFIG_LOGO_LINUX_CLUT224
		pr_info("mt6755-disp: no CLUT224 logo in kernel\n");
#else
		bar_penguin(0);
		bar_penguin(80);
		bar_penguin(160);
#endif
		bar_text(BAR_TXT_X, 6, "mtk6755 mainline Linux", 0xffffffffu);
		bar_text(BAR_TXT_X, 26, "6.12.115 by yao140514", 0xffffffffu);
		bar_dynamic(false);

		pr_info("mt6755-disp: status bar drawn, console starts at row %d\n",
			BAR_ROWS);

		schedule_delayed_work(&bar_work, msecs_to_jiffies(2000));
	} else {
		pr_info("mt6755-disp: framebuffer wipe skipped (no map)\n");
	}

	return 0;
}
postcore_initcall(mt6755_disp_fixup);
