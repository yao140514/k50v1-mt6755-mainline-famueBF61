// SPDX-License-Identifier: GPL-2.0
/*
 * lkfb - reuse the framebuffer that the MediaTek LK bootloader left running.
 *
 * On this board (MT6755 / BF61) LK initialises the DSI panel, starts the video
 * stream and keeps the display engine scanning out of a fixed buffer at
 * LKFB_ADDR.  Nothing in the mainline kernel touches the display controller
 * yet, so the panel keeps showing whatever lives in that buffer.
 *
 * Describing the buffer through the device tree turned out to make LK itself
 * panic before it printed anything, so instead this driver hard-codes the
 * region LK reports ("FB base = 0x7fa50000, FB size = 5439488") and registers
 * it as a plain fbdev.  fbcon then binds to it and the kernel log shows up on
 * the panel.
 *
 * The buffer must never be handed to the page allocator: it is reserved with
 * memblock before anything else can claim it, and the DSI keeps reading it.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fb.h>
#include <linux/io.h>
#include <linux/memblock.h>
#include <linux/ioport.h>
#include <linux/string.h>

#define LKFB_NAME	"lkfb"

/* Values printed by LK: "FB base = 0x7fa50000, FB size = 5439488" */
#define LKFB_ADDR	0x7fa50000UL
#define LKFB_SIZE	0x530000UL

#define LKFB_WIDTH	480
#define LKFB_HEIGHT	800
#define LKFB_BPP	32
#define LKFB_STRIDE	(LKFB_WIDTH * (LKFB_BPP / 8))

static struct fb_info *lkfb_info;
static void __iomem *lkfb_screen;

static const struct fb_fix_screeninfo lkfb_fix = {
	.id		= LKFB_NAME,
	.type		= FB_TYPE_PACKED_PIXELS,
	.visual		= FB_VISUAL_TRUECOLOR,
	.xpanstep	= 0,
	.ypanstep	= 0,
	.ywrapstep	= 0,
	.accel		= FB_ACCEL_NONE,
};

static const struct fb_var_screeninfo lkfb_var = {
	.xres		= LKFB_WIDTH,
	.yres		= LKFB_HEIGHT,
	.xres_virtual	= LKFB_WIDTH,
	.yres_virtual	= LKFB_HEIGHT,
	.xoffset	= 0,
	.yoffset	= 0,
	.bits_per_pixel	= LKFB_BPP,
	.transp		= { 24, 8, 0 },
	.red		= { 16, 8, 0 },
	.green		= { 8, 8, 0 },
	.blue		= { 0, 8, 0 },
	.vmode		= FB_VMODE_NONINTERLACED,
};

static const struct fb_ops lkfb_ops = {
	.owner		= THIS_MODULE,
	.fb_fillrect	= cfb_fillrect,
	.fb_copyarea	= cfb_copyarea,
	.fb_imageblit	= cfb_imageblit,
};

static int __init lkfb_init(void)
{
	struct fb_info *info;
	int ret;

	/*
	 * memblock_reserve() returns 0 on success and -errno on failure --
	 * the opposite of what the first version of this driver assumed.
	 * Reserving an already-reserved range is not an error either (the
	 * ranges are simply merged), so a single call is enough.
	 */
	if (memblock_reserve(LKFB_ADDR, LKFB_SIZE)) {
		pr_err(LKFB_NAME ": cannot reserve %#lx-%#lx\n",
		       LKFB_ADDR, LKFB_ADDR + LKFB_SIZE - 1);
		return -EBUSY;
	}

	/*
	 * Not fatal: nothing else claims this memory, and even without an
	 * iomem resource the mapping below is what actually matters.
	 */
	if (!request_mem_region(LKFB_ADDR, LKFB_SIZE, LKFB_NAME))
		pr_warn(LKFB_NAME ": iomem region busy, continuing anyway\n");

	lkfb_screen = ioremap_wc(LKFB_ADDR, LKFB_SIZE);
	if (!lkfb_screen) {
		pr_err(LKFB_NAME ": ioremap failed\n");
		ret = -ENOMEM;
		goto err_region;
	}

	info = framebuffer_alloc(0, NULL);
	if (!info) {
		ret = -ENOMEM;
		goto err_unmap;
	}

	info->fix = lkfb_fix;
	info->fix.smem_start = LKFB_ADDR;
	info->fix.smem_len = LKFB_SIZE;
	info->fix.line_length = LKFB_STRIDE;

	info->var = lkfb_var;

	info->screen_base = (char __iomem *)lkfb_screen;
	info->screen_size = LKFB_SIZE;
	info->fbops = &lkfb_ops;
	info->flags = 0;

	ret = fb_alloc_cmap(&info->cmap, 256, 0);
	if (ret)
		goto err_free;

	ret = register_framebuffer(info);
	if (ret) {
		pr_err(LKFB_NAME ": register_framebuffer failed: %d\n", ret);
		goto err_cmap;
	}

	lkfb_info = info;
	pr_info(LKFB_NAME ": %ux%u@%ubpp at %#lx, stride %u (LK framebuffer)\n",
		LKFB_WIDTH, LKFB_HEIGHT, LKFB_BPP, LKFB_ADDR, LKFB_STRIDE);
	return 0;

err_cmap:
	fb_dealloc_cmap(&info->cmap);
err_free:
	framebuffer_release(info);
err_unmap:
	iounmap(lkfb_screen);
	lkfb_screen = NULL;
err_region:
	release_mem_region(LKFB_ADDR, LKFB_SIZE);
	return ret;
}
device_initcall(lkfb_init);

MODULE_DESCRIPTION("Framebuffer handed over by the MTK LK bootloader");
MODULE_LICENSE("GPL");
