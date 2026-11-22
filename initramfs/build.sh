#!/bin/bash
# Build a minimal static initramfs for mainline bring-up.
set -e

PROJ=/mnt/f/BF61/mainline
SRC=$PROJ/initramfs
ROOT=$SRC/rootfs
OUT=$PROJ/out/initramfs.cpio.gz
BUSYBOX=${BUSYBOX:-$HOME/mt6755/busybox-1.37.0/busybox}

rm -rf "$ROOT"
# One mkdir per directory: /bin/sh is dash here and has no brace expansion.
for d in dev proc sys tmp bin sbin etc mnt root; do
	mkdir -p "$ROOT/$d"
done

# Bake the build time into the binary: there is no RTC driver on this board,
# and apt/TLS validation needs a clock that is at least close to real time.
printf '#define BUILD_EPOCH %sULL\n' "$(date +%s)" > "$SRC/build_time.h"

aarch64-linux-gnu-gcc -static -O2 -I"$SRC" -o "$ROOT/init" "$SRC/init.c"
chmod 755 "$ROOT/init"

# One shell shared by serial, telnet and the panel.
aarch64-linux-gnu-gcc -static -O2 -o "$ROOT/bin/consolehub" "$SRC/consolehub.c"
chmod 755 "$ROOT/bin/consolehub"
aarch64-linux-gnu-gcc -static -O2 -o "$ROOT/bin/fastfetch" "$SRC/fastfetch.c"
chmod 755 "$ROOT/bin/fastfetch"

# Static busybox gives the console a real shell (ash) plus ~400 applets.
if [ -x "$BUSYBOX" ]; then
	cp "$BUSYBOX" "$ROOT/bin/busybox"
	chmod 755 "$ROOT/bin/busybox"
	ln -sf busybox "$ROOT/bin/sh"
else
	echo "WARNING: busybox not found at $BUSYBOX -- image will have no shell"
fi

# Runtime SMP bring-up helper (powers on CPU MTCMOS via SPM, then hotplugs).
if [ -f "$PROJ/../repo/scripts/smp_on.sh" ]; then
	cp "$PROJ/../repo/scripts/smp_on.sh" "$ROOT/smp_on.sh"
	chmod 755 "$ROOT/smp_on.sh"
fi

# charging mode helper (sysfs front-end of the mt6353 driver)
cat > "$ROOT/bin/chgmode" <<'CHGEOF'
#!/bin/sh
# charging mode: cpu0 only, backlight off.
# exit: hold the power key ~2 s, or run: chgmode 0
case "$1" in
0|off|exit)
	echo 0 > /sys/kernel/mt6353/chgmode
	echo "charging mode OFF (cpus online, backlight on)"
	;;
*)
	echo 1 > /sys/kernel/mt6353/chgmode
	echo "charging mode ON: cpu0 only, backlight off"
	echo "exit: hold power key ~2 s, or run: chgmode 0"
	;;
esac
CHGEOF
chmod 755 "$ROOT/bin/chgmode"

cd "$ROOT"
find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -9 > "$OUT"

echo "init:    $(stat -c%s "$ROOT/init") bytes"
[ -x "$BUSYBOX" ] && echo "busybox: $(stat -c%s "$ROOT/bin/busybox") bytes"
echo "cpio.gz: $(stat -c%s "$OUT") bytes"
