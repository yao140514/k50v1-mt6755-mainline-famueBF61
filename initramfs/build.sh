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

cd "$ROOT"
find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -9 > "$OUT"

echo "init:    $(stat -c%s "$ROOT/init") bytes"
[ -x "$BUSYBOX" ] && echo "busybox: $(stat -c%s "$ROOT/bin/busybox") bytes"
echo "cpio.gz: $(stat -c%s "$OUT") bytes"
