# Kernel config notes

`kernel/config-6.12.115` in this repo is the **r56-era** snapshot. Since then
these options changed; apply them on top of an r56 config, or start from
`make ARCH=arm64 defconfig` plus the list below.

```
CONFIG_LOCALVERSION="-mt6755-mainline"
CONFIG_SMP=y
CONFIG_HOTPLUG_CPU=y
CONFIG_MAGIC_SYSRQ=y
CONFIG_WATCHDOG=y
CONFIG_MEDIATEK_WATCHDOG=y
CONFIG_FB=y
CONFIG_FB_SIMPLE=y
CONFIG_DUMMY_CONSOLE=y
CONFIG_FRAMEBUFFER_CONSOLE=y
CONFIG_FRAMEBUFFER_CONSOLE_DETECT_PRIMARY=y
```

Why each one:

- `LOCALVERSION` -> `uname -r` reads `6.12.115-mt6755-mainline`, so a flashed
  board can be identified without guessing.
- `MEDIATEK_WATCHDOG` -> `mtk_wdt` binds `10007000.watchdog` (DTS node
  `mediatek,mt6589-wdt`) and provides the restart handler. Note it is
  registered at priority 128 while PSCI's `SYSTEM_RESET` is 129, so PSCI runs
  first -- on this board ATF's reset never returns, which is why `/sbin/reboot`
  writes TOPRGU (`WDT_SWRQ`) from userspace instead.
- `FB` / `FB_SIMPLE` / `FRAMEBUFFER_CONSOLE` -> `simplefb` adopts LK's
  framebuffer through `memory-region` and registers `fb0`.
- `HOTPLUG_CPU` -> lets a secondary be retried at runtime while debugging.

## cmdline

```
bootopt=64S3,32N2,64N2 maxcpus=8 console=ttyS0,921600n8 console=tty0 \
earlycon loglevel=7 ignore_loglevel
```

`console=ttyS0` comes first so `/dev/console` stays on the serial port (the
init shell depends on it); `tty0` is only there so the framebuffer console
also gets the log once a display driver exists.
