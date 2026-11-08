# k50v1 MT6755 mainline Linux

国产白牌 10 寸平板（板号 `k50v1_64_bsp`，MT6750/MT6755 平台，原厂 Android 10）上运行
**mainline Linux 6.12 LTS** 的移植记录与全部源码。

原厂只提供 3.18 内核 BSP，且没有 GPL 源码；这里从零把 mainline 引导起来，
绕过 MTK 私有 boot 链、时钟框架和 PSCI 的诸多限制。

## 当前状态

| 功能 | 状态 | 说明 |
|---|---|---|
| 引导 | ✅ | 自编译 LK（`svoboda18/lk`）+ 自制 MTK 头 boot.img |
| 内核 | ✅ | 6.12.115，单核 A53，3.66 GiB 可用 |
| 串口控制台 | ✅ | CH340，921600 8N1，`ttyS0` |
| USB gadget | ✅ | `mtu3`（MTK SSUSB），CDC-ACM + CDC-NCM |
| USB 网络 | ✅ | NCM，PC 侧需静态 `192.168.137.x` 或 ICS |
| eMMC | ✅ | 29.1 GiB，31 个 GPT 分区全部识别 |
| 文件系统 | ✅ | `vendor`/`system` 只读挂载，`userdata` 可写 |
| pinctrl | ✅ | `pinctrl-mt6755.c`（190 引脚，paris 框架） |
| I2C | ⚠️ | `i2c1` 正常；`i2c0` 控制器不产生中断（已定位到硬件侧） |
| 显示 | ❌ | 缺 mmsys + DSI + panel 三层驱动；面板为 480×800 |
| 触摸 | ❌ | AXS 控制器，地址 `0x5d`，挂在 `i2c0` 上，受显示依赖 |
| SMP | ❌ | PSCI v0.2 可用但 CPU1 的 MTCMOS 电源域未上电（见 `docs/smp.md`） |

## 目录

```
dts/        设备树（mt6755-k50v1.dts）
initramfs/  自制 init（C）+ 构建脚本，含硬件普查、USB gadget、网络、chroot 帮助
kernel/     内核侧改动文件与配置（i2c / mtu3 / pinctrl / tphy + .config）
scripts/    boot.img 打包工具与各类补丁脚本
tools/      PC 侧工具（HTTP 代理，用于 apt/apk 走 PC 的公网）
docs/       LK 补丁、bring-up 记录、SMP 调研
images/     编译产物（boot.img 16 MB / lk.img 1 MB）
```

## 构建

### 内核

需要 `aarch64-linux-gnu-` 工具链和 6.12.115 源码：

```sh
cp dts/mt6755-k50v1.dts  $KERNEL/arch/arm64/boot/dts/mediatek/
cp kernel/pinctrl-mt6755.c kernel/pinctrl-mtk-mt6755.h $KERNEL/drivers/pinctrl/mediatek/
# i2c-mt65xx.c / mtu3_plat.c / phy-mtk-tphy.c 同理覆盖
cp kernel/config-6.12.115 $OUT/.config
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=$OUT -j$(nproc) Image.gz dtbs
```

`pinctrl-mt6755.c` 来自 `mtk-mainline/mt6755/linux` 的 `pinctrl-staging` 分支，
基于 paris 框架，在 6.12 上零修改可编译。

### initramfs

```sh
cd initramfs && ./build.sh          # 产出 out/initramfs.cpio.gz
```

需要预先编译好的静态 busybox（1.37.0）：
`make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- defconfig` + 打开 `CONFIG_STATIC`。

### boot.img

MTK 的 boot 头有私有扩展字段，必须逐字节对齐原厂：

```sh
python3 scripts/bootimg.py pack -o boot.img \
  --kernel Image.gz --dtb mt6755-k50v1.dtb --ramdisk initramfs.cpio.gz \
  --cmdline 'bootopt=64S3,32N2,64N2 maxcpus=1 console=ttyS0,921600n8 earlycon loglevel=7' \
  --header-version 2 --os-version 0x14000148 \
  --tags-addr 0x44000000 --dtb-addr 0 --pad-to 0x1000000
```

关键点（踩过的坑，见 `docs/bringup.md`）：
- `tags_addr` 必须等于 LK 的 mblock 分配的 DTB 地址（`0x44000000`），否则 LK 直接 panic
- 扩展头偏移 `1644=1660`、`1648=dtbo_size`、`1652=0x44000000`
- SHA1 写入 `hdr[576:596]`（20 字节），写错会 `incomplete bootimage`

### LK

`docs/lk-changes.patch` 是相对 `svoboda18/lk` 的全部改动，包括：
- DCT 脚本 Python2 → Python3
- 关掉显示初始化（`MACH_FPGA_NO_DISPLAY=y`）
- 禁用 boot menu / recovery 误判 / 看门狗
- `pmic_init()` 之后打开 VLDO28、VUSB33

## 刷写

| 分区 | 文件 | 大小 |
|---|---|---|
| `boot` | `images/boot-r56.img` | 16 MB |
| `lk` / `lk2` | `images/lk.img` | 1 MB |

用 SP Flash Tool，**逐行核对文件名与大小**——把 16 MB 的 boot 填到 lk 行会溢出覆盖
`lk2` 和 `boot`，只能用 BROM 恢复。

## 使用

串口登录后：

```
alpine              进入 Alpine 3.20（apk）
distro ubuntu       进入 Ubuntu 24.04（apt）
net                 查看 usb0 地址 / 路由 / telnetd
reboot              重启（走 sysrq，不依赖 init）
```

两套发行版解压在 `userdata`（`/mnt/data/alpine`、`/mnt/data/ubuntu`），互不干扰。

### 网络

板子不跑 DHCP 服务器时，PC 需要手工配：

```powershell
netsh interface ip set address name="以太网 5" static 192.168.137.50 255.255.255.0
```

设备侧 init 会自动探测 `192.168.137.1` / `192.168.42.2` 并选路。
若 PC 的 ICS NAT 不工作，可用 `tools/proxy.py`（PC 上跑，设备设 `http_proxy`）。

## 已知问题

1. **i2c0 控制器**：寄存器可读写，但 `START` 位写下去状态机不动、零中断，2 秒后超时。
   同一对引脚用 i2c-gpio 位操作正常，说明是控制器侧的时钟/复位问题，缺 `clk-mt6755` 驱动。
2. **SMP**：PSCI v0.2 存在，`CPU_ON` 返回 SUCCESS 但核不启动。已确认 `MP0_CPU1_PWR_STA`
   为 0（电源域关着），需在 OS 侧打开 SPM MTCMOS，见 `docs/smp.md`。
3. **RTC**：无 MT6353 驱动，开机时间用编译时间戳播种。

## 许可

GPL-2.0。`kernel/` 下的文件派生自 Linux 内核（GPL-2.0），
`docs/lk-changes.patch` 针对 BSD 许可的 LK。
