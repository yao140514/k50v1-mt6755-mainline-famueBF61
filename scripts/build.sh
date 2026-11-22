#!/bin/bash
# Build the MT6755 minimal bring-up kernel inside WSL.
set -euo pipefail

SRC="${HOME}/mt6755/linux-6.12.115"
OUT="${HOME}/mt6755/out"
PROJ="/mnt/f/BF61/mainline"
CROSS="aarch64-linux-gnu-"

step() { printf '\n\033[1m== %s ==\033[0m\n' "$*"; }

step "0. source tree"
if [ ! -d "$SRC" ]; then
	cd "${HOME}/mt6755"
	tar xf linux-6.12.115.tar.xz
fi
cd "$SRC"
echo "kernel version: $(make -s kernelversion 2>/dev/null || echo unknown)"

step "1. install board dts"
cp "$PROJ/dts/mt6755-k50v1.dts" arch/arm64/boot/dts/mediatek/
MK=arch/arm64/boot/dts/mediatek/Makefile
if ! grep -q "mt6755-k50v1.dtb" "$MK"; then
	sed -i '/mt6755-evb.dtb/a dtb-$(CONFIG_ARCH_MEDIATEK) += mt6755-k50v1.dtb' "$MK"
fi
# LK merges the odmdtbo overlay into the main DTB and needs __symbols__
# for phandle resolution. Without -@ the merge corrupts the tree and LK
# asserts in app/mt_boot/bootargs.c.
if ! grep -q "DTC_FLAGS_mt6755-k50v1" "$MK"; then
	echo 'DTC_FLAGS_mt6755-k50v1 := -@' >> "$MK"
fi
grep -n "mt6755" "$MK"

step "1b. install lkfb (bootloader framebuffer) driver"
cp "$PROJ/drivers/lkfb.c" drivers/video/fbdev/lkfb.c
MKFB=drivers/video/fbdev/Makefile
grep -q "lkfb.o" "$MKFB" || echo 'obj-$(CONFIG_FB_LKFB) += lkfb.o' >> "$MKFB"
printf '#define BUILD_EPOCH %sULL
' "$(date +%s)" > drivers/mt6755_buildstamp.h
cp "$PROJ/drivers/mt6755_disp_dbg.c" drivers/mt6755_disp_dbg.c
cp "$PROJ/drivers/mt6755_i2c_scan.c" drivers/mt6755_i2c_scan.c
cp "$PROJ/drivers/mt6755_charger_probe.c" drivers/mt6755_charger_probe.c
cp "$PROJ/drivers/mt6755_pmic.c" drivers/mt6755_pmic.c
cp "$PROJ/drivers/input/axs15260.c" drivers/input/touchscreen/axs15260.c
MKI2C=drivers/input/touchscreen/Makefile
grep -q "axs15260.o" "$MKI2C" || echo 'obj-$(CONFIG_TOUCHSCREEN_AXS15260) += axs15260.o' >> "$MKI2C"
KI2C=drivers/input/touchscreen/Kconfig
if ! grep -q "config TOUCHSCREEN_AXS15260" "$KI2C"; then
	cat >> "$KI2C" <<'EOF'

config TOUCHSCREEN_AXS15260
	tristate "AXS15260 touchscreen (bring-up probe)"
	depends on I2C
	help
	  Probe driver for the AXS15260 panel touch controller.
EOF
fi

MKDBG=drivers/Makefile
grep -q "mt6755_disp_dbg" "$MKDBG" || echo 'obj-y += mt6755_disp_dbg.o' >> "$MKDBG"
grep -q "mt6755_i2c_scan" "$MKDBG" || echo 'obj-y += mt6755_i2c_scan.o' >> "$MKDBG"
grep -q "mt6755_charger_probe" "$MKDBG" || echo 'obj-y += mt6755_charger_probe.o' >> "$MKDBG"
grep -q "mt6755_pmic" "$MKDBG" || echo 'obj-y += mt6755_pmic.o' >> "$MKDBG"

KFB=drivers/video/fbdev/Kconfig
if ! grep -q "config FB_LKFB" "$KFB"; then
	cat >> "$KFB" <<'EOF'

config FB_LKFB
	bool "Framebuffer handed over by the MTK LK bootloader"
	depends on FB
	help
	  Reuse the framebuffer LK left running (MT6755 bring-up).  The region is
	  reserved with memblock and registered as a plain fbdev so fbcon can use
	  the panel without touching the display controller.
EOF
fi

step "2. configure"
#
# Start from the lean bring-up config, not from arm64 defconfig.  defconfig
# builds ~3200 drivers in and produces a 46 MB Image (14.7 MB gzipped); the
# boot image then no longer fits the 16 MB partition together with the
# ramdisk, and LK halts before it prints anything.  The Nov-8 images that
# booted were built from repo/kernel/config-6.12.115 (1436 built-ins,
# gzipped Image ~7.4 MB).
LEAN="$PROJ/../repo/kernel/config-6.12.115"
if [ -f "$LEAN" ]; then
	cp "$LEAN" "$OUT/.config"
	make ARCH=arm64 CROSS_COMPILE="$CROSS" O="$OUT" olddefconfig
else
	make ARCH=arm64 CROSS_COMPILE="$CROSS" O="$OUT" defconfig
fi

scripts/config --file "$OUT/.config" \
	-e ARCH_MEDIATEK \
	-e SERIAL_8250 \
	-e SERIAL_8250_CONSOLE \
	-e SERIAL_8250_MT6577 \
	-e EARLY_PRINTK \
	-e PRINTK \
	-e FB \
	-e FB_SIMPLE \
	-e TOUCHSCREEN_AXS15260 \
	-d FB_LKFB \
	-e FRAMEBUFFER_CONSOLE \
	-e LOGO \
	-e IKCONFIG \
	-e IKCONFIG_PROC \
	-d DEBUG_INFO \
	-d DEBUG_INFO_DWARF5 \
	-d DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT

make ARCH=arm64 CROSS_COMPILE="$CROSS" O="$OUT" olddefconfig

step "3. sanity check config"
for opt in ARCH_MEDIATEK SERIAL_8250_CONSOLE SERIAL_8250_MT6577; do
	if grep -q "^CONFIG_${opt}=y" "$OUT/.config"; then
		echo "  ok   CONFIG_${opt}"
	else
		echo "  MISSING CONFIG_${opt}"
	fi
done

step "4. build"
time make ARCH=arm64 CROSS_COMPILE="$CROSS" O="$OUT" -j"$(nproc)" Image.gz dtbs

step "5. collect artifacts"
mkdir -p "$PROJ/out"
cp "$OUT/arch/arm64/boot/Image.gz" "$PROJ/out/"
cp "$OUT/arch/arm64/boot/dts/mediatek/mt6755-k50v1.dtb" "$PROJ/out/"
cp "$OUT/.config" "$PROJ/out/config-6.12.115"
ls -lh "$PROJ/out/"
